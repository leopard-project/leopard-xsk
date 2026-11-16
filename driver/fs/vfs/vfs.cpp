/**
 * Virtual File System Interface
 * 虚拟文件系统接口
 * Create by min0911Y & zhouzhihao & copi143
 */
#define ALL_IMPLEMENTATION

#include "fs/vfs/vfs.h"
#include <abi/posix.h>
#include <dlinker.h>
#include "errno.h"
#include "krlibc.h"
#include "fs/vfs/list.h"
#include "llist.h"
#include "pipe.h"
#include "task/pcb.h"
#include <cpu/lock.h>
#include <syscall/syscall.h>
#include <task/poll.h>
#include <user/user.h>

vfs_node_t rootdir = NULL;

// 通用 VFS 策略钩子（权限层、审计或沙箱模块）。
// 空链 = 无策略，默认放行；任一钩子拒绝即拒绝。
static constexpr int VFS_ACCESS_HOOK_MAX = 4;
static vfs_access_hook_t         g_vfs_access_hooks[VFS_ACCESS_HOOK_MAX];
static int                       g_vfs_access_hook_count = 0;

// Installed by the terminal layer (pty.cpp); NULL keeps the static /dev/tty.
static vfs_ctty_resolver_t       g_vfs_ctty_resolver = NULL;

void vfs_set_ctty_resolver(vfs_ctty_resolver_t resolver)
{
    g_vfs_ctty_resolver = resolver;
}

void vfs_set_access_hook(vfs_access_hook_t fn)
{
    // 兼容旧的单槽 API：替换第 0 槽，不动权限层/其他模块。
    g_vfs_access_hooks[0] = fn;
    if (fn != NULL && g_vfs_access_hook_count < 1) g_vfs_access_hook_count = 1;
    if (fn == NULL && g_vfs_access_hook_count <= 1) g_vfs_access_hook_count = 0;
}

bool vfs_add_access_hook(vfs_access_hook_t fn)
{
    if (fn == NULL) return true;
    for (int i = 0; i < g_vfs_access_hook_count; ++i)
        if (g_vfs_access_hooks[i] == fn) return true;
    if (g_vfs_access_hook_count >= VFS_ACCESS_HOOK_MAX) return false;
    g_vfs_access_hooks[g_vfs_access_hook_count++] = fn;
    return true;
}

bool vfs_access_allowed(vfs_node_t node, const char *path, vfs_access_op_t op)
{
    for (int i = 0; i < g_vfs_access_hook_count; ++i)
    {
        vfs_access_hook_t hook = g_vfs_access_hooks[i];
        if (hook != NULL && !hook(node, path, op)) return false;
    }
    return true;
}

int vfs_last_error()
{
    tcb_t task = get_current_task();
    int error = task != NULL ? task->file_access_error : 0;
    return error != 0 ? error : EIO;
}

void vfs_clear_access_error()
{
    tcb_t task = get_current_task();
    if (task != NULL) task->file_access_error = 0;
}

void vfs_set_access_error(int error)
{
    tcb_t task = get_current_task();
    if (task != NULL) task->file_access_error = error;
}

bool vfs_check_access(vfs_node_t node, const char *path, int operation)
{
    vfs_clear_access_error();
    if (!vfs_access_allowed(node, path, (vfs_access_op_t)operation))
    {
        tcb_t task = get_current_task();
        if (task != NULL && task->file_access_error == 0) task->file_access_error = EACCES;
        return false;
    }
    return true;
}

bool vfs_check_child_creation(vfs_node_t parent, const char *name)
{
    if (parent == NULL || name == NULL || name[0] == '\0')
    {
        vfs_set_access_error(EINVAL);
        return false;
    }

    /* DAC: creating a directory entry requires search (x) and write (w) on
       the parent directory, never permissions on the not-yet-existing child. */
    if (!vfs_check_access(parent, NULL, VFS_ACCESS_WRITE) ||
        !vfs_check_access(parent, NULL, VFS_ACCESS_EXEC))
        return false;

    char *base = vfs_get_fullpath(parent);
    if (base == NULL)
    {
        vfs_set_access_error(ENOMEM);
        return false;
    }

    size_t capacity = strlen(base) + strlen(name) + 2;
    char *path = (char *)malloc(capacity);
    if (path == NULL)
    {
        free(base);
        vfs_set_access_error(ENOMEM);
        return false;
    }
    snprintf(path, capacity, "%s/%s", base, name);

    char *canonical = normalize_path(path);
    bool allowed = canonical != NULL && vfs_check_access(NULL, canonical, VFS_ACCESS_WRITE);
    if (canonical == NULL) vfs_set_access_error(ENOMEM);

    free(canonical);
    free(path);
    free(base);
    return allowed;
}

static void empty_func() {}

struct vfs_callback   vfs_empty_callback;
struct vfs_filesystem vfs_empty_filesystem;

vfs_callback_t fs_callbacks[256] = {
    [0] = &vfs_empty_callback,
};

struct llist_header fs_metadata_list;

static int fs_nextid = 1;
static spin_t vfs_child_list_lock = SPIN_INIT;

#define callbackof(node, _name_) (fs_callbacks[(node)->fsid]->_name_)

errno_t vfs_chmod(vfs_node_t node, uint32_t mode)
{
    if (node == NULL) return -EINVAL;
    vfs_callback_t callback = fs_callbacks[node->fsid];
    if (callback != NULL && callback->chmod != NULL)
    {
        int error = callback->chmod(node, (uint16_t)(mode & 07777));
        if (error < 0) return error;
    }
    vfs_node_set_mode(node, mode);
    return EOK;
}

errno_t vfs_chown(vfs_node_t node, uint32_t uid, uint32_t gid)
{
    if (node == NULL) return -EINVAL;
    uint32_t owner = uid == 0xFFFFFFFFu ? node->owner : uid;
    uint32_t group = gid == 0xFFFFFFFFu ? node->group : gid;
    vfs_callback_t callback = fs_callbacks[node->fsid];
    if (callback != NULL && callback->chown != NULL)
    {
        int error = callback->chown(node, owner, group);
        if (error < 0) return error;
    }
    node->owner = owner;
    node->group = group;
    return EOK;
}

void vfs_child_lock() {
    spin_lock(&vfs_child_list_lock);
}

