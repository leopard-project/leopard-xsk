/**
 * Console/tty device layer.
 *
 * Historically these definitions lived in driver/fs/vfs/vfs.cpp even though
 * they are not VFS objects: they register /dev/stdio, /dev/tty, /dev/null and
 * /dev/urandom and translate termios/ioctl requests. Keeping them separate
 * makes the VFS core a pure namespace/dispatch layer.
 */
#include <device.h>
#include <errno.h>
#include <ioctl.h>
#include <krlibc.h>
#include <ps2/keyboard.h>
#include <rng.h>
#include <task/pcb.h>
#include <task/poll.h>
#include <task/scheduler.h>
#include <tty.h>
#include <x3tp.h>

extern void p_xapi_output_kernel(const char *str);

tty_t *defualt_tty = NULL;
int    tty_mode = KD_TEXT;
int    tty_kbmode = K_XLATE;
struct vt_mode current_vt_mode = {0};

static tty_t *current_tty()
{
    tcb_t task = get_current_task();
    if (task == NULL || task->parent_group == NULL || task->parent_group->tty == NULL) return get_default_tty();
    return task->parent_group->tty;
}

static x3tp_terminal_t *current_x3tp_terminal()
{
    tcb_t task = get_current_task();
    pcb_t process = task != NULL ? task->parent_group : NULL;
    while (process != NULL && process != kernel_group)
    {
        if (process->terminal != NULL && process->terminal->console_owner) return process->terminal;
        process = process->parent_task;
    }
    return NULL;
}

static void tty_kernel_flush(tty_t *tty)
{
    (void)tty;
}

static void tty_kernel_print(tty_t *tty, const char *msg)
{
    (void)tty;
    /* p_xapi_output_kernel already routes to the XTTTP terminal when one
     * exists, and falls back to the serial/framebuffer console otherwise.
     * Writing serial here as well would duplicate every line on the
     * LeopardOS serial console (there is no separate terminal app). */
    p_xapi_output_kernel(msg);
}

static void tty_kernel_putc(tty_t *tty, int c)
{
    (void)tty;
    char buf[2] = {(char)c, '\0'};
    p_xapi_output_kernel(buf);
}

static void tty_termios_default(termios_t *termios)
{
    memset(termios, 0, sizeof(*termios));
    termios->c_lflag = ECHO | ICANON | IEXTEN | ISIG;
    termios->c_iflag = BRKINT | ICRNL | INPCK | ISTRIP | IXON;
    termios->c_oflag = OPOST;
    termios->c_cflag = CS8 | CREAD | CLOCAL;
    termios->c_line = 0;
    termios->c_cc[VINTR] = 3;
    termios->c_cc[VQUIT] = 28;
    termios->c_cc[VERASE] = 0x7F;
    termios->c_cc[VKILL] = 21;
    termios->c_cc[VEOF] = 4;
    termios->c_cc[VTIME] = 0;
    termios->c_cc[VMIN] = 1;
    termios->c_cc[VSTART] = 17;
    termios->c_cc[VSTOP] = 19;
    termios->c_cc[VSUSP] = 26;
    termios->c_cc[VREPRINT] = 18;
    termios->c_cc[VDISCARD] = 15;
    termios->c_cc[VWERASE] = 23;
    termios->c_cc[VLNEXT] = 22;
}

tty_t *alloc_default_tty()
{
    tty_t *tty = (tty_t *)malloc(sizeof(tty_t));
    if (tty == NULL) return NULL;
    memset(tty, 0, sizeof(*tty));
    tty->width = 720;
    tty->height = 400;
    tty->print = tty_kernel_print;
    tty->putchar = tty_kernel_putc;
    tty->flush = tty_kernel_flush;
    tty_termios_default(&tty->termios);
    tty->is_sigterm = false;
    return tty;
}

void free_tty(tty_t *tty)
{
    if (tty == NULL || tty == defualt_tty) return;
    free(tty);
}

tty_t *get_default_tty()
{
    return defualt_tty;
}

