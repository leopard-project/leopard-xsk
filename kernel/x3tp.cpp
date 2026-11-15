/*
 * Input Core v2 — seat/endpoint routing without a router thread.
 *
 * Layout
 * ------
 *   g_input_sources[3]   per-source SPSC rings, one producer each:
 *                        PS/2 IRQ, xHCI worker, /dev/vkbd writer.
 *   g_input_seats[8]     seat -> owning endpoint + source mask.
 *   g_input_endpoints[8] static endpoints (endpoint0 -> /dev/console).
 *   g_input_routes[3]    source -> {seat, endpoint}, constant while running.
 *
 * The IRQ path is x3tp_route_irq(): a plain table lookup, one SPSC publish and
 * a single atomic "notify" exchange per endpoint. It takes no lock, allocates
 * nothing and never wakes a router thread. A queue-full source simply drops.
 *
 * Consumer side (x3tp_console_read / poll / legacy drain) takes one consumer
 * lock so two readers cannot race the same SPSC tail. That lock never appears
 * on the producer path.
 *
 * Multiseat growth: fill the seat/endpoint tables and call x3tp_route_bind()
 * during seat setup. x3tp_route_irq() does not change.
 */
#include <cpu/lock.h>
#include <errno.h>
#include <ime_utf8.h>
#include <x3tp.h>
#include <krlibc.h>
#include <mm/uaccess.h>
#include <task/pcb.h>
#include <task/poll.h>

static x3tp_source_t   g_input_sources[X3TP_SOURCE_MAX];
static x3tp_seat_t     g_input_seats[X3TP_SEAT_MAX];
static x3tp_endpoint_t g_input_endpoints[X3TP_ENDPOINT_MAX];
static x3tp_route_t    g_input_routes[X3TP_SOURCE_MAX];
static x3tp_terminal_t g_default_terminal;

/* Serialises consumers only (never producers). */
static spin_t g_input_consume_lock = SPIN_INIT;

static x3tp_endpoint_t *input_endpoint_lookup(uint8_t endpoint)
{
    if (endpoint >= X3TP_ENDPOINT_MAX) return NULL;
    return &g_input_endpoints[endpoint];
}

static bool input_source_has_data(const x3tp_source_t *queue)
{
    uint32_t tail = queue->tail;
    return tail != __atomic_load_n(&queue->head, __ATOMIC_ACQUIRE);
}

/* Does this endpoint's seat still have queued events? Read-only, lock-free. */
static bool input_endpoint_has_events(const x3tp_endpoint_t *endpoint)
{
    if (endpoint->seat >= X3TP_SEAT_MAX) return false;

    uint8_t mask = g_input_seats[endpoint->seat].source_mask;
    for (uint32_t source = 0; source < X3TP_SOURCE_MAX; ++source)
    {
        if (!(mask & (1u << source))) continue;
        if (input_source_has_data(&g_input_sources[source])) return true;
    }
    return false;
}

/* Producer-side wake: only called when this endpoint had no queued event
   before the current push (notify 0 -> 1). With no bound consumer the hook is
   NULL, which is the normal v1 state: /dev/console readers poll. */
static void input_endpoint_kick(x3tp_endpoint_t *endpoint)
{
    __atomic_add_fetch(&endpoint->wakes, 1, __ATOMIC_RELAXED);
    x3tp_endpoint_wake_fn wake = endpoint->wake;
    if (wake != NULL) wake(endpoint);
}

void x3tp_endpoint_open(uint8_t endpoint)
{
    x3tp_endpoint_t *ep = input_endpoint_lookup(endpoint);
    if (ep == NULL) return;

    spin_lock(&g_input_consume_lock);
    ep->closed = false;
    ep->active = true;
    ep->notify = 0;
    spin_unlock(&g_input_consume_lock);
}

void x3tp_endpoint_close(uint8_t endpoint)
{
    x3tp_endpoint_t *ep = input_endpoint_lookup(endpoint);
    if (ep == NULL) return;

    spin_lock(&g_input_consume_lock);
    ep->closed = true;
    spin_unlock(&g_input_consume_lock);
}

void x3tp_route_bind(uint32_t source, uint8_t seat, uint8_t endpoint)
{
    if (source >= X3TP_SOURCE_MAX) return;
    if (seat >= X3TP_SEAT_MAX || endpoint >= X3TP_ENDPOINT_MAX) return;

    g_input_seats[seat].active      = true;
    g_input_seats[seat].id          = seat;
    g_input_seats[seat].endpoint    = endpoint;
    g_input_seats[seat].source_mask |= (uint8_t)(1u << source);

    g_input_endpoints[endpoint].active = true;
    g_input_endpoints[endpoint].id     = endpoint;
    g_input_endpoints[endpoint].seat   = seat;

    g_input_routes[source].seat     = seat;
    g_input_routes[source].endpoint = endpoint;
}