void vfs_child_unlock() {
    spin_unlock(&vfs_child_list_lock);
}

static inline char *pathtok(char **sp) {
    char *s = *sp;
    char *e = *sp;

    // 跳过所有连续的斜杠
    while (*e == '/') {
        e++;
    }

    // 如果已经到达字符串末尾，返回 NULL
    if (*e == '\0') {
        *sp = e; // 更新指针到字符串末尾
        return NULL;
    }

    s = e; // 设置令牌起始位置（第一个非斜杠字符）

    // 查找下一个斜杠或字符串结尾
    while (*e != '\0' && *e != '/') {
        e++;
    }

    // 保存下一个令牌的起始位置
    char *next = e;
    if (*e == '/') {
        next++; // 跳过斜杠指向下一个字符
    }

    // 终止当前令牌
    if (*e != '\0') { *e = '\0'; }

    *sp = next; // 更新指针到下一个令牌位置
    return s;   // 返回当前令牌
}

static inline void do_open(vfs_node_t file) {
    if (file == NULL) return;
    bool deleting = (file->type & file_delete) != 0;
    if (file->handle != NULL) {
        callbackof(file, stat)(file->handle, file);
    } else {
        if (file->parent == NULL || file->parent->handle == NULL) return;
        callbackof(file, open)(file->parent->handle, file->name, file);
    }
    if (deleting) file->type |= file_delete;
}

static inline void do_update(vfs_node_t file) {
    if (file == NULL) return;
    if ((file->type & file_symlink) && file->linkname != NULL) return;
    if (file->type & file_none || file->handle == NULL || file->type & file_dir ||
        file->type & file_symlink || file->type & file_pipe)
        do_open(file);
}

vfs_filesystem_t get_filesystem(const char *type) {
    if (type == NULL) return NULL;
    vfs_filesystem_t filesystem = NULL;
    vfs_filesystem_t pos, n;
    llist_for_each(pos, n, &fs_metadata_list, node) {
        if (strcmp(pos->name, type) == 0) { filesystem = pos; }
    }
    return filesystem;
}

/* 注册 ID 0 保留给按设备探测的文件系统（ext4/fatfs）；虚拟文件系统
   使用各 *_REGISTER_ID 定义的非 0 ID；负数表示不可挂载的内部回调。 */
static vfs_filesystem_t get_filesystem_by_id(int register_id) {
    if (register_id <= 0) return NULL;
    vfs_filesystem_t filesystem = NULL;
    vfs_filesystem_t pos, n;
    llist_for_each(pos, n, &fs_metadata_list, node) {
        if (pos->id == register_id) { filesystem = pos; }
    }
    return filesystem;
}

vfs_filesystem_t get_filesystem_node(vfs_node_t node) {
    vfs_filesystem_t filesystem = NULL;
    vfs_filesystem_t pos, n;
    llist_for_each(pos, n, &fs_metadata_list, node) {
        if (pos->fsid == node->fsid) { filesystem = pos; }
    }
    return filesystem;
}

vfs_node_t vfs_child_append(vfs_node_t parent, const char *name, void *handle) {
    vfs_node_t node = vfs_node_alloc(parent, name);
    if (node == NULL) return NULL;
    node->handle = handle;
    return node;
}

static vfs_node_t vfs_child_find(vfs_node_t parent, const char *name) {
    if (parent == NULL || name == NULL) return NULL;
    vfs_child_lock();
    vfs_node_t child = (vfs_node_t)list_first(parent->child, data,
                                               !(((vfs_node_t)data)->type & file_delete) &&
                                               streq(name, ((vfs_node_t)data)->name));
    vfs_child_unlock();
    return child;
}

static vfs_node_t vfs_child_find_and_ref(vfs_node_t parent, const char *name)
{
    if (parent == NULL || name == NULL) return NULL;

    vfs_child_lock();
    vfs_node_t child = (vfs_node_t)list_first(parent->child, data,
                                               !(((vfs_node_t)data)->type & file_delete) &&
                                               streq(name, ((vfs_node_t)data)->name));
    if (child != NULL) child->refcount++;
    vfs_child_unlock();
    return child;
}

/* 新节点的默认 DAC 属性：owner = 当前身份，group = 父目录 setgid 或当前身份，
   mode = 0777/0666 & ~umask。文件系统 callback 负责把这些值写进持久 inode。 */
static void vfs_prepare_new_node(vfs_node_t node, bool is_dir)
{
    if (node == NULL) return;

    UserInfo *user = task_effective_user();
    uint32_t uid = user_uid(user);
    uint32_t gid = user_gid(user);
    uint32_t umask = 022;

    tcb_t task = get_current_task();
    if (task != NULL && task->parent_group != NULL) umask = task->parent_group->umask & 0777;
    if (node->parent != NULL && (vfs_node_mode(node->parent) & 02000)) gid = node->parent->group;

    node->owner = uid;
    node->group = gid;
    vfs_node_set_mode(node, (is_dir ? 0777u : 0666u) & ~umask);
}

errno_t vfs_mkdir(const char *name) {
    if (!vfs_check_access(NULL, name, VFS_ACCESS_WRITE)) return VFS_STATUS_FAILED;
    if (name[0] != '/') return VFS_STATUS_FAILED;
    char      *path     = strdup(name + 1);
    char      *save_ptr = path;
    vfs_node_t current  = rootdir;
    for (const char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
        const vfs_node_t father = current;
        if (streq(buf, ".")) continue;
        if (streq(buf, "..")) {
            if (current->parent && current->type & file_dir) {
                current = current->parent;
                goto upd;
            } else {
                goto err;
            }
        }
        current = vfs_child_find(current, buf);

    upd:
        if (current == NULL) {
            if (!vfs_check_child_creation(father, buf)) goto err;
            current       = vfs_node_alloc(father, buf);
            current->type = file_dir;
            vfs_prepare_new_node(current, true);
            callbackof(father, mkdir)(father->handle, buf, current);
            do_update(current);
        } else {
            do_update(current);
            if (!(current->type & file_dir)) goto err;
        }
    }

    free(path);
    return EOK;

err:
    free(path);
    return VFS_STATUS_FAILED;
}

