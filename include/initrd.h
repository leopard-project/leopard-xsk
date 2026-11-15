#pragma once

#include <efi/boot.h>
#include <fs/vfs/vfs.h>
#include <installer_protocol.h>

/* Generic initrd/ramdisk support. The kernel only unpacks the image to tmpfs;
 * deciding "live" vs "installer" is user-space policy (init/initrc). */
bool initrd_boot_active(const BOOT_CONFIG &boot_config);
int  initrd_prepare_root(const BOOT_CONFIG &boot_config);
bool initrd_root_is_tmpfs_ready();
const BOOT_CONFIG *initrd_current_boot_config();
vfs_node_t initrd_base_root();