void x3tp_init(void)
{
    for (uint32_t i = 0; i < X3TP_SOURCE_MAX; ++i)
    {
        g_input_sources[i].head    = 0;
        g_input_sources[i].tail    = 0;
        g_input_sources[i].dropped = 0;
        g_input_routes[i].seat     = X3TP_INDEX_NONE;
        g_input_routes[i].endpoint = X3TP_INDEX_NONE;
    }

    for (uint32_t i = 0; i < X3TP_SEAT_MAX; ++i)
    {
        g_input_seats[i].active      = false;
        g_input_seats[i].id          = (uint8_t)i;
        g_input_seats[i].endpoint    = X3TP_INDEX_NONE;
        g_input_seats[i].source_mask = 0;
    }

    for (uint32_t i = 0; i < X3TP_ENDPOINT_MAX; ++i)
    {
        g_input_endpoints[i].active   = false;
        g_input_endpoints[i].closed   = true;
        g_input_endpoints[i].id       = (uint8_t)i;
        g_input_endpoints[i].seat     = X3TP_INDEX_NONE;
        g_input_endpoints[i].notify   = 0;
        g_input_endpoints[i].sequence = 0;
        g_input_endpoints[i].wakes    = 0;
        g_input_endpoints[i].wake     = NULL;
    }

    x3tp_terminal_init(&g_default_terminal);

    /* Mono-session: every source lands on seat0, whose only endpoint is the
       /dev/console reader. This is the whole "focus model" of v2. */
    x3tp_route_bind(X3TP_SOURCE_PS2, X3TP_CONSOLE_SEAT, X3TP_CONSOLE_ENDPOINT);
    x3tp_route_bind(X3TP_SOURCE_USB, X3TP_CONSOLE_SEAT, X3TP_CONSOLE_ENDPOINT);
    x3tp_route_bind(X3TP_SOURCE_VKBD, X3TP_CONSOLE_SEAT, X3TP_CONSOLE_ENDPOINT);
    x3tp_endpoint_open(X3TP_CONSOLE_ENDPOINT);
}

bool x3tp_route_irq(uint32_t source, const x3tp_event_t *event)
{
    if (event == NULL || source >= X3TP_SOURCE_MAX) return false;

    /* Boot-time constant table: no lock, no focus lookup. */
    x3tp_route_t route = g_input_routes[source];
    if (route.endpoint >= X3TP_ENDPOINT_MAX) return false;

    x3tp_endpoint_t *endpoint = &g_input_endpoints[route.endpoint];
    if (!endpoint->active || endpoint->closed) return false;

    x3tp_source_t *queue = &g_input_sources[source];
    uint32_t        head  = queue->head;
    uint32_t        tail  = queue->tail;
    if ((uint32_t)(head - tail) >= X3TP_SOURCE_RING_CAP)
    {
        /* Full: drop the newest event, never block or overwrite history. */
        __atomic_add_fetch(&queue->dropped, 1, __ATOMIC_RELAXED);
        return false;
    }

    x3tp_event_t copy = *event;
    copy.reserved = 0;
    copy.sequence = __atomic_fetch_add(&endpoint->sequence, 1, __ATOMIC_RELAXED) + 1;

    queue->events[head % X3TP_SOURCE_RING_CAP] = copy;
    __atomic_store_n(&queue->head, head + 1, __ATOMIC_RELEASE);

    /* Wake only on endpoint empty -> nonempty. The exchange is the transition
       detector, so concurrent producers do not storm the wake hook. */
    if (__atomic_exchange_n(&endpoint->notify, 1, __ATOMIC_ACQ_REL) == 0)
        input_endpoint_kick(endpoint);
    return true;
}

bool x3tp_console_has_data(void)
{
    return input_endpoint_has_events(&g_input_endpoints[X3TP_CONSOLE_ENDPOINT]);
}

int x3tp_console_poll(size_t events)
{
    x3tp_endpoint_t *endpoint = &g_input_endpoints[X3TP_CONSOLE_ENDPOINT];
    if (!endpoint->active || endpoint->closed) return EPOLLHUP;

    int revents = 0;
    if ((events & EPOLLIN) != 0 && x3tp_console_has_data()) revents |= EPOLLIN;
    return revents;
}