errno_t vfs_link(const char *name, const char *target_name) {
    if (!vfs_check_access(NULL, name, VFS_ACCESS_WRITE) ||
        !vfs_check_access(NULL, target_name, VFS_ACCESS_WRITE)) return VFS_STATUS_FAILED;
    vfs_node_t current = rootdir;
    char      *path    = strdup(name + 1);

    char *save_ptr = path;
    char *filename = path + strlen(path);

    while (*--filename != '/' && filename != path) {}
    if (filename != path) {
        *filename++ = '\0';
    } else {
        goto create;
    }

    if (strlen(path) == 0) {
        free(path);
        return -ENOENT;
    }
    vfs_node_t node ;
    for (const char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
        if (streq(buf, ".")) continue;
        if (streq(buf, "..")) {
            if (!current->parent || !(current->type & file_dir)) goto err;
            current = current->parent;
            continue;
        }
        vfs_node_t new_current = vfs_child_find(current, buf);
        if (new_current == NULL) {
            new_current       = vfs_node_alloc(current, buf);
            new_current->type = file_dir;
            vfs_prepare_new_node(new_current, true);
            callbackof(current, mkdir)(current->handle, buf, new_current);
        }
        current = new_current;
        do_update(current);

        if (!(current->type & file_dir)) goto err;
    }

create:
    if (!vfs_check_child_creation(current, filename)) goto err;
    node = vfs_child_append(current, filename, NULL);
    node->type      = file_none;
    vfs_prepare_new_node(node, false);
    callbackof(current, link)(current->handle, target_name, node);
    node->linkto = vfs_open(target_name);

    free(path);

    return EOK;

err:
    free(path);
    return VFS_STATUS_FAILED;
}

errno_t vfs_symlink(const char *name, const char *target_name) {
    if (!vfs_check_access(NULL, name, VFS_ACCESS_WRITE) ||
        !vfs_check_access(NULL, target_name, VFS_ACCESS_WRITE)) return VFS_STATUS_FAILED;
    vfs_node_t current = rootdir;
    char      *path    = strdup(name + 1);

    char *save_ptr = path;
    char *filename = path + strlen(path);

    while (*--filename != '/' && filename != path) {}
    if (filename != path) {
        *filename++ = '\0';
    } else {
        goto create;
    }
    vfs_node_t node;

    if (strlen(path) == 0) {
        free(path);
        return -1;
    }
    for (const char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
        if (streq(buf, ".")) continue;
        if (streq(buf, "..")) {
            if (!current->parent || !(current->type & file_dir)) goto err;
            current = current->parent;
            continue;
        }
        vfs_node_t new_current = vfs_child_find(current, buf);
        if (new_current == NULL) {
            new_current       = vfs_node_alloc(current, buf);
            new_current->type = file_dir;
            vfs_prepare_new_node(new_current, true);
            callbackof(current, mkdir)(current->handle, buf, new_current);
        }
        current = new_current;
        do_update(current);

        if (!(current->type & file_dir)) goto err;
    }

create:
    if (!vfs_check_child_creation(current, filename)) goto err;
    node = vfs_child_append(current, filename, NULL);
    node->type      = file_symlink;
    vfs_prepare_new_node(node, false);
    vfs_node_set_mode(node, 0777);
    node->linkname  = strdup(target_name);
    node->size      = node->linkname != NULL ? strlen(node->linkname) : 0;
    callbackof(current, symlink)(current->handle, target_name, node);
    node->linkto = vfs_open(target_name);

    free(path);

    return EOK;

err:
    free(path);
    return -EIO;
}

errno_t vfs_mkfile(const char *name) {
    if (!vfs_check_access(NULL, name, VFS_ACCESS_WRITE)) return VFS_STATUS_FAILED;
    if (name[0] != '/') return VFS_STATUS_FAILED;

    // 分离路径和文件名
    char *fullpath  = strdup(name);
    char *filename  = fullpath;
    char *lastslash = strrchr(fullpath, '/');

    if (lastslash == fullpath) {
        // 根目录下的文件
        filename   = fullpath + 1;
        *lastslash = '\0';
    } else if (lastslash) {
        *lastslash = '\0';
        filename   = lastslash + 1;
    }

    // 打开父目录
    vfs_node_t parent;
    if (lastslash == fullpath) {
        parent = rootdir;
    } else {
        parent = vfs_open(fullpath);
    }

    if (parent == NULL || parent->type != file_dir) { return VFS_STATUS_FAILED; }
    if (!vfs_check_child_creation(parent, filename)) { free(fullpath); return VFS_STATUS_FAILED; }

    // 创建文件
    vfs_node_t node = vfs_child_append(parent, filename, NULL);
    node->type      = file_none;
    vfs_prepare_new_node(node, false);
    errno_t status  = callbackof(parent, mkfile)(parent->handle, filename, node);
    free(fullpath);
    return status;
}

/* sticky 目录：只有 Root、目录 owner 或文件 owner 能删除/改名。 */
static bool vfs_sticky_allows(vfs_node_t parent, vfs_node_t victim)
{
    if (parent == NULL || victim == NULL) return false;
    if ((vfs_node_mode(parent) & 01000) == 0) return true;
    uint32_t uid = user_uid(task_effective_user());
    return uid == 0 || uid == parent->owner || uid == victim->owner;
}

errno_t vfs_delete(vfs_node_t node) {
    if (node == NULL || node == rootdir || node->parent == NULL) return VFS_STATUS_FAILED;
    vfs_node_t parent = node->parent;

    /* Unix 语义：删除取决于父目录的 w|x，而不是文件自身 mode。 */
    if (!vfs_check_access(parent, NULL, VFS_ACCESS_WRITE) ||
        !vfs_check_access(parent, NULL, VFS_ACCESS_EXEC) ||
        !vfs_sticky_allows(parent, node))
    {
        vfs_set_access_error(EACCES);
        return VFS_STATUS_FAILED;
    }

    vfs_child_lock();
    if (parent == NULL || (node->type & file_delete) ||
        ((node->type & file_dir) && node->child != NULL))
    {
        vfs_child_unlock();
        return VFS_STATUS_FAILED;
    }
    node->type |= file_delete;
    vfs_child_unlock();

    if (node->handle == NULL) do_update(node);

    errno_t res = VFS_STATUS_SUCCESS;
    if (!((node->type & file_symlink) && node->linkname != NULL))
    {
        if (node->handle == NULL)
        {
            res = VFS_STATUS_FAILED;
        }
        else
        {
            res = callbackof(node, del)(parent->handle, node);
        }
    }

    if (res < 0)
    {
        vfs_child_lock();
        node->type &= (uint16_t)~file_delete;
        vfs_child_unlock();
        vfs_close(node);
        return res;
    }

    vfs_child_lock();
    parent->child = list_delete(parent->child, node);
    node->parent = NULL;
    vfs_child_unlock();

    // Drop the directory-tree reference and the reference held by this
    // delete caller. Any other open references keep the detached node alive.
    if (node->refcount > 0) node->refcount--;
    if (node->refcount > 0) node->refcount--;
    if (node->refcount == 0)
    {
        if (node->handle != NULL && !(node->type & file_proxy))
        {
            callbackof(node, close)(node->handle);
            node->handle = NULL;
        }
        free(node->name);
        free(node->linkname);
        free(node);
    }
    return VFS_STATUS_SUCCESS;
}

