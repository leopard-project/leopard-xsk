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
#include <rng.h>
#include <task/pcb.h>
#include <task/poll.h>
#include <tty.h>

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

static void tty_kernel_flush(tty_t *tty)
{
    (void)tty;
}

// /dev/stdio is a non-interactive fallback for kernel-launched tasks: output
// goes to the serial console, never to a kernel-side terminal view.
static void tty_kernel_print(tty_t *tty, const char *msg)
{
    (void)tty;
    write_serial_string(msg);
}

static void tty_kernel_putc(tty_t *tty, int c)
{
    (void)tty;
    char buf[2] = {(char)c, '\0'};
    write_serial_string(buf);
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

// No kernel-side line editor exists any more: a kernel-launched task has no
// interactive terminal, so report EOF immediately instead of blocking.
static size_t stdin_read(int drive, uint8_t *buffer, size_t number, size_t lba) {
    UNUSED(drive);
    UNUSED(buffer);
    UNUSED(number);
    UNUSED(lba);
    return 0;
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

// Input is EOF, so only writability is reported; a fake POLLIN would spin
// readers on a descriptor that can never produce data.
static errno_t tty_poll(size_t events)
{
    int revents = 0;
    if (events & POLLOUT) revents |= POLLOUT;
    return revents;
}

// /dev/null and /dev/urandom always have data (EOF or fresh bytes) and accept
// writes, unlike the dead interactive input of /dev/stdio.
static errno_t ready_poll(size_t events)
{
    int revents = 0;
    if (events & POLLIN) revents |= POLLIN;
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
    null_dev.poll        = ready_poll;
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
    urandom_dev.poll        = ready_poll;
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

