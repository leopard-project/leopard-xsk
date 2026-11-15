#pragma once
#include <stdint.h>

/*
 * X3TP — the kernel input/terminal standard.
 *
 * Event core:
 *   PS/2 IRQ, xHCI worker and /dev/vkbd enqueue a structured x3tp_event_t into
 *   their own per-source SPSC ring.  A static table routes source -> seat ->
 *   endpoint; /dev/console is the seat0 endpoint.  The IRQ path takes no lock,
 *   allocates nothing and never consults a global focus slot.
 *
 * Terminal view:
 *   xapi_Getch / xapi_Input / SXAH_* are terminal APIs, not the event core.
 *   They operate on one x3tp_terminal_view_t per terminal (byte stream, line
 *   mailbox and legacy flags).  Terminal hosts (leopard-conhost today, a
 *   compositor or serial/remote host later) feed that view.
 *
 * ABI: /dev/console read() returns whole 24-byte little-endian records of
 * x3tp_event_t.  No write/ioctl is defined; commit text is handled by the
 * terminal host (PTY or window).
 */

#define X3TP_EVENT_CHAR    1u
#define X3TP_EVENT_KEYDOWN 2u
#define X3TP_EVENT_KEYUP   3u

/* KEYDOWN/KEYUP values are the stable KEY_* range from
 * include/ps2/keyboard.h (KEY_ESC = 128 .. KEY_DELETE = 160).  They are part
 * of the /dev/console ABI and must never be renumbered. */
#define X3TP_FLAG_SHIFT  (1u << 0)
#define X3TP_FLAG_CTRL   (1u << 1)
#define X3TP_FLAG_ALT    (1u << 2)
#define X3TP_FLAG_WIN    (1u << 3)
#define X3TP_FLAG_CAPS   (1u << 4)
#define X3TP_FLAG_NUM    (1u << 5)
#define X3TP_FLAG_SCROLL (1u << 6)

typedef struct x3tp_event
{
    uint32_t type;     /* X3TP_EVENT_* */
    uint32_t value;    /* CHAR: packed UTF-8 (ime_utf8_pack); KEY: key code */
    uint32_t flags;    /* X3TP_FLAG_* */
    uint32_t reserved; /* must be 0 */
    uint64_t sequence; /* monotonic per endpoint */
} x3tp_event_t;

#ifdef __cplusplus
static_assert(sizeof(x3tp_event_t) == 24, "x3tp_event_t must stay 24 bytes (read ABI)");
#else
_Static_assert(sizeof(x3tp_event_t) == 24, "x3tp_event_t must stay 24 bytes (read ABI)");
#endif

#define X3TP_SEAT_MAX     8
#define X3TP_ENDPOINT_MAX 8
#define X3TP_SOURCE_RING_CAP 256u /* power of two */

enum x3tp_source_id
{
    X3TP_SOURCE_PS2  = 0,
    X3TP_SOURCE_USB  = 1,
    X3TP_SOURCE_VKBD = 2,
    X3TP_SOURCE_MAX
};

#define X3TP_INDEX_NONE       0xFFu
#define X3TP_CONSOLE_SEAT     0u
#define X3TP_CONSOLE_ENDPOINT 0u

typedef struct x3tp_route
{
    uint8_t seat;     /* X3TP_SEAT_MAX, or X3TP_INDEX_NONE */
    uint8_t endpoint; /* X3TP_ENDPOINT_MAX, or X3TP_INDEX_NONE */
} x3tp_route_t;

typedef struct x3tp_source
{
    x3tp_event_t events[X3TP_SOURCE_RING_CAP];
    volatile uint32_t head __attribute__((aligned(64)));
    volatile uint32_t tail __attribute__((aligned(64)));
    volatile uint64_t dropped;
} x3tp_source_t;

struct x3tp_endpoint;
typedef void (*x3tp_endpoint_wake_fn)(struct x3tp_endpoint *endpoint);

typedef struct x3tp_endpoint
{
    volatile bool active;
    volatile bool closed;
    uint8_t       id;
    uint8_t       seat;
    volatile uint32_t notify;
    volatile uint64_t sequence __attribute__((aligned(64)));
    volatile uint64_t wakes;
    x3tp_endpoint_wake_fn wake;
} x3tp_endpoint_t;

typedef struct x3tp_seat
{
    volatile bool active;
    uint8_t       id;
    uint8_t       endpoint;
    uint8_t       source_mask;
} x3tp_seat_t;

/* ---- event core ------------------------------------------------------- */

void x3tp_init(void);
void x3tp_route_bind(uint32_t source, uint8_t seat, uint8_t endpoint);
void x3tp_endpoint_open(uint8_t endpoint);
void x3tp_endpoint_close(uint8_t endpoint);
bool x3tp_route_irq(uint32_t source, const x3tp_event_t *event);

bool    x3tp_console_has_data(void);
int64_t x3tp_console_read(void *buffer, size_t size); /* bytes, or -errno */
int     x3tp_console_poll(size_t events);             /* EPOLL* revents */

/* ---- terminal view (xapi_Getch / xapi_Input / SXAH_*) ----------------- */

#define X3TP_TERMINAL_GETCH_CAP 1024
#define X3TP_TERMINAL_LINE_CAP  1024
#define X3TP_TERMINAL_OUTPUT_CAP 1024

typedef struct x3tp_terminal_view
{
    volatile uint32_t getch_head __attribute__((aligned(64)));
    volatile uint32_t getch_tail __attribute__((aligned(64)));
    char     getch_bytes[X3TP_TERMINAL_GETCH_CAP];
    uint32_t input_flags;      /* XAPI_INPUT_NO_ECHO */
    bool     wait_for_input;
    bool     input_lock;
    char     line[X3TP_TERMINAL_LINE_CAP];
} x3tp_terminal_view_t;

typedef struct x3tp_terminal
{
    bool console_owner;        /* terminal host that owns console output */
    bool output_lock;
    char output[X3TP_TERMINAL_OUTPUT_CAP];
    x3tp_terminal_view_t view;
} x3tp_terminal_t;

void x3tp_terminal_init(x3tp_terminal_t *terminal);
void x3tp_terminal_view_init(x3tp_terminal_view_t *view);
void x3tp_terminal_view_reset(x3tp_terminal_view_t *view);

/* Non-blocking one-byte read; drains CHAR events from the seat0 rings when the
 * byte ring is empty.  -1 means "nothing ready". */
int  x3tp_terminal_getch(x3tp_terminal_view_t *view);

x3tp_terminal_view_t *x3tp_terminal_current_view(void);
x3tp_terminal_view_t *x3tp_terminal_default_view(void);

/* Terminal-facing APIs used by xapi_Input / SXAH_* wrappers. */
void x3tp_mark_terminal(void);
int  x3tp_terminal_ready(void);
int  x3tp_terminal_waiting_status(void);
int  x3tp_terminal_read_output(char *str, size_t capacity);
int  x3tp_terminal_write_input(const char *str);
void x3tp_terminal_finish_output(void);

/* Device registration for /dev/console (driver/fs/vfs/dev.cpp). */
void x3tp_console_device_init(void);