errno_t vfs_rename(vfs_node_t node, const char *nw) {
    if (node == NULL || nw == NULL || node->parent == NULL) return VFS_STATUS_FAILED;
    if (!vfs_check_access(node->parent, NULL, VFS_ACCESS_WRITE) ||
        !vfs_check_access(node->parent, NULL, VFS_ACCESS_EXEC) ||
        !vfs_sticky_allows(node->parent, node) ||
        !vfs_check_access(NULL, nw, VFS_ACCESS_WRITE))
    {
        vfs_set_access_error(EACCES);
        return VFS_STATUS_FAILED;
    }

    // 把目标路径拆成"父目录 + 末级名字"，让新增子项的权限检查落在真正的目标父目录上。
    const char *leaf = strrchr(nw, '/');
    leaf             = leaf != NULL ? leaf + 1 : nw;
    if (*leaf == '\0')
    {
        vfs_set_access_error(EINVAL);
        return VFS_STATUS_FAILED;
    }

    char *dest_parent_path = strdup(nw);
    if (dest_parent_path == NULL)
    {
        vfs_set_access_error(ENOMEM);
        return VFS_STATUS_FAILED;
    }
    char *dest_slash = strrchr(dest_parent_path, '/');
    if (dest_slash == dest_parent_path)
    {
        dest_parent_path[1] = '\0'; // "/name" -> "/"
    }
    else if (dest_slash != NULL)
    {
        *dest_slash = '\0';
    }
    else
    {
        strcpy(dest_parent_path, "."); // "name" -> 当前父目录
    }

    char *leaf_copy = strdup(leaf);
    if (leaf_copy == NULL)
    {
        free(dest_parent_path);
        vfs_set_access_error(ENOMEM);
        return VFS_STATUS_FAILED;
    }

    // 没有父目录部分的相对名字沿用旧语义：相对当前节点所在的父目录。
    bool       use_node_parent   = streq(dest_parent_path, ".");
    vfs_node_t dest_parent       = use_node_parent ? node->parent : vfs_open(dest_parent_path);
    bool       dest_parent_owned = !use_node_parent;
    free(dest_parent_path);

    if (dest_parent == NULL || !(dest_parent->type & file_dir) ||
        !vfs_check_child_creation(dest_parent, leaf_copy))
    {
        if (dest_parent_owned) vfs_close(dest_parent);
        free(leaf_copy);
        return VFS_STATUS_FAILED;
    }

    if ((node->type & file_symlink) && node->linkname != NULL)
    {
        bool moved = dest_parent != node->parent;
        if (moved)
        {
            vfs_child_lock();
            node->parent->child = list_delete(node->parent->child, node);
            dest_parent->child = list_prepend(dest_parent->child, node);
            node->parent = dest_parent;
            node->fsid = dest_parent->fsid;
            node->root = dest_parent->root;
            node->dev = dest_parent->dev;
            vfs_child_unlock();
        }

        free(node->name);
        node->name = leaf_copy;
        if (dest_parent_owned) vfs_close(dest_parent);
        return VFS_STATUS_SUCCESS;
    }

    free(leaf_copy);
    do_update(node);
    if (node->handle == NULL)
    {
        if (dest_parent_owned) vfs_close(dest_parent);
        return VFS_STATUS_FAILED;
    }
    errno_t result = callbackof(node, rename)(node->handle, nw);
    if (dest_parent_owned) vfs_close(dest_parent);
    return result;
}

errno_t vfs_resize(vfs_node_t node, uint64_t size) {
    if (!vfs_check_access(node, NULL, VFS_ACCESS_WRITE)) return VFS_STATUS_FAILED;
    if (node == NULL) return VFS_STATUS_FAILED;
    do_update(node);
    if (node->type == file_dir) return VFS_STATUS_FAILED;
    callbackof(node, resize)(node->handle, size);
    do_update(node);
    return VFS_STATUS_SUCCESS;
}

int vfs_regist(const char *name, vfs_callback_t callback, int register_id, uint64_t magic) {
    if (callback == NULL) return VFS_STATUS_FAILED;
    /* chmod/chown are optional persistence hooks; every other slot is
       mandatory so dispatch never hits a NULL function pointer. */
    size_t required = offsetof(struct vfs_callback, chmod) / sizeof(void *);
    for (size_t i = 0; i < required; i++) {
        if (((void **)callback)[i] == NULL) return VFS_STATUS_FAILED;
    }
    int id           = fs_nextid++;
    fs_callbacks[id] = callback;

    vfs_filesystem_t filesystem = (vfs_filesystem_t)malloc(sizeof(struct vfs_filesystem));
    filesystem->callback        = callback;
    filesystem->id              = register_id;
    filesystem->fsid            = id;
    filesystem->magic           = magic;
    strcpy(filesystem->name, name);
    llist_init_head(&filesystem->node);
    llist_append(&fs_metadata_list, &filesystem->node);
    return id;
}

vfs_node_t vfs_do_search(vfs_node_t dir, const char *name) {
    if (dir == NULL || name == NULL) return NULL;
    vfs_child_lock();
    vfs_node_t child = (vfs_node_t)list_first(dir->child, data,
                                               !(((vfs_node_t)data)->type & file_delete) &&
                                               streq(name, ((vfs_node_t)data)->name));
    vfs_child_unlock();
    return child;
}

