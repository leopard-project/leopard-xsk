#pragma once
// SPDX-License-Identifier: Apache-2.0
// Leopard glue surface for the native ext4 core. Formatting lives in the
// user-space mkfs-ext4 tool, not in the kernel.

#include <efi/root_binding.h>
#include <fs/vfs/vfs.h>

struct stat;
struct statfs;
struct timespec;

void ext4fs_init();
// Mount the bound system root (ext4 UUID + kernel CRC are validated).
int ext4fs_mount_bound_root(const char *device, const XJ_ROOT_BINDING *binding);
int ext4fs_finish_all();
void ext4fs_quiescent_exit();
bool ext4fs_owns_node(vfs_node_t node);
int ext4fs_stat(vfs_node_t node, struct stat *result);
int ext4fs_statfs(vfs_node_t node, struct statfs *result);
int ext4fs_sync(vfs_node_t node);
int ext4fs_sync_all();
int ext4fs_chmod(vfs_node_t node, uint16_t mode);
int ext4fs_chown(vfs_node_t node, uint32_t uid, uint32_t gid);
int ext4fs_utimens(vfs_node_t node, const struct timespec *times);
