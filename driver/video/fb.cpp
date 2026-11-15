/**
 * Minimal /dev/fb0 for the userspace console host.
 *
 * The bootloader already hands the kernel a GOP framebuffer through
 * FrameBufferConfig (virtual address in the HHDM). This driver exposes that
 * single scanout as a DEVICE_FB node so a display server can query geometry
 * with FBIOGET_*SCREENINFO and mmap the pixels directly, without pulling in
 * graphics.sys.
 *
 * Deliberate limits:
 *  - one framebuffer, no mode setting (FBIOPUT_VSCREENINFO is read-only);
 *  - mmap starts at offset 0 and covers at most smem_len bytes;
 *  - the last mapped page may extend to the end of the physical page that
 *    contains the framebuffer tail (normal fbdev behaviour).
 */
#include <console.h>
#include <device.h>
#include <efi/fbc.h>
#include <errno.h>
#include <ioctl.h>
#include <krlibc.h>
#include <mm/page.h>
#include <proto.hpp>
#include <screen_capture.h>

namespace
{
constexpr uint32_t FB_BYTES_PER_PIXEL = 4;

const FrameBufferConfig *fb_config    = nullptr;
uint64_t                 fb_phys_base = 0;
size_t                   fb_smem_len  = 0;
uint32_t                 fb_line_len  = 0;
bool                     fb_ready     = false;

void fb_fill_fix(struct fb_fix_screeninfo *fix)
{
    memset(fix, 0, sizeof(*fix));
    memcpy(fix->id, "fb0", 4);
    fix->smem_start  = fb_phys_base;
    fix->smem_len    = (uint32_t)fb_smem_len;
    fix->type        = FB_TYPE_PACKED_PIXELS;
    fix->visual      = FB_VISUAL_TRUECOLOR;
    fix->line_length = fb_line_len;
    fix->accel       = FB_ACCEL_NONE;
}

void fb_fill_var(struct fb_var_screeninfo *var)
{
    memset(var, 0, sizeof(*var));
    var->xres          = fb_config->horizontal_resolution;
    var->yres          = fb_config->vertical_resolution;
    var->xres_virtual  = fb_config->pixels_per_scan_line;
    var->yres_virtual  = fb_config->vertical_resolution;
    var->bits_per_pixel = 32;
    var->grayscale      = 0;
    var->height         = fb_config->vertical_resolution;
    var->width          = fb_config->horizontal_resolution;

    if (fb_config->pixel_format == PixelFormat::kRGBR)
    {
        var->red    = {0, 8, 0};
        var->green  = {8, 8, 0};
        var->blue   = {16, 8, 0};
        var->transp = {24, 8, 0};
    }
    else
    {
        var->blue   = {0, 8, 0};
        var->green  = {8, 8, 0};
        var->red    = {16, 8, 0};
        var->transp = {24, 8, 0};
    }
}

int fb_ioctl(struct _device *device, size_t request, void *arg)
{
    UNUSED(device);
    if (!fb_ready) return -ENODEV;
    if (arg == NULL) return -EFAULT;

    switch ((uint32_t)request)
    {
    case FBIOGET_FSCREENINFO:
        fb_fill_fix((struct fb_fix_screeninfo *)arg);
        return EOK;
    case FBIOGET_VSCREENINFO:
        fb_fill_var((struct fb_var_screeninfo *)arg);
        return EOK;
    case FBIOPUT_VSCREENINFO:
    {
        const struct fb_var_screeninfo *requested = (const struct fb_var_screeninfo *)arg;
        // Read-only framebuffer: accept a no-op write but reject mode changes.
        if (requested->xres != fb_config->horizontal_resolution ||
            requested->yres != fb_config->vertical_resolution ||
            requested->xres_virtual != fb_config->pixels_per_scan_line ||
            requested->bits_per_pixel != 32)
            return -EINVAL;
        return EOK;
    }
    case FBIOPAN_DISPLAY:
    {
        const struct fb_var_screeninfo *requested = (const struct fb_var_screeninfo *)arg;
        return (requested->xoffset == 0 && requested->yoffset == 0) ? EOK : -EINVAL;
    }
    default:
        return -ENOTTY;
    }
}

void *fb_map(int drive, void *addr, size_t len)
{
    UNUSED(drive);
    if (!fb_ready || addr == NULL || len == 0) return NULL;
    // mmap lengths are page-rounded by the syscall layer, so accept the page
    // that contains the framebuffer tail; never map past that.
    const size_t map_limit = (fb_smem_len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (len > map_limit) return NULL;
    if (((uint64_t)addr & (PAGE_SIZE - 1)) != 0) return NULL;

    const uint64_t flags = PTE_PRESENT | PTE_WRITEABLE | PTE_USER | PTE_NO_EXECUTE;
    page_directory_t *directory = get_current_directory();
    if (directory == NULL) return NULL;

    // Map exactly the framebuffer's physical range; no PTE_FRAME_ALLOCATED so
    // teardown never frees GOP memory.
    for (size_t offset = 0; offset < len; offset += PAGE_SIZE)
        page_map_to(directory, (uint64_t)addr + offset, fb_phys_base + offset, flags);

    // A live raw framebuffer mapping is readable without further syscalls.
    // The kernel text console must stop drawing into the same scanout; kernel
    // logs continue on serial while the user-space console host owns the fb.
    // Registration is a no-op for trusted root/System mappings (such as the
    // console host's boot-time map), which do not reserve secret capture mode.
    console_suspend_fb();
    screen_capture_register_mapping();
    return addr;
}

bool fb_build_geometry()
{
    if (fbc_addr == NULL || fbc_addr->frame_buffer == nullptr) return false;

    uint64_t phys = (uint64_t)virt_to_phys((uint64_t)fbc_addr->frame_buffer);
    if (phys == 0) return false;
    if ((phys & (PAGE_SIZE - 1)) != 0)
    {
        write_serial_fmt("fb: framebuffer base %#llx is not page aligned\n",
                         (unsigned long long)phys);
        return false;
    }

    if (fbc_addr->pixels_per_scan_line < fbc_addr->horizontal_resolution) return false;
    if (fbc_addr->horizontal_resolution == 0 || fbc_addr->vertical_resolution == 0) return false;

    uint64_t line = (uint64_t)fbc_addr->pixels_per_scan_line * FB_BYTES_PER_PIXEL;
    uint64_t size = line * fbc_addr->vertical_resolution;
    if (line == 0 || size == 0 || size > 0xffffffffULL) return false;

    fb_config    = fbc_addr;
    fb_phys_base = phys;
    fb_line_len  = (uint32_t)line;
    fb_smem_len  = (size_t)size;
    fb_ready     = true;
    return true;
}
} // namespace

void fb_init()
{
    if (fb_ready) return;
    if (!fb_build_geometry())
    {
        write_serial_fmt("fb: no usable GOP framebuffer\n");
        return;
    }

    device_t dev = {};
    dev.type        = DEVICE_FB;
    dev.flag        = 1;
    dev.sector_size = PAGE_SIZE;
    dev.size        = fb_smem_len;
    dev.max_size    = fb_smem_len;
    dev.map         = fb_map;
    dev.ioctl       = fb_ioctl;
    strcpy(dev.drive_name, "fb0");

    int id = regist_device(NULL, dev);
    if (id < 0)
    {
        fb_ready = false;
        write_serial_fmt("fb: cannot register /dev/fb0\n");
        return;
    }

    write_serial_fmt("fb: /dev/fb0 %ux%u stride %u, %u bytes at %#llx\n",
                     fbc_addr->horizontal_resolution, fbc_addr->vertical_resolution,
                     fb_line_len, (unsigned)fb_smem_len, (unsigned long long)fb_phys_base);
}