static constexpr unsigned VFS_MAX_LINK_DEPTH = 40;

static vfs_node_t vfs_open_impl(const char *str, bool follow_final_symlink, unsigned link_depth) {
    if (unlikely(str == NULL)) return NULL;
    if (unlikely(str[0] != '/')) return NULL;
    if (unlikely(link_depth > VFS_MAX_LINK_DEPTH)) return NULL;
    if (str[1] == '\0') return rootdir; // 根目录

    // "/dev/tty" follows the caller's controlling PTY when one is attached.
    // The resolver returns a referenced node or NULL for the static devfs tty.
    if (g_vfs_ctty_resolver != NULL && streq(str, "/dev/tty"))
    {
        vfs_node_t ctty = g_vfs_ctty_resolver();
        if (ctty != NULL) return ctty;
    }

    char *path = strdup(str + 1);
    if (unlikely(path == NULL)) return NULL;

    char      *save_ptr = path;
    vfs_node_t current  = rootdir;
    vfs_node_t acquired_current = NULL;

    for (char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
        if (streq(buf, ".")) {
            // 当前目录，不需要操作
            continue;
        } else if (streq(buf, "..")) {
            // 父目录
            if (current->parent) { current = current->parent; }
            continue;
        }

        bool is_final_component = save_ptr == NULL || *save_ptr == '\0';
        current = is_final_component ? vfs_child_find_and_ref(current, buf) : vfs_child_find(current, buf);
        if (current == NULL) { goto err; }
        if (is_final_component) acquired_current = current;
        do_update(current);
        if (is_final_component && !follow_final_symlink) break;
        if (current->type & file_symlink) {
            if (!current->parent) { goto err; }

            bool dynamic_symlink = current->name != NULL && streq(current->name, "self") &&
                                   current->parent != NULL && current->parent->parent == NULL &&
                                   current->handle != NULL && current->linkto == NULL;
            if (!dynamic_symlink) current->type = file_symlink | file_proxy;

            vfs_node_t target = dynamic_symlink ? NULL : current->linkto;
            bool target_was_cached = target != NULL;
            if (target == NULL)
            {
                char target_path[256];
                size_t len = vfs_readlink(current, target_path, sizeof(target_path) - 1);
                if (len == (size_t)VFS_STATUS_FAILED || len == 0 || len >= sizeof(target_path)) goto err;
                target_path[len] = '\0';
                if (target_path[0] == '/')
                {
                    target = vfs_open_impl(target_path, true, link_depth + 1);
                }
                else
                {
                    char *parent_path = vfs_get_fullpath(current->parent);
                    if (parent_path == NULL) goto err;
                    size_t parent_len = strlen(parent_path);
                    size_t target_len = strlen(target_path);
                    char *joined_target = (char *)malloc(parent_len + target_len + 2);
                    if (joined_target == NULL)
                    {
                        free(parent_path);
                        goto err;
                    }
                    sprintf(joined_target, "%s%s%s", parent_path,
                            parent_path[parent_len - 1] == '/' ? "" : "/", target_path);
                    char *full_target = normalize_path(joined_target);
                    free(joined_target);
                    free(parent_path);
                    if (full_target == NULL) goto err;
                    target = vfs_open_impl(full_target, true, link_depth + 1);
                    free(full_target);
                }
                if (!dynamic_symlink) current->linkto = target;
            }
            if (!target) goto err;

            if (is_final_component && target_was_cached)
            {
                bool target_reference_valid = true;
                vfs_child_lock();
                if (target->type & file_delete)
                    target_reference_valid = false;
                else
                    target->refcount++;
                vfs_child_unlock();
                if (!target_reference_valid)
                {
                    vfs_close(current);
                    acquired_current = NULL;
                    goto err;
                }
            }
            if (is_final_component)
            {
                vfs_close(current);
                acquired_current = NULL;
            }

            current = target;
            continue;
        }
    }

    free(path);
    return current;
err:
    free(path);
    if (acquired_current != NULL) vfs_close(acquired_current);
    return NULL;
}

vfs_node_t vfs_open(const char *str) {
    vfs_clear_access_error();
    vfs_node_t node = vfs_open_impl(str, true, 0);
    if (node && !vfs_check_access(node, NULL, VFS_ACCESS_READ)) { vfs_close(node); return NULL; }
    return node;
}

vfs_node_t vfs_open_no_follow(const char *str) {
    vfs_clear_access_error();
    vfs_node_t node = vfs_open_impl(str, false, 0);
    if (node && !vfs_check_access(node, NULL, VFS_ACCESS_READ)) { vfs_close(node); return NULL; }
    return node;
}

void vfs_update(vfs_node_t node) {
    do_update(node);
}

fd_file_handle *fd_dup(fd_file_handle *src) {
    fd_file_handle *nw = (fd_file_handle *)malloc(sizeof(fd_file_handle));
    // not_null_assets(new, "fd_dup out of memory.");
    nw->node       = src->node;
    nw->offset     = src->offset;
    nw->flags      = src->flags;
    nw->fd         = src->fd;
    vfs_node_t node = nw->node;
    if ((node->type & (file_ptmx | file_pts)) && fs_callbacks[node->fsid]->dup != (void *)empty_func) {
        fs_callbacks[node->fsid]->dup(node);
        node->refcount++;
        return nw;
    }
    if ((node->type & file_pipe) && node->handle != NULL) {
        pipe_specific_t *spec = (pipe_specific_t *)node->handle;
        pipe_info_t     *pipe = spec != NULL ? spec->info : NULL;
        if (pipe != NULL) {
            spin_lock(&pipe->lock);
            if (spec->write) {
                pipe->write_fds++;
            } else {
                pipe->read_fds++;
            }
            spin_unlock(&pipe->lock);
        }
    }
    node->refcount++;
    return nw;
}

vfs_node_t get_rootdir() {
    return rootdir;
}

void set_rootdir(vfs_node_t node) {
    rootdir         = node;
    rootdir->parent = NULL;
}

void *vfs_map(vfs_node_t node, uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags,
              uint64_t offset) {
    if (unlikely(node == NULL)) return NULL;
    if (unlikely(node->type == file_dir)) return NULL;
    vfs_access_op_t access = (prot & 2) ? ((prot & 1) ? VFS_ACCESS_READ_WRITE : VFS_ACCESS_WRITE) :
                                          VFS_ACCESS_READ;
    if (!vfs_check_access(node, NULL, access)) return NULL;
    return callbackof(node, map)(node->handle, (void *)addr, offset, len, prot, flags);
}

