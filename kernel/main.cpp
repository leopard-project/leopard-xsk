#include "build_settings.h"
#include <ahci/ahci.h>
#include <cpu/regio.h>
#include <device.h>
#include <cpu/fsgsbase.h>
#include <console.h>
#include <dlinker.h>
#include <efi/boot.h>
#include <efi/efi.h>
#include <efi/fbc.h>
#include <fs/fatfs/fatfs.h>
#include <fs/partition.h>
#include <fs/vfs/devfs.h>
#include <fs/ext4/ext4.h>
#include <fs/vfs/vfs.h>
#include <hda/hda.h>
#include <hda/pcspk.h>
#include <krlibc.h>
#include <mm/alloc/alloc.h>
#include <mm/frame.h>
#include <nvme/nvme.h>
#include <openxj380/config.h>
#include <openxj380/socket.h>
#include <openxj380/syscall.h>
#include <pci/pci.h>
#include <pipe.h>
#include <power.h>
#include <proto.hpp>
#include <pty.h>
#include <ps2/keyboard.h>
#include <ps2/mouse.h>
#include <rtc.h>
#include <sb16.h>
#include <syscall/signal.h>
#include <syscall/pxapi.h>
#include <syscall/syscall.h>
#include <task/pcb.h>
#include <task/scheduler.h>
#include <user/permissions.h>
#include <user/runfile.h>
#include <user/settings.h>
#include <user/info_register.h>
#include <hda/vsound.h>
#include <hda/hda.h>
#include <initrd.h>
#include <x3tp.h>

const FrameBufferConfig *fbc_addr;

extern volatile int  scheduler_is_ready;
extern XSK_SMP_INFO *xsi;
#define NULL 0
uint64_t *saved_mtrrs;
bool      no_interrupt = false;

static OpenXJ380MouseInterruptHook g_openxj380_mouse_hook = NULL;
static OpenXJ380KeyboardInterruptHook g_openxj380_keyboard_hook = NULL;
static OpenXJ380SyscallHook g_openxj380_syscall_hook = NULL;
static OpenXJ380ProcessExitHook g_openxj380_process_exit_hook = NULL;
extern EFI_SYSTEM_TABLE *EFI_ST;
extern BOOT_CONFIG *EFI_BC;

extern "C" int OpenXJ380Socket_RegisterMouseHook(OpenXJ380MouseInterruptHook hook)
{
#if !OPENXJ380_INPUT_OUTPUT_DISABLED
    if (hook == NULL) return -1;
    OpenXJ380MouseInterruptHook expected = NULL;
    return __atomic_compare_exchange_n(&g_openxj380_mouse_hook, &expected, hook, false, __ATOMIC_RELEASE,
                                       __ATOMIC_RELAXED)
               ? 0
               : -1;
#else
    (void)hook;
    return -1;
#endif
}

extern "C" int OpenXJ380Socket_RegisterKeyboardHook(OpenXJ380KeyboardInterruptHook hook)
{
#if !OPENXJ380_INPUT_OUTPUT_DISABLED
    if (hook == NULL) return -1;
    OpenXJ380KeyboardInterruptHook expected = NULL;
    return __atomic_compare_exchange_n(&g_openxj380_keyboard_hook, &expected, hook, false, __ATOMIC_RELEASE,
                                       __ATOMIC_RELAXED)
               ? 0
               : -1;
#else
    (void)hook;
    return -1;
#endif
}

