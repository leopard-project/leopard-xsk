/*
 * /dev/vkbd — virtual keyboard injection primitive.
 * User space writes UTF-8 and each scalar becomes a CHAR event on
 * X3TP_SOURCE_VKBD (seat0 -> endpoint0 -> /dev/console). This is an input
 * source, not an IME API: root-only, no exported symbol.
 */
#include <cpu/lock.h>
#include <device.h>
#include <errno.h>
#include <fs/vfs/vfs.h>
#include <ime_utf8.h>
#include <x3tp.h>
#include <krlibc.h>
#include <proto.hpp>
#include <syscall/pxapi.h>

// Several user processes may write the same source; the core ring is
// single-producer, so serialise producers here (syscall context, never IRQ).
static spin_t g_vkbd_lock = SPIN_INIT;

#define VKBD_WRITE_MAX 1024

static size_t vkbd_read(int drive, uint8_t *buffer, size_t number, size_t lba)
{
    (void)drive;
    (void)buffer;
    (void)number;
    (void)lba;
    return 0; // write-only device
}

static size_t vkbd_write(int drive, uint8_t *buffer, size_t number, size_t lba)
{
    (void)drive;
    (void)lba;
    if (buffer == NULL || number == 0) return 0;

    // Keep one write bounded so a paste cannot flood the source ring.
    if (number > VKBD_WRITE_MAX) number = VKBD_WRITE_MAX;

    size_t accepted = 0;
    spin_lock_no_irqsave(&g_vkbd_lock);
    while (accepted < number)
    {
        size_t count = ime_utf8_scalar_bytes((const char *)buffer + accepted, number - accepted);
        if (count == 0) break;

        x3tp_event_t event = {};
        event.type  = X3TP_EVENT_CHAR;
        event.value = (uint32_t)ime_utf8_pack((const char *)buffer + accepted, count);
        if (!x3tp_route_irq(X3TP_SOURCE_VKBD, &event)) break; // ring full: drop
        accepted += count;
    }
    spin_unlock_no_irqstore(&g_vkbd_lock);
    return accepted;
}

static int vkbd_ioctl(device_t *device, size_t request, void *arg)
{
    (void)device;
    (void)request;
    (void)arg;
    return -ENOTTY;
}

static int vkbd_poll(size_t events)
{
    (void)events;
    return 0;
}

void vkbd_init()
{
    device_t dev = {};
    dev.type        = DEVICE_STREAM;
    strcpy(dev.drive_name, "vkbd");
    dev.flag        = 1;
    dev.sector_size = 1;
    dev.size        = (size_t)-1;
    dev.read        = vkbd_read;
    dev.write       = vkbd_write;
    dev.ioctl       = vkbd_ioctl;
    dev.poll        = vkbd_poll;
    dev.map         = (mapf)empty;

    int id = regist_device(NULL, dev);
    if (id < 0) return;

    vfs_node_t node = vfs_open("/dev/vkbd");
    if (node != NULL)
    {
        // Input injection is privileged: only the console/IME broker may write.
        vfs_chown(node, 0, 0);
        vfs_chmod(node, 0600);
        vfs_close(node);
    }
}