vfs_node_t vfs_node_alloc(vfs_node_t parent, const char *name) {
    vfs_node_t node = (vfs_node_t)malloc(sizeof(struct vfs_node));
    // not_null_assets(node, "vfs alloc null");
    if (unlikely(node == NULL)) return NULL;
    memset(node, 0, sizeof(struct vfs_node));
    node->parent   = parent;
    node->name     = name ? strdup(name) : NULL;
    node->type     = file_none;
    node->fsid     = parent ? parent->fsid : 0;
    node->root     = parent ? parent->root : node;
    node->dev      = parent ? parent->dev : 0;
    node->refcount = 1;
    node->blksz    = PAGE_SIZE;
    vfs_node_set_mode(node, 0777);
    node->linkto   = NULL;
    if (parent)
    {
        vfs_child_lock();
        parent->child = list_prepend(parent->child, node);
        vfs_child_unlock();
    }
    return node;
}

errno_t vfs_close(vfs_node_t node) {
    if (unlikely(node == NULL)) return VFS_STATUS_FAILED;
    if (node == rootdir) return VFS_STATUS_SUCCESS;

    if (node->refcount > 0) node->refcount--;

    if (node->type & (file_ptmx | file_pts)) {
        /* fd_dup() bumps both refcount and the PTY handle count
         * (ptmx_dup/pts_dup).  The fs close callback decrements that count,
         * so it must run exactly once, when the last reference goes away.
         * Calling it on every close drives slave_fds/master_fds to zero while
         * the orchestrator still owns fd 0/1/2, which makes conhost's writes
         * to the PTY master fail with EIO (keyboard input silently stops). */
        if (node->refcount > 0) return VFS_STATUS_SUCCESS;
        if (node->handle != NULL) callbackof(node, close)(node->handle);
        node->handle = NULL;
        if (node->parent == NULL) {
            free(node->name);
            free(node);
        }
        return VFS_STATUS_SUCCESS;
    }

    if (node->type & file_pipe) {
        if (node->handle != NULL) {
            callbackof(node, close)(node->handle);
        }

        if (node->refcount <= 0) {
            if (node->parent != NULL) {
                vfs_child_lock();
                node->parent->child = list_delete(node->parent->child, node);
                vfs_child_unlock();
            }
            node->handle = NULL;
            free(node->name);
            free(node);
        }
        return VFS_STATUS_SUCCESS;
    }

    if (node->type & file_epoll) {
        if (node->refcount > 0) return VFS_STATUS_SUCCESS;
        epoll_file_t *ep = (epoll_file_t *)node->handle;
        if (ep != NULL) {
            if (ep->watches != NULL) {
                while (true) {
                    epoll_watch_t *watch = (epoll_watch_t *)queue_dequeue(ep->watches);
                    if (watch == NULL) break;
                    free(watch);
                }
                queue_destroy(ep->watches);
            }
            free(ep);
            node->handle = NULL;
        }
        if (node->parent == NULL) {
            free(node->name);
            free(node);
        }
        return VFS_STATUS_SUCCESS;
    }

    if (unlikely(node->handle == NULL))
    {
        if ((node->type & file_delete) && node->refcount == 0)
        {
            free(node->name);
            free(node->linkname);
            free(node);
        }
        return VFS_STATUS_SUCCESS;
    }

    if (node->type & file_socket)
    {
        if (node->refcount > 0) return VFS_STATUS_SUCCESS;
        if (node->type & file_delete)
        {
            callbackof(node, del)(node->parent != NULL ? node->parent->handle : NULL, node);
            node->handle = NULL;
            if (node->parent != NULL)
            {
                vfs_child_lock();
                node->parent->child = list_delete(node->parent->child, node);
                node->parent = NULL;
                vfs_child_unlock();
            }
            free(node->name);
            free(node->linkname);
            free(node);
        }
        return VFS_STATUS_SUCCESS;
    }

    if (node->type & file_proxy){
         if ((node->type & file_delete) && node->refcount == 0)
         {
             free(node->name);
             free(node->linkname);
             free(node);
         }
         return VFS_STATUS_SUCCESS;
    }
    if ((node->type & file_dir) && !(node->type & file_delete))
    {
        return VFS_STATUS_SUCCESS;
    } 
    if (!(node->type & file_delete) && node->refcount > 0) {
        return VFS_STATUS_SUCCESS;
    }
    if (node->type & file_delete)
    {
        if (node->refcount > 0) return VFS_STATUS_SUCCESS;
        callbackof(node, close)(node->handle);
        node->handle = NULL;
        if (node->parent != NULL)
        {
            vfs_child_lock();
            node->parent->child = list_delete(node->parent->child, node);
            node->parent = NULL;
            vfs_child_unlock();
        }
        free(node->name);
        free(node->linkname);
        free(node);
    }
    else
    {
        callbackof(node, close)(node->handle);
        node->handle = NULL;
    }
    return VFS_STATUS_SUCCESS;
}

void vfs_free(vfs_node_t vfs) {
    if (vfs == NULL) return;
    vfs_child_lock();
    list_t child = vfs->child;
    vfs->child   = NULL;
    vfs_child_unlock();
    list_free_with(child, (void (*)(void *))vfs_free);
    vfs_close(vfs);
    free(vfs->name);
    free(vfs->linkname);
    free(vfs);
}

void vfs_free_child(vfs_node_t vfs) {
    if (vfs == NULL) return;
    vfs_child_lock();
    list_t child = vfs->child;
    vfs->child   = NULL;
    vfs_child_unlock();
    list_free_with(child, (void (*)(void *))vfs_free);
}

static errno_t vfs_mount_filesystem(vfs_filesystem_t fs, const char *src, vfs_node_t node) {
    if (fs == NULL || fs->callback == NULL || fs->callback->mount == NULL)
        return VFS_STATUS_FAILED;
    if (node == NULL || node->type != file_dir) return VFS_STATUS_FAILED;
    if (!vfs_check_access(node, NULL, VFS_ACCESS_MOUNT)) return VFS_STATUS_FAILED;
    if (fs->callback->mount(src, node) != 0) return VFS_STATUS_FAILED;
    node->fsid     = fs->fsid;
    node->root     = node;
    node->is_mount = true;
    return VFS_STATUS_SUCCESS;
}