static size_t stdin_read(int drive, uint8_t *buffer, size_t number, size_t lba) {
    bool is_sti = are_interrupts_enabled();
    open_interrupt;

    x3tp_terminal_t *terminal = current_x3tp_terminal();
    if (terminal != NULL)
    {
        terminal->view.wait_for_input = true;
        while (!terminal->view.input_lock)
        {
            scheduler_yield();
        }
    }

    size_t i = 0;
    for (; i < number; i++) {
        char c;
        if (terminal != NULL)
        {
            c = terminal->view.line[0];
            if (c == '\0') break;
            size_t remaining = strlen(terminal->view.line);
            if (remaining > 0)
            {
                memmove(terminal->view.line, terminal->view.line + 1, remaining);
            }
        }
        else
        {
            c = (char)get_keyboard_input();
        }
        if (c == '\0') { break; }
        tty_t *tty = current_tty();
        uint32_t lflag = tty != NULL ? tty->termios.c_lflag : 0;
        if (c == 0x9) { c = '\t'; }
        if (terminal == NULL && (c == 0x7f || c == '\b') && (lflag & ICANON)) {
            if (tty != NULL && (lflag & ECHO)) write_serial_fmt("\b \b");
            if (i > 0) {
                i--;
                buffer[i] = '\0';
            }
            continue;
        }
        if (tty != NULL && (lflag & ECHO)) write_serial_fmt("%c", c);
        buffer[i] = c;
        if (c == '\n') {
            i++;
            break;
        }
    }

    if (terminal != NULL)
    {
        if (terminal->view.line[0] == '\0')
        {
            terminal->view.input_lock = false;
            terminal->view.wait_for_input = false;
        }
        else
        {
            terminal->view.input_lock = true;
            terminal->view.wait_for_input = false;
        }
    }

    if (!is_sti) close_interrupt;

    pcb_t process = get_current_task() ? get_current_task()->parent_group : NULL;
    if (process != NULL && process->linux_abi) {
        // write_serial_fmt("[] stdin_read requested=%llu ret=%llu\n",
        //                  (unsigned long long)number,
        //                  (unsigned long long)i);
    }

    return i;
}

static size_t stdout_write(int drive, uint8_t *buffer, size_t number, size_t lba) {
    tty_t *tty = current_tty();
    if (tty != NULL && tty->print != NULL)
    {
        char *text = (char *)malloc(number + 1);
        if (text == NULL) return 0;
        memcpy(text, buffer, number);
        text[number] = '\0';
        tty->print(tty, text);
        free(text);
    }
    else
    {
        for (size_t i = 0; i < number; i++) write_serial_fmt("%c", buffer[i]);
    }
    return number;
}

static errno_t tty_ioctl(device_t *device, size_t req, void *arg)
{
    (void)device;
    tty_t *tty = current_tty();
    if (tty == NULL) return -ENOTTY;

    switch (req)
    {
    case TIOCGWINSZ:
    {
        struct winsize *ws = (struct winsize *)arg;
        if (ws != NULL)
        {
            ws->ws_row = 25;
            ws->ws_col = 80;
            ws->ws_xpixel = (unsigned short)tty->width;
            ws->ws_ypixel = (unsigned short)tty->height;
        }
        break;
    }
    case TCGETS:
    {
        struct termios *termios = (struct termios *)arg;
        if (termios == NULL) return -EFAULT;
        memcpy(termios, &tty->termios, sizeof(struct termios));
        break;
    }
    case TCSETS:
    case TCSETSF:
    case TCSETSW:
    {
        const struct termios *termios = (const struct termios *)arg;
        if (termios == NULL) return -EFAULT;
        memcpy(&tty->termios, termios, sizeof(struct termios));
        break;
    }
    case TIOCGPGRP:
        if (arg == NULL) return -EFAULT;
        *(int *)arg = get_current_task()->parent_group->pid;
        tty->is_sigterm = true;
        break;
    case TIOCSPGRP:
    case TIOCSCTTY:
        break;
    case KDGETMODE:
        if (arg == NULL) return -EFAULT;
        *(int *)arg = tty_mode;
        break;
    case KDSETMODE:
        if (arg == NULL) return -EFAULT;
        tty_mode = *(int *)arg;
        break;
    case KDGKBMODE:
        if (arg == NULL) return -EFAULT;
        *(int *)arg = tty_kbmode;
        break;
    case KDSKBMODE:
        if (arg == NULL) return -EFAULT;
        tty_kbmode = *(int *)arg;
        break;
    case VT_SETMODE:
        if (arg == NULL) return -EFAULT;
        memcpy(&current_vt_mode, arg, sizeof(current_vt_mode));
        break;
    case VT_GETMODE:
        if (arg == NULL) return -EFAULT;
        memcpy(arg, &current_vt_mode, sizeof(current_vt_mode));
        break;
    case VT_OPENQRY:
        if (arg == NULL) return -EFAULT;
        *(int *)arg = 1;
        break;
    default:
        return -ENOTTY;
    }
    return EOK;
}

