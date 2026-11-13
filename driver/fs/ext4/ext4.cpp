// SPDX-License-Identifier: Apache-2.0
// Leopard VFS glue for the native ext4 core (read/write, namespace, journal).
// Formatting is a user-space tool; installer-specific import paths are not
// part of the kernel.

#include "attributes.h"
#include "native.h"
#include "write.h"

#include <errno.h>
#include <fs/ext4/ext4.h>
#include <fs/vfs/vfs.h>
#include <ioctl.h>
#include <fs/vfs/devfs.h>
#include <syscall/syscall.h>
#include <krlibc.h>
#include <mutex.h>
#include <rtc.h>
#include <task/pcb.h>
#include <task/scheduler.h>

namespace
{
struct Mount;

struct Node
{
    Mount *mount;
    xjext4::Inode inode;
    uint32_t parent_inode;
    vfs_node_t vnode;
};

struct Mount
{
    vfs_node_t device;
    xjext4::WriteSession *writer;
    xjext4::WriteWorkspace *workspace;
    xjext4::WriteStorage storage;
    uint64_t bytes;
    bool writable;
    int fsid;
    Mount *next;
};

mutex_t operation_lock;
bool initialized;
Mount *mounts;
int filesystem_id;

void lock() { if (initialized) mutex_lock(&operation_lock); }
void unlock() { if (initialized) mutex_unlock(&operation_lock); }

uint64_t seconds_now() { return realtime_ns() / 1000000000ULL; }

uint16_t node_type(const xjext4::Inode &inode)
{
    if ((inode.mode & 0170000) == 0040000) return file_dir;
    return file_none;
}

void apply_inode(Mount *mount, vfs_node_t node, const xjext4::Inode &inode)
{
    if (node == NULL) return;
    uint16_t type = node_type(inode);
    node->type = type;
    node->size = type == file_dir ? 0 : inode.size;
    node->realsize = inode.blocks_512 * 512;
    node->inode = inode.number;
    node->blksz = mount->writer->view().block_size();
    node->owner = inode.uid;
    node->group = inode.gid;
    node->permissions = inode.mode & 07777;
    node->fsid = (uint16_t)mount->fsid;
    if (node->root != NULL) node->dev = node->root->dev;
}

bool read_device(void *context, uint64_t offset, void *buffer, xjext4::Size size)
{
    Mount *mount = (Mount *)context;
    return vfs_read(mount->device, buffer, offset, size) == size;
}

bool write_device(void *context, uint64_t offset, const void *buffer, xjext4::Size size)
{
    Mount *mount = (Mount *)context;
    return vfs_write(mount->device, (void *)buffer, offset, size) == size;
}

bool flush_device(void *context)
{
    Mount *mount = (Mount *)context;
    return vfs_ioctl(mount->device, IOBLKSYNC, NULL) == 0;
}

int validate_root(void *context, const xjext4::Volume &volume)
{
    const XJ_ROOT_BINDING *binding = (const XJ_ROOT_BINDING *)context;
    if (binding == NULL) return 0;
    if (memcmp(volume.uuid(), xj_root_binding_ext4_uuid(binding), 16)) return -EUCLEAN;

    xjext4::Inode root, system, kernel;
    int error = volume.read_inode(2, root);
    if (!error) error = volume.lookup(root, "system", system);
    if (!error && (system.mode & 0170000) != 0040000) error = -EUCLEAN;
    if (!error) error = volume.lookup(system, "kernel.krl", kernel);
    if (error) return error;
    if ((kernel.mode & 0170000) != 0100000 ||
        kernel.size != xj_root_binding_kernel_bytes(binding)) return -EUCLEAN;

    uint8_t buffer[8192];
    uint32_t checksum = 0xffffffffU;
    for (uint64_t offset = 0; offset < kernel.size;)
    {
        xjext4::Size bytes = kernel.size - offset < sizeof(buffer) ?
                             xjext4::Size(kernel.size - offset) : sizeof(buffer);
        xjext4::Size completed = 0;
        error = volume.read(kernel, offset, buffer, bytes, completed);
        if (error || completed != bytes) return error ? error : -EIO;
        checksum = xj_root_binding_crc32c_update(checksum, buffer, bytes);
        offset += bytes;
    }
    return (checksum ^ 0xffffffffU) == xj_root_binding_kernel_crc32c(binding) ? 0 : -EUCLEAN;
}

int append_entry(void *context, const char *name, uint32_t number, uint8_t type)
{
    vfs_node_t parent = (vfs_node_t)context;
    if (name == NULL || name[0] == '\0') return 0;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;

    vfs_child_lock();
    vfs_node_t existing = NULL;
    list_foreach(parent->child, child_node)
    {
        vfs_node_t child = (vfs_node_t)child_node->data;
        if (child != NULL && !strcmp(child->name, name)) { existing = child; break; }
    }
    vfs_child_unlock();
    if (existing != NULL) return 0;

    vfs_node_t child = vfs_child_append(parent, name, NULL);
    if (child == NULL) return -ENOMEM;
    child->type = type == 2 ? file_dir : file_none;
    child->inode = number;
    child->fsid = parent->fsid;
    child->root = parent->root;
    child->dev = parent->dev;
    return 0;
}

int populate(Mount *mount, vfs_node_t node, const xjext4::Inode &inode)
{
    return mount->writer->view().list_directory(inode, append_entry, node);
}

Node *make_node(Mount *mount, const xjext4::Inode &inode, uint32_t parent_inode)
{
    Node *handle = (Node *)calloc(1, sizeof(Node));
    if (handle == NULL) return NULL;
    handle->mount = mount;
    handle->inode = inode;
    handle->parent_inode = parent_inode;
    return handle;
}

int lookup_child(Mount *mount, const xjext4::Inode &parent, const char *name, xjext4::Inode &out)
{
    return mount->writer->view().lookup(parent, name, out);
}

void ext4_open(void *parent, const char *name, vfs_node_t node)
{
    if (parent == NULL || name == NULL || node == NULL) return;
    Node *directory = (Node *)parent;
    lock();
    xjext4::Inode inode;
    int error = lookup_child(directory->mount, directory->inode, name, inode);
    if (error == 0)
    {
        Node *handle = make_node(directory->mount, inode, directory->inode.number);
        if (handle != NULL)
        {
            handle->vnode = node;
            node->handle = handle;
            apply_inode(directory->mount, node, inode);
            if (node_type(inode) == file_dir) populate(directory->mount, node, inode);
        }
    }
    unlock();
}

void ext4_close(void *handle)
{
    Node *node = (Node *)handle;
    if (node == NULL) return;
    free(node);
}

size_t ext4_read(void *handle, void *buffer, size_t offset, size_t size)
{
    Node *node = (Node *)handle;
    if (node == NULL || buffer == NULL) return VFS_STATUS_FAILED;
    lock();
    xjext4::Size completed = 0;
    int error = node->mount->writer->view().read(node->inode, offset, buffer, size, completed);
    unlock();
    if (error) return VFS_STATUS_FAILED;
    return completed;
}

size_t ext4_write(void *handle, const void *buffer, size_t offset, size_t size)
{
    Node *node = (Node *)handle;
    if (node == NULL || buffer == NULL) return VFS_STATUS_FAILED;
    lock();
    xjext4::Size completed = 0;
    int error = node->mount->writer->pwrite(node->inode.number, offset, buffer, size,
                                            seconds_now(), completed);
    if (!error)
    {
        xjext4::Inode refreshed;
        if (node->mount->writer->view().read_inode(node->inode.number, refreshed) == 0)
        {
            node->inode = refreshed;
            if (node->vnode != NULL) apply_inode(node->mount, node->vnode, refreshed);
        }
    }
    unlock();
    return error ? VFS_STATUS_FAILED : completed;
}

errno_t ext4_stat(void *handle, vfs_node_t node)
{
    Node *entry = (Node *)handle;
    if (entry == NULL || node == NULL) return VFS_STATUS_FAILED;
    lock();
    xjext4::Inode refreshed;
    int error = entry->mount->writer->view().read_inode(entry->inode.number, refreshed);
    if (!error)
    {
        entry->inode = refreshed;
        apply_inode(entry->mount, node, refreshed);
    }
    unlock();
    return error ? VFS_STATUS_FAILED : EOK;
}

int ext4_mkdir(void *parent, const char *name, vfs_node_t node)
{
    Node *directory = (Node *)parent;
    if (directory == NULL || name == NULL || node == NULL) return VFS_STATUS_FAILED;
    lock();
    xjext4::Inode created;
    int error = directory->mount->writer->create(directory->inode.number, name,
                                                0040755, 0, 0, seconds_now(), created);
    if (!error)
    {
        Node *handle = make_node(directory->mount, created, directory->inode.number);
        if (handle == NULL) error = -ENOMEM;
        else
        {
            handle->vnode = node;
            node->handle = handle;
            apply_inode(directory->mount, node, created);
        }
    }
    unlock();
    return error ? VFS_STATUS_FAILED : EOK;
}

int ext4_mkfile(void *parent, const char *name, vfs_node_t node)
{
    Node *directory = (Node *)parent;
    if (directory == NULL || name == NULL || node == NULL) return VFS_STATUS_FAILED;
    lock();
    xjext4::Inode created;
    int error = directory->mount->writer->create(directory->inode.number, name,
                                                0100644, 0, 0, seconds_now(), created);
    if (!error)
    {
        Node *handle = make_node(directory->mount, created, directory->inode.number);
        if (handle == NULL) error = -ENOMEM;
        else
        {
            handle->vnode = node;
            node->handle = handle;
            apply_inode(directory->mount, node, created);
        }
    }
    unlock();
    return error ? VFS_STATUS_FAILED : EOK;
}

int ext4_delete(void *parent, vfs_node_t node)
{
    Node *directory = (Node *)parent;
    Node *entry = node != NULL ? (Node *)node->handle : NULL;
    if (directory == NULL || entry == NULL || node->name == NULL) return VFS_STATUS_FAILED;
    lock();
    int error = directory->mount->writer->unlink(directory->inode.number, node->name,
                                                 seconds_now(), false);
    unlock();
    return error ? VFS_STATUS_FAILED : EOK;
}

int ext4_rename(void *handle, const char *path)
{
    Node *entry = (Node *)handle;
    if (entry == NULL || path == NULL) return VFS_STATUS_FAILED;

    const char *name = strrchr(path, '/');
    name = name != NULL ? name + 1 : path;
    if (name[0] == '\0') return VFS_STATUS_FAILED;

    char *parent_path = strdup(path);
    if (parent_path == NULL) return VFS_STATUS_FAILED;
    char *slash = strrchr(parent_path, '/');
    if (slash == parent_path) parent_path[1] = '\0';
    else if (slash != NULL) *slash = '\0';
    else strcpy(parent_path, ".");

    vfs_node_t current_parent = entry->vnode->parent;
    vfs_node_t new_parent = strcmp(parent_path, ".") ? vfs_open(parent_path) : current_parent;
    free(parent_path);
    if (new_parent == NULL || new_parent->handle == NULL) return VFS_STATUS_FAILED;
    Node *destination = (Node *)new_parent->handle;
    bool moved = new_parent != current_parent;

    lock();
    int error = entry->mount->writer->rename(entry->parent_inode, entry->vnode->name,
                                             destination->inode.number, name,
                                             seconds_now(), false);
    if (!error)
    {
        Node *handle_node = entry;
        handle_node->parent_inode = destination->inode.number;
        if (new_parent != entry->vnode->parent)
        {
            vfs_child_lock();
            current_parent->child = list_delete(current_parent->child, entry->vnode);
            new_parent->child = list_prepend(new_parent->child, entry->vnode);
            entry->vnode->parent = new_parent;
            entry->vnode->fsid = new_parent->fsid;
            entry->vnode->root = new_parent->root;
            vfs_child_unlock();
        }
        free(entry->vnode->name);
        entry->vnode->name = strdup(name);
    }
    unlock();
    if (moved) vfs_close(new_parent);
    return error ? VFS_STATUS_FAILED : EOK;
}

void ext4_resize(void *handle, uint64_t size)
{
    Node *entry = (Node *)handle;
    if (entry == NULL) return;
    lock();
    entry->mount->writer->truncate(entry->inode.number, size, seconds_now());
    xjext4::Inode refreshed;
    if (entry->mount->writer->view().read_inode(entry->inode.number, refreshed) == 0)
    {
        entry->inode = refreshed;
        if (entry->vnode != NULL) apply_inode(entry->mount, entry->vnode, refreshed);
    }
    unlock();
}

size_t ext4_readlink(vfs_node_t, void *, size_t, size_t) { return VFS_STATUS_FAILED; }
int ext4_ioctl(void *, size_t, void *) { return -EOPNOTSUPP; }
int ext4_poll(void *, size_t) { return -EOPNOTSUPP; }
vfs_node_t ext4_dup(vfs_node_t)
{
    // VFS dup is not required for the root filesystem; report unsupported.
    return NULL;
}

void *ext4_map(void *handle, void *addr, size_t offset, size_t size, size_t prot, size_t flags)
{
    // proplib/xbps internalizes pkgdb via mmap; provide a private snapshot map
    // backed by the ext4 read callback (same model as fatfs).
    return general_map((vfs_read_t)ext4_read, handle, (uint64_t)addr, size, prot, flags, offset);
}

int ext4_do_mount(const char *source, vfs_node_t node, const XJ_ROOT_BINDING *binding, bool writable);

int ext4_mount(const char *source, vfs_node_t node)
{
    // vfs_mount() probes every registered filesystem; pseudo-filesystems pass
    // their numeric register id as "source". Only absolute device paths apply.
    if (source == NULL || is_virtual_fs(source) || source[0] != '/') return VFS_STATUS_FAILED;
    return ext4_do_mount(source, node, NULL, true);
}

void destroy_mount(Mount *mount)
{
    if (mount == NULL) return;
    lock();
    if (mount->writer != NULL)
    {
        if (mount->writable) mount->writer->shutdown();
        free(mount->workspace);
        free(mount->writer);
        if (mount->storage.data != NULL) free(mount->storage.data);
    }
    if (mount->device != NULL) vfs_close(mount->device);
    Mount **link = &mounts;
    while (*link != NULL && *link != mount) link = &(*link)->next;
    if (*link == mount) *link = mount->next;
    unlock();
    free(mount);
}

// VFS passes the mounted root node handle, not the internal Mount pointer.
int ext4_unmount(void *context)
{
    Node *root = (Node *)context;
    if (root == NULL) return 0;
    Mount *mount = root->mount;
    free(root);
    destroy_mount(mount);
    return 0;
}

int ext4_do_mount(const char *source, vfs_node_t node, const XJ_ROOT_BINDING *binding, bool writable)
{
    if (source == NULL || node == NULL) return VFS_STATUS_FAILED;
    if (node->is_mount || (node != rootdir && node->child != NULL)) return -EBUSY;
    vfs_node_t device = vfs_open(source);
    if (device == NULL || !(device->type & file_block))
    {
        if (device != NULL) vfs_close(device);
        return VFS_STATUS_FAILED;
    }

    /* The devfs node size is disk-relative; the device handle carries the
       real partition extent. Use it for both root and data mounts. */
    device_handle_t dev_handle = (device_handle_t)device->handle;
    uint64_t device_bytes = (dev_handle != NULL && dev_handle->device != NULL) ?
                            dev_handle->device->size : device->size;
    if (device_bytes == 0)
    {
        vfs_close(device);
        return VFS_STATUS_FAILED;
    }

    Mount *mount = (Mount *)calloc(1, sizeof(Mount));
    if (mount == NULL) { vfs_close(device); return -ENOMEM; }
    mount->device = device;
    mount->bytes = device_bytes;
    mount->writable = writable;
    mount->fsid = filesystem_id;

    xjext4::WriteDevice io = {read_device, write_device, flush_device, mount, mount->bytes};
    mount->workspace = (xjext4::WriteWorkspace *)malloc(sizeof(xjext4::WriteWorkspace));
    mount->writer = (xjext4::WriteSession *)calloc(1, sizeof(xjext4::WriteSession));
    if (mount->workspace == NULL || mount->writer == NULL)
    {
        free(mount->workspace);
        free(mount->writer);
        vfs_close(device);
        free(mount);
        return -ENOMEM;
    }

    int error = xjext4::WriteSession::storage_size(io, mount->storage.bytes);
    if (!error)
    {
        mount->storage.data = malloc(mount->storage.bytes);
        if (mount->storage.data == NULL) error = -ENOMEM;
    }
    if (!error)
        error = mount->writer->open(io, *mount->workspace, binding ? validate_root : nullptr,
                                    (void *)binding, &mount->storage);
    if (error)
    {
        free(mount->workspace);
        free(mount->writer);
        free(mount->storage.data);
        vfs_close(device);
        free(mount);
        return error;
    }

    xjext4::Inode root_inode;
    error = mount->writer->view().read_inode(2, root_inode);
    if (error)
    {
            destroy_mount(mount);
        return error;
    }

    Node *root = make_node(mount, root_inode, 2);
    if (root == NULL)
    {
        destroy_mount(mount);
        return -ENOMEM;
    }
    root->vnode = node;
    /* Release the covered mountpoint handle (FAT/tmpfs) before publishing ext4. */
    if (node->handle != NULL && node->fsid != 0)
    {
        vfs_filesystem_t covered = get_filesystem_node(node);
        if (covered != NULL && covered->callback != NULL && covered->callback->close != NULL)
            covered->callback->close(node->handle);
    }
    node->handle = root;
    apply_inode(mount, node, root_inode);
    node->type = file_dir;
    node->size = 0;
    node->root = node;
    error = populate(mount, node, root_inode);
    if (error)
    {
        free(root);
        node->handle = NULL;
        destroy_mount(mount);
        return error;
    }

    lock();
    mount->next = mounts;
    mounts = mount;
    unlock();
    write_serial_fmt("EXT4: mounted %s (%llu MiB, %s)\n", source,
                     (unsigned long long)(mount->bytes / (1024 * 1024)),
                     writable ? "journaled writable" : "read-only");
    return VFS_STATUS_SUCCESS;
}

struct vfs_callback ext4_callbacks = {
    .mount = ext4_mount,
    .unmount = (vfs_unmount_t)ext4_unmount,
    .open = (vfs_open_t)ext4_open,
    .close = (vfs_close_t)ext4_close,
    .read = ext4_read,
    .write = ext4_write,
    .readlink = ext4_readlink,
    .mkdir = ext4_mkdir,
    .mkfile = ext4_mkfile,
    .link = (vfs_mk_t)ext4_mkfile,
    .symlink = (vfs_mk_t)ext4_mkfile,
    .stat = ext4_stat,
    .ioctl = ext4_ioctl,
    .dup = ext4_dup,
    .poll = ext4_poll,
    .map = ext4_map,
    .resize = ext4_resize,
    .del = (vfs_del_t)ext4_delete,
    .rename = (vfs_rename_t)ext4_rename,
};
} // namespace