errno_t vfs_mount_as(vfs_filesystem_t fs, const char *src, vfs_node_t node) {
    return vfs_mount_filesystem(fs, src, node);
}

errno_t vfs_mount_device(const char *device, const char *type, vfs_node_t node) {
    if (node == NULL || node->type != file_dir) return VFS_STATUS_FAILED;

    if (type != NULL) return vfs_mount_filesystem(get_filesystem(type), device, node);

    if (!vfs_check_access(node, NULL, VFS_ACCESS_MOUNT)) return VFS_STATUS_FAILED;

    /* 自动探测只尝试设备类文件系统（注册 ID 0）。虚拟文件系统必须通过
       vfs_mount_virtual() 按 ID 显式挂载，不能被探测命中。 */
    vfs_filesystem_t pos, n;
    llist_for_each(pos, n, &fs_metadata_list, node) {
        if (pos->id != VFS_DEVICE_REGISTER_ID || pos->callback == NULL ||
            pos->callback->mount == NULL)
            continue;
        if (pos->callback->mount(device, node) == 0) {
            node->fsid     = pos->fsid;
            node->root     = node;
            node->is_mount = true;
            return VFS_STATUS_SUCCESS;
        }
    }
    return VFS_STATUS_FAILED;
}

errno_t vfs_mount_virtual(int register_id, vfs_node_t node) {
    return vfs_mount_filesystem(get_filesystem_by_id(register_id), NULL, node);
}

size_t vfs_read(vfs_node_t file, void *addr, size_t offset, size_t size) {
    if (!vfs_check_access(file, NULL, VFS_ACCESS_READ)) return VFS_STATUS_FAILED;
    if (file == NULL || addr == NULL) return VFS_STATUS_FAILED;
    do_update(file);
    if (file->type == file_dir) return VFS_STATUS_FAILED;
    return callbackof(file, read)(file->handle, addr, offset, size);
}

size_t vfs_write(vfs_node_t file, void *addr, size_t offset, size_t size) {
    if (!vfs_check_access(file, NULL, VFS_ACCESS_WRITE)) return VFS_STATUS_FAILED;
    if (file == NULL || addr == NULL) return VFS_STATUS_FAILED;
    do_update(file);
    if (file->type == file_dir) return VFS_STATUS_FAILED;
    size_t ret = callbackof(file, write)(file->handle, addr, offset, size);
    do_update(file);
    return ret;
}

size_t vfs_read_raw(vfs_node_t file, void *addr, size_t offset, size_t size) {
    if (file == NULL || addr == NULL) return VFS_STATUS_FAILED;
    do_update(file);
    if (file->type == file_dir) return VFS_STATUS_FAILED;
    return callbackof(file, read)(file->handle, addr, offset, size);
}

size_t vfs_write_raw(vfs_node_t file, const void *addr, size_t offset, size_t size) {
    if (file == NULL || addr == NULL) return VFS_STATUS_FAILED;
    do_update(file);
    if (file->type == file_dir) return VFS_STATUS_FAILED;
    size_t ret = callbackof(file, write)(file->handle, addr, offset, size);
    do_update(file);
    return ret;
}

errno_t vfs_ioctl(vfs_node_t device, size_t options, void *arg) {
    if (!vfs_check_access(device, NULL, VFS_ACCESS_WRITE)) return VFS_STATUS_FAILED;
    if (device == NULL) return VFS_STATUS_FAILED;
    do_update(device);
    if (device->type == file_dir) return VFS_STATUS_FAILED;
    return callbackof(device, ioctl)(device->handle, options, arg);
}

errno_t vfs_ioctl_raw(vfs_node_t device, size_t options, void *arg) {
    if (device == NULL) return -EINVAL;
    do_update(device);
    if (device->type == file_dir) return -EINVAL;
    return callbackof(device, ioctl)(device->handle, options, arg);
}

errno_t vfs_poll(vfs_node_t node, size_t event) {
    do_update(node);
    if (node->type & file_dir) return -1;
    return callbackof(node, poll)(node->handle, event);
}

errno_t vfs_unmount(const char *path) {
    if (!vfs_check_access(NULL, NULL, VFS_ACCESS_MOUNT)) return VFS_STATUS_FAILED;
    vfs_node_t node = vfs_open(path);
    if (node == NULL) return VFS_STATUS_FAILED;
    if (node->type != file_dir) return VFS_STATUS_FAILED;
    if (node->fsid == 0) return VFS_STATUS_FAILED;
    if (node->parent) {
        vfs_node_t cur = node;
        node           = node->parent;
        if (cur->root == cur) {
            vfs_free_child(cur);
            callbackof(cur, unmount)(cur->handle);
            cur->fsid     = node->fsid; // 交给上级
            cur->root     = node->root;
            cur->handle   = NULL;
            cur->child    = NULL;
            cur->is_mount = false;
            if (cur->fsid) do_update(cur);
            return VFS_STATUS_SUCCESS;
        }
    }
    return VFS_STATUS_FAILED;
}

size_t vfs_readlink(vfs_node_t node, char *buf, size_t bufsize) {
    if (!vfs_check_access(node, NULL, VFS_ACCESS_READ)) return VFS_STATUS_FAILED;
    if (node != NULL && node->linkname != NULL) {
        size_t link_size = strlen(node->linkname);
        size_t copy_size = MIN(link_size, bufsize);
        memcpy(buf, node->linkname, copy_size);
        return copy_size;
    }
    size_t ret = callbackof(node, readlink)(node, buf, 0, bufsize);
    return ret;
}