extern "C" void OpenXJ380Socket_UnregisterMouseHook(OpenXJ380MouseInterruptHook hook)
{
#if !OPENXJ380_INPUT_OUTPUT_DISABLED
    OpenXJ380MouseInterruptHook expected = hook;
    __atomic_compare_exchange_n(&g_openxj380_mouse_hook, &expected, NULL, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
#else
    (void)hook;
#endif
}

extern "C" void OpenXJ380Socket_UnregisterKeyboardHook(OpenXJ380KeyboardInterruptHook hook)
{
#if !OPENXJ380_INPUT_OUTPUT_DISABLED
    OpenXJ380KeyboardInterruptHook expected = hook;
    __atomic_compare_exchange_n(&g_openxj380_keyboard_hook, &expected, NULL, false, __ATOMIC_RELEASE,
                                __ATOMIC_RELAXED);
#else
    (void)hook;
#endif
}

extern "C" bool OpenXJ380Socket_MouseInterrupte(const OpenXJ380MouseInterruptInfo *event)
{
#if !OPENXJ380_INPUT_OUTPUT_DISABLED
    OpenXJ380MouseInterruptHook hook = __atomic_load_n(&g_openxj380_mouse_hook, __ATOMIC_ACQUIRE);
    return hook != NULL && event != NULL && hook(event);
#else
    (void)event;
    return false;
#endif
}

extern "C" bool OpenXJ380Socket_KeyboardInterrupt(const OpenXJ380KeyboardInterruptInfo *event)
{
#if !OPENXJ380_INPUT_OUTPUT_DISABLED
    OpenXJ380KeyboardInterruptHook hook = __atomic_load_n(&g_openxj380_keyboard_hook, __ATOMIC_ACQUIRE);
    return hook != NULL && event != NULL && hook(event);
#else
    (void)event;
    return false;
#endif
}

extern "C" const void *OpenXJ380Socket_FramebufferConfig()
{
    return fbc_addr;
}

extern "C" uint64_t OpenXJ380Socket_PowerAction(uint64_t action)
{
#if OPENXJ380_INPUT_OUTPUT_DISABLED
    (void)action;
    return (uint64_t)-1;
#else
    if (EFI_ST == NULL || EFI_BC == NULL) return (uint64_t)-1;
    if (action == XPOWER_REBOOT) power_reboot(EFI_ST, EFI_BC);
    if (action == XPOWER_SHUTDOWN) power_shutdown(EFI_ST, EFI_BC);
    return (uint64_t)-1;
#endif
}

extern "C" int OpenXJ380Socket_RegisterSyscallHook(OpenXJ380SyscallHook hook)
{
#if !OPENXJ380_GUI_DISABLED
    if (hook == NULL) return -1;
    OpenXJ380SyscallHook expected = NULL;
    return __atomic_compare_exchange_n(&g_openxj380_syscall_hook, &expected, hook, false, __ATOMIC_RELEASE,
                                       __ATOMIC_RELAXED)
               ? 0
               : -1;
#else
    (void)hook;
    return -1;
#endif
}

extern "C" void OpenXJ380Socket_UnregisterSyscallHook(OpenXJ380SyscallHook hook)
{
#if !OPENXJ380_GUI_DISABLED
    OpenXJ380SyscallHook expected = hook;
    __atomic_compare_exchange_n(&g_openxj380_syscall_hook, &expected, NULL, false, __ATOMIC_RELEASE,
                                __ATOMIC_RELAXED);
#else
    (void)hook;
#endif
}

extern "C" bool OpenXJ380Socket_DispatchSyscall(uint64_t syscall_number, struct X64_REGS *regs)
{
#if !OPENXJ380_GUI_DISABLED
    OpenXJ380SyscallHook hook = __atomic_load_n(&g_openxj380_syscall_hook, __ATOMIC_ACQUIRE);
    return hook != NULL && regs != NULL && hook(syscall_number, regs);
#else
    (void)syscall_number;
    (void)regs;
    return false;
#endif
}

extern "C" int OpenXJ380Socket_RegisterProcessExitHook(OpenXJ380ProcessExitHook hook)
{
#if !OPENXJ380_GUI_DISABLED
    if (hook == NULL) return -1;
    OpenXJ380ProcessExitHook expected = NULL;
    return __atomic_compare_exchange_n(&g_openxj380_process_exit_hook, &expected, hook, false, __ATOMIC_RELEASE,
                                       __ATOMIC_RELAXED)
               ? 0
               : -1;
#else
    (void)hook;
    return -1;
#endif
}

extern "C" void OpenXJ380Socket_UnregisterProcessExitHook(OpenXJ380ProcessExitHook hook)
{
#if !OPENXJ380_GUI_DISABLED
    OpenXJ380ProcessExitHook expected = hook;
    __atomic_compare_exchange_n(&g_openxj380_process_exit_hook, &expected, NULL, false, __ATOMIC_RELEASE,
                                __ATOMIC_RELAXED);
#else
    (void)hook;
#endif
}

extern "C" void OpenXJ380Socket_NotifyProcessExit(void *process)
{
#if !OPENXJ380_GUI_DISABLED
    OpenXJ380ProcessExitHook hook = __atomic_load_n(&g_openxj380_process_exit_hook, __ATOMIC_ACQUIRE);
    if (hook != NULL && process != NULL) hook(process);
#else
    (void)process;
#endif
}


extern bool allow_to_flush;
extern void ahci_set_accel(bool enabled);
extern bool ahci_is_qemu_environment();
extern BOOT_CONFIG *EFI_BC;

extern UserInfo *current_user;

void init_cpu()
{
    __asm__ __volatile__("movq %%cr0, %%rax\n\t"
                         "and $0xFFF3, %%ax	\n\t" // clear coprocessor emulation CR0.EM and CR0.TS
                         "or $0x2, %%ax\n\t"      // set coprocessor monitoring  CR0.MP
                         "movq %%rax, %%cr0\n\t"
                         "movq %%cr4, %%rax\n\t"
                         "or $(3 << 9), %%ax\n\t" // set CR4.OSFXSR and CR4.OSXMMEXCPT at the same time
                         "movq %%rax, %%cr4\n\t" ::
                             : "rax");

    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
    if (ecx & (1U << 26))
    {
        uint64_t cr4 = 0;
        __asm__ __volatile__("movq %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ULL << 18); // CR4.OSXSAVE: allow xgetbv/xsetbv/xsave in user-visible CPU feature paths.
        __asm__ __volatile__("movq %0, %%cr4" ::"r"(cr4) : "memory");

        uint32_t xcr0_lo = 0x3; // x87 + SSE only; context switching still uses fxsave/fxrstor.
        uint32_t xcr0_hi = 0;
        __asm__ __volatile__("xsetbv" ::"c"(0), "a"(xcr0_lo), "d"(xcr0_hi) : "memory");
    }

    if (has_fsgsbase())
    {
        uint64_t cr4 = 0;
        __asm__ __volatile__("movq %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ULL << 16);
        __asm__ __volatile__("movq %0, %%cr4" ::"r"(cr4) : "memory");
        read_fsbase  = rdfsbase;
        write_fsbase = wrfsbase;
        read_gsbase  = rdgsbase;
        write_gsbase = wrgsbase;
    }
}

static constexpr uint64_t KERNEL_HEAP_EXTEND_CHUNK = 64UL * 1024UL * 1024UL;
static constexpr uint64_t KERNEL_HEAP_MAX_BYTES    = 2048UL * 1024UL * 1024UL;

static uint64_t kernel_heap_mapped_bytes = 0;
static spin_t   kernel_heap_extend_lock  = SPIN_INIT;
static bool     kernel_heap_initialized  = false;

static uint64_t align_up_u64(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

void init_heap()
{
    page_map_range_to_random(get_kernel_pagedir(), KERNEL_HEAP_BASE, KERNEL_HEAP_BYTES, KERNEL_PTE_FLAGS);
    memset((void *)KERNEL_HEAP_BASE, 0, KERNEL_HEAP_BYTES);
    heap_init((uint8_t *)KERNEL_HEAP_BASE, KERNEL_HEAP_BYTES);
    kernel_heap_mapped_bytes = KERNEL_HEAP_BYTES;
    kernel_heap_initialized  = true;
}

bool kernel_heap_ready()
{
    return kernel_heap_initialized;
}

bool kernel_heap_extend(size_t min_bytes)
{
    if (!kernel_heap_initialized) return false;

    uint64_t extend_bytes = align_up_u64((uint64_t)min_bytes, PAGE_SIZE);
    if (extend_bytes < KERNEL_HEAP_EXTEND_CHUNK) extend_bytes = KERNEL_HEAP_EXTEND_CHUNK;

    spin_lock(&kernel_heap_extend_lock);

    if (kernel_heap_mapped_bytes + extend_bytes > KERNEL_HEAP_MAX_BYTES)
    {
        spin_unlock(&kernel_heap_extend_lock);
        write_serial_string("Kernel heap extend refused: limit reached.\n");
        return false;
    }

    uint64_t extend_base = KERNEL_HEAP_BASE + kernel_heap_mapped_bytes;
    uint64_t mapped      = 0;
    for (; mapped < extend_bytes; mapped += PAGE_SIZE)
    {
        uint64_t frame = alloc_frames(1);
        if (frame == 0)
        {
            unmap_page_range(get_kernel_pagedir(), extend_base, mapped);
            spin_unlock(&kernel_heap_extend_lock);
            write_serial_string("Kernel heap extend failed: cannot allocate frames.\n");
            return false;
        }
        memset(phys_to_virt(frame), 0, PAGE_SIZE);
        page_map_to(get_kernel_pagedir(), extend_base + mapped, frame, KERNEL_PTE_FLAGS | PTE_FRAME_ALLOCATED);
    }
    memset((void *)extend_base, 0, extend_bytes);

    bool ok = heap_extend((uint8_t *)extend_base, extend_bytes);
    if (ok)
    {
        kernel_heap_mapped_bytes += extend_bytes;
        write_serial_string("Kernel heap extended by ");
        write_serial_dec(extend_bytes / 1024 / 1024);
        write_serial_string(" MiB, total ");
        write_serial_dec(kernel_heap_mapped_bytes / 1024 / 1024);
        write_serial_string(" MiB.\n");
    }
    else
    {
        unmap_page_range(get_kernel_pagedir(), extend_base, extend_bytes);
        write_serial_string("Kernel heap extend failed.\n");
    }

    spin_unlock(&kernel_heap_extend_lock);
    return ok;
}

extern void     nvme_setup();
#if CONFIG_KERNEL_BUILTIN_XHCI && !OPENXJ380_INPUT_OUTPUT_DISABLED
extern int      xhci_setup();
extern void     xhci_start_workers();
#endif
extern void     mount_root();
extern void     stdio_init();
extern void     fb_init();
extern void     vkbd_init();
extern int      socketfs_setup();
extern int      procfs_setup();
extern int      dnsfs_setup();
extern int      nmfs_setup();
extern int      tmpfs_setup();
EFI_SYSTEM_TABLE *EFI_ST;
BOOT_CONFIG *EFI_BC;
static BOOT_CONFIG  g_kernel_boot_config;
static const char  *g_boot_cmdline;
static uint32_t     g_boot_cmdline_size;

static const char *boot_cmdline_value(const char *cmdline, uint32_t cmdline_size,
                                      const char *key, char *out, size_t capacity)
{
    if (cmdline == NULL || capacity == 0 || cmdline_size == 0) return NULL;

    const char *p    = cmdline;
    const char *end  = p + cmdline_size;
    const size_t key_len = strlen(key);

    while (p < end && *p != '\0')
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
        if (p >= end || *p == '\0') break;
        if (*p == '#')
        {
            while (p < end && *p != '\n') ++p;
            continue;
        }

        const char *line_end = p;
        while (line_end < end && *line_end != '\n') ++line_end;

        const char *eq = p;
        while (eq < line_end && *eq != '=') ++eq;
        if (eq < line_end)
        {
            const char *key_end = eq;
            while (key_end > p && (key_end[-1] == ' ' || key_end[-1] == '\t')) --key_end;
            if ((size_t)(key_end - p) == key_len && memcmp(p, key, key_len) == 0)
            {
                const char *value = eq + 1;
                while (value < line_end && (*value == ' ' || *value == '\t')) ++value;
                const char *value_end = line_end;
                while (value_end > value &&
                       (value_end[-1] == ' ' || value_end[-1] == '\t' || value_end[-1] == '\r')) --value_end;
                const char *hash = value;
                while (hash < value_end && *hash != '#') ++hash;
                value_end = hash;
                while (value_end > value &&
                       (value_end[-1] == ' ' || value_end[-1] == '\t' || value_end[-1] == '\r')) --value_end;
                if (value_end > value + 1 && value[0] == '"' && value_end[-1] == '"')
                {
                    ++value;
                    --value_end;
                }

                size_t length = (size_t)(value_end - value);
                if (length >= capacity) length = capacity - 1;
                memcpy(out, value, length);
                out[length] = '\0';
                return out;
            }
        }

        p = line_end;
        if (p < end) ++p;
    }
    return NULL;
}

extern "C" void KernelMain(const FrameBufferConfig &fbc, EFI_SYSTEM_TABLE &SystemTable, BOOT_CONFIG &BootConfigIn)
{
    uint32_t cmdline_size = BootConfigIn.cmdline_size;
    bool     cmdline_invalid = cmdline_size > BOOT_CONFIG_CMDLINE_MAX;
    if (cmdline_invalid) cmdline_size = 0;

    /* Copy only the fixed header. The cmdline tail stays in the bootloader's
       HHDM pool allocation, which is reserved in the UEFI memory map and is
       never reclaimed by the kernel; avoid a 4 KiB per-kernel static copy. */
    memcpy(&g_kernel_boot_config, &BootConfigIn, sizeof(BOOT_CONFIG));
    g_kernel_boot_config.cmdline_size = cmdline_size;
    g_boot_cmdline_size = cmdline_size;
    g_boot_cmdline = cmdline_size != 0 ? BootConfigIn.cmdline : NULL;
    BOOT_CONFIG &BootConfig = g_kernel_boot_config;

    fbc_addr = &fbc;
    console_init(fbc);
    if (BootConfig.layout != BOOT_CONFIG_LAYOUT)
    {
        write_serial_fmt("BOOT: config layout 0x%x, expected 0x%x; rebuild bootloader and kernel together\n",
                         BootConfig.layout, BOOT_CONFIG_LAYOUT);
        for (;;) __asm__ volatile("cli; hlt" ::: "memory");
    }
    if (cmdline_invalid) write_serial_string("BOOT: cmdline too large, ignored\n");
    no_interrupt = true;
    disable_intr();
    EFI_ST = &SystemTable;
    EFI_BC = &BootConfig;

    init_cpu();
    init_idt();
    init_gdt();
    init_hpet(BootConfig.HPET);
    init_apic(BootConfig.MADT);

    init_hhdm();
    init_frame(BootConfig.MemoryMap);
    init_heap();

    write_serial_string("BOOT: device_manager_init begin\n");
    device_manager_init();
    write_serial_string("BOOT: device_manager_init done\n");
    write_serial_string("BOOT: vfs_init begin\n");
    vfs_init();
    write_serial_string("BOOT: vfs_init done\n");
    write_serial_string("BOOT: fatfs_init begin\n");
    fatfs_init();
    write_serial_string("BOOT: fatfs_init done\n");
    write_serial_string("BOOT: devfs_setup begin\n");
    devfs_setup();
    write_serial_string("BOOT: devfs_setup done\n");

    write_serial_string("BOOT: pci_setup begin\n");
    pci_setup(BootConfig.MCFG);
    write_serial_string("BOOT: pci_setup done\n");
    write_serial_string("BOOT: pci_init begin\n");
    pci_init();
    write_serial_string("BOOT: pci_init done\n");
    write_serial_fmt("VM hint: %s\n",
                     boot_config_is_qemu(&BootConfig) ? "QEMU/BOCHS" : "generic/VMware/real hardware");
    if (BootConfig.boot_flags != 0)
        write_serial_fmt("Boot option flags: 0x%llx\n", (unsigned long long)BootConfig.boot_flags);
    if ((BootConfig.boot_flags & BOOT_FLAG_LAST_KNOWN_GOOD) != 0)
        write_serial_string("BOOT: last known good configuration selected\n");
    ahci_set_environment(boot_config_is_qemu(&BootConfig));
    write_serial_string("BOOT: ahci_setup begin\n");
    ahci_setup();
    write_serial_string("BOOT: ahci_setup done\n");
    ahci_set_accel(false);
    write_serial_string("BOOT: nvme_setup begin\n");
    nvme_setup();
    write_serial_string("BOOT: nvme_setup done\n");

    extern void ide_setup(void);
    write_serial_string("BOOT: ide_setup begin\n");
    ide_setup();
    write_serial_string("BOOT: ide_setup done\n");
#if CONFIG_KERNEL_BUILTIN_XHCI && !OPENXJ380_INPUT_OUTPUT_DISABLED
    write_serial_string("BOOT: xhci_setup begin\n");
    xhci_setup();
    write_serial_string("BOOT: xhci_setup done\n");
#endif
    write_serial_string("BOOT: partition_init begin\n");
    partition_init();
    write_serial_string("BOOT: partition_init done\n");
    // enable_intr();

    // disable_intr();
#if !OPENXJ380_INPUT_OUTPUT_DISABLED
    // HDA 驱动现在会在初始化阶段自行完成注册，这里只需要启动探测即可。
    hda_init();
    hda_regist();
#endif
    // sb16_init();

    // Input Core v2: static seat/endpoint/source tables must exist before the
    // first keyboard IRQ or USB worker can enqueue an event.
    x3tp_init();

    keyboard_init();
    mouse_init();
    disable_intr();

    process_setup();

    saved_mtrrs = (uint64_t *)((uint64_t)BootConfig.saved_mtrrs + 0xffff800000000000);

    init_smp(BootConfig.MADT);

    const size_t idle_alloc_size = (sizeof(struct thread_control_block) + 15ULL) & ~15ULL;
    tcb_t idle_thread            = (tcb_t)aligned_alloc(16, idle_alloc_size);
    if (idle_thread != NULL) memset(idle_thread, 0, sizeof(struct thread_control_block));
    if (idle_thread == NULL) while (true) { __asm__ volatile("hlt"); }
    idle_thread->tid          = __atomic_fetch_add(&now_tid, 1, __ATOMIC_SEQ_CST);
    idle_thread->parent_group = kernel_group;
    idle_thread->kernel_stack = get_rsp();
    idle_thread->status       = RUNNING;
    strcpy(idle_thread->name, "idle thread");
    idle_thread->fs           = 0x10;
    idle_thread->fs_base      = 0;
    idle_thread->group_index  = queue_enqueue(kernel_group->thread_queue, idle_thread);
    idle_thread->context0.rsp = get_rsp();
    set_kernel_stack(get_rsp());
    idle_thread->queue_index = queue_enqueue_ref(get_current_cpu()->scheduler_queue, idle_thread, &idle_thread->sched_node);

    idle_thread->str_cwd = "/";
    idle_thread->cwd     = rootdir;
    save_fpu_context(&idle_thread->fpu_context);

    get_current_cpu()->current_task = idle_thread;
    write_kgsbase((uint64_t)get_current_cpu());

    memset(phys_to_virt(get_cr3()), 0, PAGE_SIZE / 2);

#if CONFIG_KERNEL_BUILTIN_XHCI && !OPENXJ380_INPUT_OUTPUT_DISABLED
    xhci_start_workers();
#endif

    ext4fs_init();
    bool initrd_mode = initrd_boot_active(BootConfig);
    if (initrd_mode)
        initrd_prepare_root(BootConfig);
    else
        mount_root();
    if (!initrd_root_is_tmpfs_ready()) tmpfs_setup();
    // Permission policy is boot-time config: the kernel owns the mechanism,
    // permissions.conf owns grade masks and public/protected path sets.
    // Fail closed: never continue into PID1 with an unloaded policy.
    if (!permissions_init("/system/config/permissions.conf"))
    {
        write_serial_string("BOOT: FATAL: permissions policy unavailable; refusing to start\n");
        disable_intr();
        for (;;) __asm__ volatile("hlt");
    }
    pipefs_setup();
    pty_init();
    fsgsbase_init();
    socketfs_setup();
    procfs_setup();
    dnsfs_setup();
    nmfs_setup();
    stdio_init();
    fb_init();
    vkbd_init();
    x3tp_console_device_init();

    signal_init();
    init_syscall();
    init_message();

    if (!OPENXJ380_INPUT_OUTPUT_DISABLED &&
        (BootConfig.boot_flags & (BOOT_FLAG_SAFE_MODE | BOOT_FLAG_DISABLE_KMOD | BOOT_FLAG_INSTALLER)) == 0) {
        module_setup();
        dlinker_init();
        load_all_kernel_module();
    } else {
        write_serial_string("BOOT: kernel modules skipped by boot menu\n");
    }

    while (true)
    {
        __asm__ volatile("pause");
        if (__atomic_load_n(&scheduler_is_ready, __ATOMIC_ACQUIRE) ==
            __atomic_load_n(&xsi->cpu_count, __ATOMIC_ACQUIRE))
            break;
    }

    init_reaper();
    write_serial_string("Process Reaper is ready\n");

    if (boot_config_is_qemu(&BootConfig)) pr_warn("XJ380 is running under the QEMU/BOCHS.\n");

    // create_kernel_thread((void *)tst, NULL, (char *)"1", NULL);
    if ((BootConfig.boot_flags & (BOOT_FLAG_SAFE_MODE | BOOT_FLAG_SAFE_STORAGE_IO)) != 0) {
        write_serial_string("AHCI: keeping conservative IO path by boot menu\n");
    } else if (ahci_is_qemu_environment()) {
        ahci_set_accel(true);
    } else {
        write_serial_string("AHCI: keeping conservative IO path on vmware/real hardware\n");
    }
    user_session_use_root();
    /* PID1 由 cmdline 的 init= 指定（boot.conf），内核不再硬编码；空则回退默认 */
    char        init_path[512];
    const char *configured_init = boot_cmdline_value(g_boot_cmdline, g_boot_cmdline_size,
                                                     "init", init_path, sizeof(init_path));
    if (configured_init != NULL && configured_init[0] == '\0') configured_init = NULL;
    const char *init = configured_init != NULL ? configured_init : "/system/leopard/leopard-rc.elf";
    write_serial_fmt("BOOT: launching init %s\n", init);
    create_user_process_from_file((char *)init, NULL, NULL);


    // delay_s_hp(60);

    // uint64_t utsk = page_alloc_random(get_current_directory(), 114, PTE_PRESENT | PTE_USER);
    // memcpy((void *)utsk, (void *)test_task, 114);
    // create_user_thread((void *)utsk, NULL, (char *)"test_task2", ugp);


    // Publish runtime IRQ policy before any CPU can dispatch a worker. A
    // pending timer can preempt immediately after STI, before the next store.
    no_interrupt = false;
    scheduler_start();
    open_interrupt;

    // no_interrupt = true;
    // close_interrupt;
    // disable_scheduler();

    // enable_scheduler();
    // open_interrupt;
    // no_interrupt = false;

    while (true)
    {
        if (!no_interrupt)
        {
            enable_intr();
        }

        __asm__ __volatile__("pause");
    }
}