void ext4fs_init()
{
    if (initialized) return;
    mutex_create(&operation_lock, true);
    initialized = true;
    filesystem_id = vfs_regist("ext4", &ext4_callbacks, 0, 0xef53);
}

int ext4fs_mount_bound_root(const char *device, const XJ_ROOT_BINDING *binding)
{
    return ext4_do_mount(device, rootdir, binding, true);
}

int ext4fs_finish_all()
{
    if (!initialized) return 0;
    lock();
    for (Mount *mount = mounts; mount; mount = mount->next)
        if (mount->writable && mount->writer != nullptr) mount->writer->sync();
    unlock();
    return 0;
}

void ext4fs_quiescent_exit() {}

bool ext4fs_owns_node(vfs_node_t node)
{
    if (node == NULL || !initialized) return false;
    for (Mount *mount = mounts; mount; mount = mount->next)
    {
        if (node->fsid == (uint16_t)mount->fsid) return true;
    }
    return false;
}

int ext4fs_stat(vfs_node_t node, struct stat *result)
{
    if (node == NULL || result == NULL) return -EINVAL;
    memset(result, 0, sizeof(*result));
    result->st_ino = node->inode;
    result->st_mode = node->permissions | (node->type & file_dir ? 0040000 : 0100000);
    result->st_size = node->size;
    result->st_blksize = node->blksz ? node->blksz : 4096;
    return 0;
}