void *general_map(vfs_read_t read_callback, void *file, uint64_t addr, uint64_t len, uint64_t prot,
                  uint64_t flags, uint64_t offset) {
    UNUSED(flags);
    pcb_t current_task        = get_current_task()->parent_group;
    // current_task->mmap_start += (len + PAGE_SIZE - 1) & (~(PAGE_SIZE - 1));

    uint64_t pt_flags = PTE_USER | PTE_WRITEABLE | PTE_PRESENT;

    if (prot & PROT_READ) pt_flags |= PTE_PRESENT;
    if (prot & PROT_WRITE) pt_flags |= PTE_WRITEABLE;
    if (!(prot & PROT_EXEC)) pt_flags |= PTE_NO_EXECUTE;

    page_map_range_to_random(get_current_directory(), addr & (~(PAGE_SIZE - 1)),
                             (len + PAGE_SIZE - 1) & (~(PAGE_SIZE - 1)), pt_flags);

    ssize_t ret = read_callback(file, (void *)addr, offset, len);
    if (ret < 0) return (void *)-ENOMEM;

    return (void *)addr;
}

spin_t get_path_lock = SPIN_INIT;

// The caller owns the returned absolute path.
char *vfs_get_fullpath(vfs_node_t node)
{
    if (node == NULL) return NULL;

    spin_lock(&get_path_lock);

    size_t capacity = 32;
    size_t count    = 0;
    vfs_node_t *nodes = (vfs_node_t *)malloc(sizeof(vfs_node_t) * capacity);
    if (nodes == NULL)
    {
        spin_unlock(&get_path_lock);
        return NULL;
    }

    for (vfs_node_t current = node; current != NULL; current = current->parent)
    {
        if (count == capacity)
        {
            if (capacity > (size_t)-1 / 2 / sizeof(vfs_node_t))
            {
                free(nodes);
                spin_unlock(&get_path_lock);
                return NULL;
            }

            capacity *= 2;
            vfs_node_t *expanded = (vfs_node_t *)realloc(nodes, sizeof(vfs_node_t) * capacity);
            if (expanded == NULL)
            {
                free(nodes);
                spin_unlock(&get_path_lock);
                return NULL;
            }
            nodes = expanded;
        }
        nodes[count++] = current;
    }

    size_t path_length = 1;
    for (size_t index = count; index > 0; index--)
    {
        vfs_node_t current = nodes[index - 1];
        if (current == rootdir) continue;
        if (current->name == NULL)
        {
            free(nodes);
            spin_unlock(&get_path_lock);
            return NULL;
        }

        size_t name_length = strlen(current->name);
        size_t separator   = index > 1 ? 1 : 0;
        if (name_length > (size_t)-1 - path_length - separator - 1)
        {
            free(nodes);
            spin_unlock(&get_path_lock);
            return NULL;
        }
        path_length += name_length + separator;
    }

    char *path = (char *)malloc(path_length + 1);
    if (path == NULL)
    {
        free(nodes);
        spin_unlock(&get_path_lock);
        return NULL;
    }

    char *cursor = path;
    *cursor++ = '/';
    for (size_t index = count; index > 0; index--)
    {
        vfs_node_t current = nodes[index - 1];
        if (current == rootdir) continue;

        size_t name_length = strlen(current->name);
        memcpy(cursor, current->name, name_length);
        cursor += name_length;
        if (index > 1) *cursor++ = '/';
    }
    *cursor = '\0';

    free(nodes);
    spin_unlock(&get_path_lock);
    return path;
}

char *at_resolve_pathname(int dirfd, char *pathname) {
    if (pathname[0] == '/') { // by absolute pathname
        return normalize_path(pathname);
    } else if (pathname[0] != '/') {
        if (dirfd == AT_FDCWD) { // relative to cwd
            return vfs_cwd_path_build(pathname);
        } else { // relative to dirfd, resolve accordingly
            fd_file_handle *handle = (fd_file_handle *)queue_get(get_current_task()->parent_group->file_open, dirfd);
            if (!handle || !handle->node) return NULL;
            vfs_node_t node = handle->node;
            if (node->type != file_dir) return NULL;

            char *dirname = vfs_get_fullpath(node);
            if (dirname == NULL) return NULL;
            char *joined = pathacat(dirname, pathname);
            free(dirname);
            if (joined == NULL) return NULL;
            char *normalized = normalize_path(joined);
            free(joined);
            return normalized;
        }
    }

    return NULL;
}
char *normalize_path(const char *path)
{
    if (!path) return NULL;

    size_t len    = strlen(path);
    char  *result = (char *)malloc(len + 1);
    if (!result) return NULL;

    char *dup = strdup(path);
    if (!dup)
    {
        free(result);
        return NULL;
    }

    strcpy(result, "/");
    if (strcmp(path, "/") == 0)
    {
        free(dup);
        return result;
    }

    char *start = dup;
    if (*start == '/') start++;

    char *token = strtok(start, "/");
    while (token)
    {
        if (strcmp(token, ".") == 0) {}
        else if (strcmp(token, "..") == 0)
        {
            char *last_slash = strrchr(result, '/');
            if (last_slash != result)
                *last_slash = '\0';
            else
                result[1] = '\0';
        }
        else
        {
            if (result[strlen(result) - 1] != '/') strcat(result, "/");
            strcat(result, token);
        }

        token = strtok(NULL, "/");
    }

    free(dup);
    return result;
}
char *vfs_cwd_path_build(char *src) {
    char *s = src;
    char *path;
    char *bpath = NULL;
    if (s[0] == '/') {
        path = strdup(s);
    } else {
        bpath = vfs_get_fullpath(get_current_task()->cwd);
        path  = pathacat(bpath, s);
    }
    char *normalized_path = normalize_path(path);
    free(path);
    free(bpath);
    return normalized_path;
}

bool vfs_init() {
    for (size_t i = 0; i < sizeof(struct vfs_callback) / sizeof(void *); i++) {
        ((void **)&vfs_empty_callback)[i] = (void *)empty_func;
    }
    llist_init_head(&fs_metadata_list);
    rootdir       = vfs_node_alloc(NULL, "/");
    rootdir->type = file_dir;
    write_serial_fmt("Virtual File System initialize.\n");
    return true;
}

// VFS 原语导出给内核态模块。最小集合：只读/读写数据路径 + 追加式策略钩子。
// 根节点、路径规范化与创建/删除/截断原语不再导出；新增导出必须先确认
// in-tree 模块确实需要，并评估内核攻击面。
EXPORT_SYMBOL(vfs_open);
EXPORT_SYMBOL(vfs_close);
EXPORT_SYMBOL(vfs_read);
EXPORT_SYMBOL(vfs_write);
EXPORT_SYMBOL(vfs_add_access_hook);
