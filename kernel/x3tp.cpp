/*
 * X3TP input core — seat/endpoint routing without a router thread.
 * g_input_sources[]   per-source SPSC rings: PS/2 IRQ, xHCI worker, vkbd.
 * g_input_seats[]     seat -> owning endpoint + source mask.
 * g_input_endpoints[] static endpoints (endpoint0 -> /dev/console).
 * g_input_routes[]    source -> {seat, endpoint}, constant while running.
 * Multiseat: fill the tables and call x3tp_route_bind() at seat setup.
 */
#include <cpu/lock.h>
#include <errno.h>
#include <x3tp.h>
#include <krlibc.h>
#include <task/poll.h>

static x3tp_source_t   g_input_sources[X3TP_SOURCE_MAX];
static x3tp_seat_t     g_input_seats[X3TP_SEAT_MAX];
static x3tp_endpoint_t g_input_endpoints[X3TP_ENDPOINT_MAX];
static x3tp_route_t    g_input_routes[X3TP_SOURCE_MAX];

// Serialises consumers only (never producers). Two readers must not race the
// same SPSC tail.
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

// Does this endpoint's seat still have queued events? Read-only, lock-free.
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

// Producer-side wake: only when this endpoint was empty before the push
// (notify 0 -> 1). With no bound consumer wake is NULL and readers poll.
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

    // Mono-session: every source lands on seat0, whose only endpoint is the
    // /dev/console reader. This is the whole "focus model".
    x3tp_route_bind(X3TP_SOURCE_PS2, X3TP_CONSOLE_SEAT, X3TP_CONSOLE_ENDPOINT);
    x3tp_route_bind(X3TP_SOURCE_USB, X3TP_CONSOLE_SEAT, X3TP_CONSOLE_ENDPOINT);
    x3tp_route_bind(X3TP_SOURCE_VKBD, X3TP_CONSOLE_SEAT, X3TP_CONSOLE_ENDPOINT);
    x3tp_endpoint_open(X3TP_CONSOLE_ENDPOINT);
}

bool x3tp_route_irq(uint32_t source, const x3tp_event_t *event)
{
    if (event == NULL || source >= X3TP_SOURCE_MAX) return false;

    // IRQ fast path: table lookup, one SPSC publish, one notify exchange.
    // No lock, no allocation, no router thread.
    x3tp_route_t route = g_input_routes[source];
    if (route.endpoint >= X3TP_ENDPOINT_MAX) return false;

    x3tp_endpoint_t *endpoint = &g_input_endpoints[route.endpoint];
    if (!endpoint->active || endpoint->closed) return false;

    x3tp_source_t *queue = &g_input_sources[source];
    uint32_t        head  = queue->head;
    uint32_t        tail  = queue->tail;
    if ((uint32_t)(head - tail) >= X3TP_SOURCE_RING_CAP)
    {
        // Full: drop the newest event, never block or overwrite history.
        __atomic_add_fetch(&queue->dropped, 1, __ATOMIC_RELAXED);
        return false;
    }

    x3tp_event_t copy = *event;
    copy.reserved = 0;
    copy.sequence = __atomic_fetch_add(&endpoint->sequence, 1, __ATOMIC_RELAXED) + 1;

    queue->events[head % X3TP_SOURCE_RING_CAP] = copy;
    __atomic_store_n(&queue->head, head + 1, __ATOMIC_RELEASE);

    // Wake only on endpoint empty -> nonempty; the exchange is the transition
    // detector, so concurrent producers do not storm the wake hook.
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

    // Clear then drain: anything published after the clear is either copied
    // here or re-flagged, so a wake cannot be lost between the two.
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
