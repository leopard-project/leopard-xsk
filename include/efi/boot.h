#ifndef _BOOT_H_
#define _BOOT_H_
#include <efi/efi.h>
#include <mm/memory.h>
#include <efi/root_binding.h>

typedef enum
{
    BOOT_FLAG_NONE             = 0,
    BOOT_FLAG_SAFE_MODE        = 1u << 0,
    BOOT_FLAG_VERBOSE          = 1u << 1,
    BOOT_FLAG_DISABLE_KMOD     = 1u << 2,
    BOOT_FLAG_SAFE_STORAGE_IO  = 1u << 3,
    BOOT_FLAG_BASE_VIDEO       = 1u << 4,
    BOOT_FLAG_LAST_KNOWN_GOOD  = 1u << 5,
    BOOT_FLAG_SHOW_PROGRESS    = 1u << 6,
    BOOT_FLAG_INSTALLER        = 1u << 7,
    BOOT_FLAG_INITRD           = 1u << 8, /* 引导器已把 initrd/ramdisk 载入 RAM */
    BOOT_FLAG_BOUND_ROOT       = 1u << 9, /* ESP root.bin 指定 ext4/FAT32 根分区 */
    BOOT_FLAG_QEMU             = 1u << 10
} boot_flags_t;

/* Variable-length command line at the tail. cmdline_size includes the NUL. */
#define BOOT_CONFIG_CMDLINE_MAX 4096
#define BOOT_CONFIG_ABI_VERSION 1u

typedef struct
{
    MEMORY_MAP MemoryMap;        /* 36 bytes, packed UEFI memory map */
    uint32_t   layout;           /* (version << 16) | sizeof(BOOT_CONFIG) */
    uint32_t   boot_flags;       /* BOOT_FLAG_* */
    uint32_t   cmdline_size;     /* bytes in cmdline[], including the NUL */

    uint64_t FADT;
    uint64_t MADT;
    uint64_t HPET;
    uint64_t MCFG;

    /* Generic initrd/ramdisk loaded by the bootloader (XJPAK10 for now). */
    uint64_t initrd_phys;
    uint64_t initrd_size;

    uint64_t *saved_mtrrs;

    XJ_ROOT_BINDING root_binding; /* BOOT_FLAG_BOUND_ROOT 时由引导器填入 */
    char            cmdline[];    /* flexible tail: "init=... flags=..." */
} BOOT_CONFIG;

#define BOOT_CONFIG_LAYOUT ((BOOT_CONFIG_ABI_VERSION << 16) | (uint32_t)sizeof(BOOT_CONFIG))

static inline int boot_config_is_qemu(const BOOT_CONFIG *config)
{
    return config != 0 && (config->boot_flags & BOOT_FLAG_QEMU) != 0;
}

#if defined(__cplusplus)
#define BOOT_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define BOOT_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif
#define BOOT_OFFSETOF(t, m) __builtin_offsetof(t, m)
BOOT_STATIC_ASSERT(sizeof(MEMORY_MAP) == 36, "MEMORY_MAP must stay 36 bytes");
BOOT_STATIC_ASSERT(BOOT_OFFSETOF(BOOT_CONFIG, layout) == 36, "layout must fill MEMORY_MAP tail");
BOOT_STATIC_ASSERT(BOOT_OFFSETOF(BOOT_CONFIG, cmdline_size) == 44, "cmdline_size offset");
BOOT_STATIC_ASSERT(BOOT_OFFSETOF(BOOT_CONFIG, FADT) == 48, "FADT offset");
BOOT_STATIC_ASSERT(BOOT_OFFSETOF(BOOT_CONFIG, initrd_phys) == 80, "initrd offset");
BOOT_STATIC_ASSERT(BOOT_OFFSETOF(BOOT_CONFIG, root_binding) == 104, "root_binding offset");
BOOT_STATIC_ASSERT(BOOT_OFFSETOF(BOOT_CONFIG, cmdline) == 200, "cmdline offset");
BOOT_STATIC_ASSERT(sizeof(BOOT_CONFIG) == 200, "BOOT_CONFIG header size");
BOOT_STATIC_ASSERT(sizeof(BOOT_CONFIG) % 8 == 0, "BOOT_CONFIG alignment");
#undef BOOT_STATIC_ASSERT
#undef BOOT_OFFSETOF

#endif