static errno_t tty_poll(size_t events)
{
    int revents = 0;
    if (events & POLLIN)
    {
        x3tp_terminal_t *terminal = current_x3tp_terminal();
        if (terminal != NULL)
        {
            if (terminal->view.line[0] != '\0') revents |= POLLIN;
        }
        else
        {
            revents |= POLLIN;
        }
    }
    if (events & POLLOUT) revents |= POLLOUT;
    return revents;
}

static size_t null_read(int drive, uint8_t *buffer, size_t number, size_t lba)
{
    UNUSED(drive);
    UNUSED(buffer);
    UNUSED(lba);
    return 0;
}

static size_t null_write(int drive, uint8_t *buffer, size_t number, size_t lba)
{
    UNUSED(drive);
    UNUSED(buffer);
    UNUSED(lba);
    return number;
}

static size_t urandom_read(int drive, uint8_t *buffer, size_t number, size_t lba)
{
    UNUSED(drive);
    UNUSED(lba);
    if (buffer == NULL) return 0;

    get_random_bytes(buffer, number);
    return number;
}

static size_t urandom_write(int drive, uint8_t *buffer, size_t number, size_t lba)
{
    UNUSED(drive);
    UNUSED(buffer);
    UNUSED(lba);
    return number;
}

void build_tty_device()
{
    device_t stdio;
    stdio.type = DEVICE_STREAM;
    strcpy(stdio.drive_name, "stdio");
    stdio.flag        = 1;
    stdio.sector_size = 1;
    stdio.size        = 1;
    stdio.read        = stdin_read;
    stdio.write       = stdout_write;
    stdio.ioctl       = tty_ioctl;
    stdio.poll        = tty_poll;
    stdio.map         = (mapf)empty;
    regist_device(NULL, stdio);

    device_t ttydev = stdio;
    strcpy(ttydev.drive_name, "tty");
    regist_device(NULL, ttydev);

    device_t null_dev = {};
    null_dev.type = DEVICE_STREAM;
    strcpy(null_dev.drive_name, "null");
    null_dev.flag        = 1;
    null_dev.sector_size = 1;
    null_dev.size        = 0;
    null_dev.read        = null_read;
    null_dev.write       = null_write;
    null_dev.ioctl       = tty_ioctl;
    null_dev.poll        = tty_poll;
    null_dev.map         = (mapf)empty;
    regist_device(NULL, null_dev);

    device_t urandom_dev = {};
    urandom_dev.type = DEVICE_STREAM;
    strcpy(urandom_dev.drive_name, "urandom");
    urandom_dev.flag        = 1;
    urandom_dev.sector_size = 1;
    urandom_dev.size        = (size_t)-1;
    urandom_dev.read        = urandom_read;
    urandom_dev.write       = urandom_write;
    urandom_dev.ioctl       = tty_ioctl;
    urandom_dev.poll        = tty_poll;
    urandom_dev.map         = (mapf)empty;
    regist_device(NULL, urandom_dev);
}

void init_tty()
{
    defualt_tty = alloc_default_tty();
    build_tty_device();
}

void stdio_init()
{
    init_tty();
}