int ext4fs_statfs(vfs_node_t node, struct statfs *result)
{
    if (result == NULL) return -EINVAL;
    memset(result, 0, sizeof(*result));
    /* fsid identifies the filesystem driver, not the mount, so every ext4
       mount shares it. Use the node's mount record to report the volume the
       path actually lives on (root and /users are separate ext4 mounts). */
    Mount *mount = NULL;
    if (node != NULL && node->handle != NULL) mount = ((Node *)node->handle)->mount;
    if (mount == NULL) mount = mounts;
    if (mount == NULL || mount->writer == NULL) return -ENODEV;

    const xjext4::Volume &volume = mount->writer->view();
    result->f_bsize = volume.block_size();
    result->f_blocks = volume.blocks_count();
    result->f_bfree = volume.free_blocks();
    result->f_bavail = volume.free_blocks();
    result->f_files = volume.inodes_count();
    result->f_ffree = volume.free_inodes();
    result->f_namelen = 255;
    return 0;
}

int ext4fs_sync(vfs_node_t node)
{
    (void)node;
    if (!initialized) return 0;
    lock();
    for (Mount *mount = mounts; mount; mount = mount->next)
        if (mount->writable && mount->writer) mount->writer->sync();
    unlock();
    return 0;
}

int ext4fs_sync_all() { return ext4fs_sync(NULL); }