int64_t x3tp_console_read(void *buffer, size_t size)
{
    if (buffer == NULL) return -EINVAL;
    if (size < sizeof(x3tp_event_t)) return -EINVAL;

    x3tp_endpoint_t *endpoint = &g_input_endpoints[X3TP_CONSOLE_ENDPOINT];
    if (!endpoint->active || endpoint->closed) return -EIO;

    uint8_t mask = g_input_seats[X3TP_CONSOLE_SEAT].source_mask;
    uint8_t *out  = (uint8_t *)buffer;
    size_t   used = 0;

    spin_lock(&g_input_consume_lock);

    /* Clear then drain: anything published after the clear is either copied
       here or re-flagged, so a wake can never be lost between the two. */
    __atomic_store_n(&endpoint->notify, 0, __ATOMIC_RELEASE);

    for (uint32_t source = 0; source < X3TP_SOURCE_MAX; ++source)
    {
        if (!(mask & (1u << source))) continue;
        x3tp_source_t *queue = &g_input_sources[source];

        while (used + sizeof(x3tp_event_t) <= size)
        {
            uint32_t tail = queue->tail;
            if (tail == __atomic_load_n(&queue->head, __ATOMIC_ACQUIRE)) break;

            x3tp_event_t event = queue->events[tail % X3TP_SOURCE_RING_CAP];
            __atomic_store_n(&queue->tail, tail + 1, __ATOMIC_RELEASE);

            memcpy(out + used, &event, sizeof(event));
            used += sizeof(event);
        }
    }

    if (input_endpoint_has_events(endpoint))
        __atomic_store_n(&endpoint->notify, 1, __ATOMIC_RELEASE);

    spin_unlock(&g_input_consume_lock);

    return used == 0 ? -EAGAIN : (int64_t)used;
}

/* ---- terminal view ----------------------------------------------------- */

void x3tp_terminal_view_init(x3tp_terminal_view_t *view)
{
    if (view == NULL) return;
    view->getch_head     = 0;
    view->getch_tail     = 0;
    view->input_flags    = 0;
    view->wait_for_input = false;
    view->input_lock     = false;
    memset(view->getch_bytes, 0, sizeof(view->getch_bytes));
    memset(view->line, 0, sizeof(view->line));
}

void x3tp_terminal_view_reset(x3tp_terminal_view_t *view)
{
    x3tp_terminal_view_init(view);
}

void x3tp_terminal_init(x3tp_terminal_t *terminal)
{
    if (terminal == NULL) return;
    terminal->console_owner = false;
    terminal->output_lock   = false;
    memset(terminal->output, 0, sizeof(terminal->output));
    x3tp_terminal_view_init(&terminal->view);
}

static uint32_t x3tp_terminal_view_push(x3tp_terminal_view_t *view, const void *data, uint32_t size)
{
    if (view == NULL || data == NULL || size == 0) return 0;

    const char *bytes = (const char *)data;
    uint32_t    head  = view->getch_head;
    uint32_t    tail  = view->getch_tail;
    uint32_t    space = X3TP_TERMINAL_GETCH_CAP - (head - tail);
    if (size > space) size = space;

    for (uint32_t i = 0; i < size; ++i)
        view->getch_bytes[(head + i) % X3TP_TERMINAL_GETCH_CAP] = bytes[i];
    if (size != 0) __atomic_store_n(&view->getch_head, head + size, __ATOMIC_RELEASE);
    return size;
}

static int x3tp_terminal_view_pop(x3tp_terminal_view_t *view)
{
    if (view == NULL) return -1;

    uint32_t head = __atomic_load_n(&view->getch_head, __ATOMIC_ACQUIRE);
    uint32_t tail = view->getch_tail;
    if (head == tail) return -1;

    char c = view->getch_bytes[tail % X3TP_TERMINAL_GETCH_CAP];
    __atomic_store_n(&view->getch_tail, tail + 1, __ATOMIC_RELEASE);
    return (int)(unsigned char)c;
}

/* Pull committed CHAR events from the seat0 rings into this terminal's byte
 * ring.  KEYDOWN/KEYUP have no byte encoding and are consumed here. */
static bool x3tp_terminal_view_drain(x3tp_terminal_view_t *view)
{
    if (view == NULL) return false;

    x3tp_endpoint_t *endpoint = &g_input_endpoints[X3TP_CONSOLE_ENDPOINT];
    if (!endpoint->active || endpoint->closed) return false;

    uint8_t mask   = g_input_seats[X3TP_CONSOLE_SEAT].source_mask;
    bool    pushed = false;

    spin_lock(&g_input_consume_lock);
    __atomic_store_n(&endpoint->notify, 0, __ATOMIC_RELEASE);

    for (uint32_t source = 0; source < X3TP_SOURCE_MAX; ++source)
    {
        if (!(mask & (1u << source))) continue;
        x3tp_source_t *queue = &g_input_sources[source];

        for (;;)
        {
            uint32_t tail = queue->tail;
            if (tail == __atomic_load_n(&queue->head, __ATOMIC_ACQUIRE)) break;

            x3tp_event_t event = queue->events[tail % X3TP_SOURCE_RING_CAP];
            __atomic_store_n(&queue->tail, tail + 1, __ATOMIC_RELEASE);

            if (event.type != X3TP_EVENT_CHAR) continue;

            char   bytes[4];
            memcpy(bytes, &event.value, sizeof(bytes));
            size_t count = ime_utf8_scalar_bytes(bytes, sizeof(bytes));
            if (count != 0 && x3tp_terminal_view_push(view, bytes, (uint32_t)count) != 0)
                pushed = true;
        }
    }

    if (input_endpoint_has_events(endpoint))
        __atomic_store_n(&endpoint->notify, 1, __ATOMIC_RELEASE);

    spin_unlock(&g_input_consume_lock);
    return pushed;
}