int ext4fs_chmod(vfs_node_t node, uint16_t mode)
{
    if (node == NULL || node->handle == NULL) return -EINVAL;
    Node *entry = (Node *)node->handle;
    xjext4::AttributeUpdate update = {};
    update.mask = xjext4::kAttributeMode;
    update.mode = mode;
    lock();
    int error = entry->mount->writer->set_attributes(entry->inode.number, update, seconds_now());
    unlock();
    return error;
}

int ext4fs_chown(vfs_node_t node, uint32_t uid, uint32_t gid)
{
    if (node == NULL || node->handle == NULL) return -EINVAL;
    Node *entry = (Node *)node->handle;
    xjext4::AttributeUpdate update = {};
    update.mask = xjext4::kAttributeUid | xjext4::kAttributeGid;
    update.uid = uid;
    update.gid = gid;
    lock();
    int error = entry->mount->writer->set_attributes(entry->inode.number, update, seconds_now());
    unlock();
    return error;
}

int ext4fs_utimens(vfs_node_t node, const struct timespec *times)
{
    if (node == NULL || node->handle == NULL || times == NULL) return -EINVAL;
    Node *entry = (Node *)node->handle;
    xjext4::AttributeUpdate update = {};
    update.mask = xjext4::kAttributeAtime | xjext4::kAttributeMtime;
    update.atime_seconds = times[0].tv_sec;
    update.atime_nanoseconds = times[0].tv_nsec;
    update.mtime_seconds = times[1].tv_sec;
    update.mtime_nanoseconds = times[1].tv_nsec;
    lock();
    int error = entry->mount->writer->set_attributes(entry->inode.number, update, seconds_now());
    unlock();
    return error;
}