int x3tp_terminal_getch(x3tp_terminal_view_t *view)
{
    int value = x3tp_terminal_view_pop(view);
    if (value >= 0) return value;
    if (!x3tp_terminal_view_drain(view)) return -1;
    return x3tp_terminal_view_pop(view);
}

x3tp_terminal_view_t *x3tp_terminal_default_view(void)
{
    return &g_default_terminal.view;
}

x3tp_terminal_view_t *x3tp_terminal_current_view(void)
{
    tcb_t current = get_current_task();
    pcb_t process = current != NULL ? current->parent_group : NULL;

    while (process != NULL && process != kernel_group)
    {
        if (process->terminal != NULL && process->terminal->console_owner)
            return &process->terminal->view;
        process = process->parent_task;
    }
    return &g_default_terminal.view;
}

/* ---- terminal lifecycle / xapi_Input + SXAH_* -------------------------- */

void x3tp_mark_terminal(void)
{
    tcb_t current = get_current_task();
    if (current == NULL || current->parent_group == NULL || current->parent_group->terminal == NULL)
        return;
    x3tp_terminal_init(current->parent_group->terminal);
    current->parent_group->terminal->console_owner = true;
}

int x3tp_terminal_ready(void)
{
    tcb_t current = get_current_task();
    if (current == NULL || current->parent_group == NULL ||
        current->parent_group->parent_task == NULL ||
        current->parent_group->parent_task->terminal == NULL)
        return 0;
    return current->parent_group->parent_task->terminal->console_owner ? 1 : 0;
}

int x3tp_terminal_waiting_status(void)
{
    tcb_t current = get_current_task();
    if (current == NULL || current->parent_group == NULL ||
        current->parent_group->terminal == NULL ||
        !current->parent_group->terminal->view.wait_for_input)
        return 0;

    int status = 1; /* XTTTP_INPUT_STATUS_WAITING */
    if ((current->parent_group->terminal->view.input_flags & 1u /* XAPI_INPUT_NO_ECHO */) != 0)
        status |= 2; /* XTTTP_INPUT_STATUS_NO_ECHO */
    return status;
}

int x3tp_terminal_read_output(char *str, size_t capacity)
{
    tcb_t current = get_current_task();
    if (str == NULL || current == NULL || current->parent_group == NULL ||
        current->parent_group->terminal == NULL)
        return -EFAULT;
    if (capacity == 0) return -EINVAL;

    x3tp_terminal_t *terminal = current->parent_group->terminal;
    char output[X3TP_TERMINAL_OUTPUT_CAP];
    memset(output, 0, sizeof(output));
    if (terminal->output_lock)
        return copy_to_user_pagedir(current->parent_group->pagedir, str, output, 1) ? 0 : -EFAULT;

    strncpy(output, terminal->output, sizeof(output) - 1);
    output[sizeof(output) - 1] = '\0';
    size_t len = strnlen(output, sizeof(output) - 1);
    if (len >= capacity) len = capacity - 1;
    output[len] = '\0';
    return copy_to_user_pagedir(current->parent_group->pagedir, str, output, len + 1) ? 0 : -EFAULT;
}

int x3tp_terminal_write_input(const char *str)
{
    tcb_t current = get_current_task();
    if (str == NULL || current == NULL || current->parent_group == NULL ||
        current->parent_group->terminal == NULL)
        return -EFAULT;

    x3tp_terminal_view_t *view = &current->parent_group->terminal->view;
    size_t cur = strnlen(view->line, sizeof(view->line));
    if (cur >= sizeof(view->line)) cur = 0;
    size_t space = sizeof(view->line) - cur - 1;
    size_t len = 0;
    while (len < space)
    {
        char ch;
        if (!copy_from_user_pagedir(current->parent_group->pagedir, &ch, str + len, 1))
            return -EFAULT;
        if (ch == '\0') break;
        view->line[cur + len++] = ch;
    }
    view->line[cur + len] = '\0';
    view->input_lock = true;
    return 0;
}

void x3tp_terminal_finish_output(void)
{
    tcb_t current = get_current_task();
    if (current != NULL && current->parent_group != NULL && current->parent_group->terminal != NULL)
        current->parent_group->terminal->output_lock = true;
}
