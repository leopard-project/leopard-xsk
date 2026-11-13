#include <user/permissions.h>

#include <cpu/lock.h>
#include <errno.h>
#include <fs/vfs/vfs.h>
#include <krlibc.h>
#include <task/scheduler.h>
#include <user/file_access.h>

// Mechanism: credentials, masks, consent queue, path classification.
// Policy: grade masks and path categories, loaded from permissions.conf.

namespace
{
spin_t permission_lock = SPIN_INIT;
uint64_t session_generation = 1, next_token = 1;
bool backend_available;
uint32_t outstanding_requests;

PermissionPolicy policy;
bool policy_loaded;
UserInfo session_user;
bool session_published;

struct PermissionRequest
{
    PermissionRequest *next;
    tcb_t owner;
    pcb_t process;
    PermissionPrompt prompt;
    uint64_t session, revision;
    uint8_t remaining;
    bool done, role_confirmed;
};
PermissionRequest *requests, *tail;
} // namespace

const PermissionPolicy *permissions_policy()
{
    return policy_loaded ? &policy : NULL;
}

uint8_t permissions_grade_mask(UserType type)
{
    if (!policy_loaded || type < XUT_Root || type >= XUT_Custom) return 0;
    return policy.grade_mask[type];
}

static uint8_t mask_of(const UserInfo *user)
{
    // A missing permissions.conf disables the model entirely (allow all),
    // matching permissions_file_access(); do not report an empty mask here,
    // otherwise syscalls such as XAPI_POWER_ACTION's preflight fail.
    if (!policy_loaded) return XPERM_ALL;
    if (user == NULL) return 0;
    if (user->user_type == XUT_Custom) return user_permission_mask(user->user_prms);
    uint8_t grade = permissions_grade_mask(user->user_type);
    return grade ? grade : user_permission_mask(user->user_prms);
}

static UserInfo identity_for(ProcessCredentialSource source, pcb_t parent)
{
    UserInfo out;
    memset(&out, 0, sizeof(out));

    if (source == XPCS_INHERIT && parent != NULL && parent != kernel_group)
    {
        out = parent->permission_user;
        return out;
    }

    bool ready = user_session_is_ready();
    const UserInfo *session = ready && session_published ? &session_user :
                              (current_user != NULL ? current_user : &root_user);
    if (session != NULL) out = *session;

    if (source == XPCS_VISITOR)
    {
        out.user_type = policy_loaded ? policy.visitor_type : XUT_Visitor;
    }
    else if (!ready || session == NULL)
    {
        // Kernel-owned launches before login use the configured bootstrap grade.
        out.user_type = policy_loaded ? policy.pre_login_type : XUT_Admin;
    }

    if (out.user_type == XUT_Custom) out.user_prms = user_permissions_from_mask(mask_of(&out));
    else out.user_prms = user_permissions_from_mask(permissions_grade_mask(out.user_type));
    memset(out.password, 0, sizeof(out.password));
    return out;
}

void permissions_session_publish(const UserInfo *info)
{
    if (info == NULL) return;
    spin_lock(&permission_lock);
    session_user = *info;
    memset(session_user.password, 0, sizeof(session_user.password));
    session_published = true;
    ++session_generation;
    spin_unlock(&permission_lock);
}

void permissions_process_init(pcb_t process, pcb_t parent, ProcessCredentialSource source)
{
    if (process == NULL) return;
    UserInfo identity = identity_for(source, parent);
    spin_lock(&permission_lock);
    process->permission_user = identity;
    process->permission_revision = 1;
    spin_unlock(&permission_lock);
}

void permissions_process_exec(pcb_t process)
{
    if (process == NULL) return;
    spin_lock(&permission_lock);
    __atomic_add_fetch(&process->permission_revision, 1ULL, __ATOMIC_RELEASE);
    spin_unlock(&permission_lock);
}

uint8_t permissions_current_mask()
{
    tcb_t task = get_current_task();
    if (task == NULL || task->parent_group == NULL || task->task_level != TASK_APPLICATION_LEVEL)
        return XPERM_ALL;
    return mask_of(&task->parent_group->permission_user);
}

UserType permissions_current_type()
{
    tcb_t task = get_current_task();
    if (task == NULL || task->parent_group == NULL) return XUT_Root;
    return task->parent_group->permission_user.user_type;
}

bool permissions_have(uint8_t required)
{
    return (permissions_current_mask() & required) == required;
}

// ---- consent queue -------------------------------------------------------

bool permissions_modal_active()
{
    return __atomic_load_n(&outstanding_requests, __ATOMIC_ACQUIRE) != 0;
}

static bool request_valid_locked(PermissionRequest *r)
{
    return r->session == session_generation &&
           r->revision == __atomic_load_n(&r->process->permission_revision, __ATOMIC_ACQUIRE) &&
           r->process->status != DEATH && r->process->status != OUT && r->process->status != ZOMBIE &&
           r->owner->status != DEATH && r->owner->status != OUT;
}

static PermissionRequest *first_locked()
{
    for (PermissionRequest *r = requests; r; r = r->next)
    {
        if (r->done) continue;
        if (!backend_available || !request_valid_locked(r)) { r->done = true; continue; }
        uint8_t current = mask_of(&r->process->permission_user);
        r->remaining &= ~current;
        if (!r->remaining) { r->done = true; continue; }
        if (!r->prompt.token)
        {
            r->prompt.token = next_token++;
            r->prompt.current = current;
            r->prompt.current_type = r->process->permission_user.user_type;
            r->prompt.asking = r->remaining;
        }
        return r;
    }
    return NULL;
}

bool permissions_prompt(PermissionPrompt *out)
{
    spin_lock(&permission_lock);
    PermissionRequest *r = first_locked();
    if (r && out) *out = r->prompt;
    spin_unlock(&permission_lock);
    return r != NULL;
}

static void unlink_locked(PermissionRequest *r)
{
    PermissionRequest **link = &requests, *previous = NULL;
    while (*link && *link != r) { previous = *link; link = &(*link)->next; }
    if (!*link) return;
    *link = r->next;
    __atomic_sub_fetch(&outstanding_requests, 1U, __ATOMIC_RELEASE);
    if (tail == r) tail = previous;
}

void permissions_answer(uint64_t token, bool approved)
{
    spin_lock(&permission_lock);
    PermissionRequest *r = first_locked();
    if (r && r->prompt.token == token)
    {
        if (approved)
        {
            UserInfo *user = &r->process->permission_user;
            uint8_t granted = r->prompt.asking & permissions_grade_mask(user->user_type);
            user->user_prms = user_permissions_from_mask(mask_of(user) | granted);
            user->user_type = XUT_Custom;
            __atomic_add_fetch(&r->process->permission_revision, 1ULL, __ATOMIC_RELEASE);
            r->role_confirmed = true;
        }
        r->remaining &= ~r->prompt.asking;
        r->prompt.token = 0;
        if (!r->remaining) r->done = true;
    }
    spin_unlock(&permission_lock);
}

void permissions_set_backend(bool available)
{
    spin_lock(&permission_lock);
    backend_available = available;
    if (!available)
        for (PermissionRequest *r = requests; r; r = r->next) r->done = true;
    spin_unlock(&permission_lock);
}

void permissions_thread_exit(tcb_t task)
{
    if (task == NULL) return;
    PermissionRequest *garbage = NULL;
    spin_lock(&permission_lock);
    for (PermissionRequest *r = requests, *next; r; r = next)
    {
        next = r->next;
        if (r->owner != task) continue;
        unlink_locked(r);
        r->next = garbage;
        garbage = r;
    }
    spin_unlock(&permission_lock);
    while (garbage)
    {
        PermissionRequest *next = garbage->next;
        free(garbage);
        garbage = next;
    }
}

static int request_permissions(uint8_t target, UserType type, bool individual)
{
    tcb_t task = get_current_task();
    if (task == NULL || task->parent_group == NULL || task->task_level != TASK_APPLICATION_LEVEL)
        return -EPERM;

    pcb_t process = task->parent_group;
    uint8_t current = mask_of(&process->permission_user);
    uint8_t missing = target & ~current;
    if (!missing) return 0;
    if (!backend_available)
    {
        // No consent UI: report the missing capability without blocking.
        if (!individual && type < process->permission_user.user_type)
            return missing ? (int)missing : -EACCES;
        return (int)missing;
    }

    PermissionRequest *r = (PermissionRequest *)calloc(1, sizeof(PermissionRequest));
    if (r == NULL) return -ENOMEM;
    r->owner = task;
    r->process = process;
    r->prompt.pid = process->pid;
    r->prompt.ppid = process->ppid;
    r->prompt.tid = task->tid;
    strncpy(r->prompt.name, process->name, sizeof(r->prompt.name) - 1);
    strncpy(r->prompt.path, process->exe_path ? process->exe_path : "(unknown)", sizeof(r->prompt.path) - 1);
    r->prompt.target_type = type;
    r->prompt.individual = individual;
    r->prompt.requested = target;
    r->remaining = missing;
    r->revision = __atomic_load_n(&process->permission_revision, __ATOMIC_ACQUIRE);

    spin_lock(&permission_lock);
    r->session = session_generation;
    strncpy(r->prompt.user, process->permission_user.name, sizeof(r->prompt.user) - 1);
    if (tail) tail->next = r; else requests = r;
    tail = r;
    __atomic_add_fetch(&outstanding_requests, 1U, __ATOMIC_RELEASE);
    spin_unlock(&permission_lock);

    for (;;)
    {
        spin_lock(&permission_lock);
        first_locked();
        if (r->done)
        {
            int failed = target & ~mask_of(&process->permission_user);
            if (failed && individual) failed = (int)(target & ~mask_of(&process->permission_user));
            unlink_locked(r);
            spin_unlock(&permission_lock);
            free(r);
            return failed ? failed : 0;
        }
        spin_unlock(&permission_lock);
        scheduler_sleep_ns(10000000ULL);
    }
}

int permissions_lift(UserType type)
{
    if (type < XUT_Root || type > XUT_Custom) return -EINVAL;
    uint8_t target = type == XUT_Custom ? XPERM_ALL : permissions_grade_mask(type);
    return request_permissions(target, type, false);
}

int permissions_request(uint8_t requested)
{
    if (requested & ~XPERM_ALL) return -EINVAL;
    return request_permissions(requested, XUT_Custom, true);
}

// ---- path policy ---------------------------------------------------------

enum AccessKind { ACCESS_READ, ACCESS_WRITE, ACCESS_REMOVE, ACCESS_MOUNT };

static bool path_under(const char *path, const char *prefix)
{
    if (path == NULL || prefix == NULL || prefix[0] == '\0') return false;
    size_t n = strlen(prefix);
    for (size_t i = 0; i < n; ++i)
    {
        char a = path[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a == '\0' || a != b) return false;
    }
    return path[n] == '\0' || path[n] == '/' || prefix[n - 1] == '/';
}

static bool any_prefix(const char *path, const char (*list)[PERMISSION_PATH_MAX], int count)
{
    for (int i = 0; i < count; ++i)
        if (list[i][0] && path_under(path, list[i])) return true;
    return false;
}

static bool own_home(const char *path, const UserInfo *identity)
{
    if (!policy_loaded || policy.users_dir[0] == '\0' || identity == NULL || identity->name[0] == '\0')
        return false;
    char home[PERMISSION_PATH_MAX];
    snprintf(home, sizeof(home), "%s/%s", policy.users_dir, identity->name);
    return path_under(path, home);
}

static int required_bits(const char *path, AccessKind kind)
{
    if (!policy_loaded || path == NULL) return 0;

    if (kind == ACCESS_MOUNT) return XPERM_SV;

    bool sys = policy.sys_dir[0] && path_under(path, policy.sys_dir);
    if (kind == ACCESS_READ && any_prefix(path, policy.public_read, PERMISSION_PUBLIC_MAX)) return 0;

    bool settings = (policy.settings_dir[0] && path_under(path, policy.settings_dir)) ||
                    any_prefix(path, policy.protected_write, PERMISSION_PROTECTED_MAX);

    if (kind == ACCESS_READ)
    {
        if (policy.users_dir[0] && path_under(path, policy.users_dir))
        {
            const char *name = NULL;
            tcb_t task = get_current_task();
            if (task != NULL && task->parent_group != NULL) name = task->parent_group->permission_user.name;
            UserInfo identity;
            memset(&identity, 0, sizeof(identity));
            if (name) strncpy(identity.name, name, sizeof(identity.name) - 1);
            return own_home(path, &identity) ? XPERM_R : (XPERM_R | XPERM_NV);
        }
        return sys ? XPERM_SR : XPERM_R;
    }

    if (kind == ACCESS_WRITE || kind == ACCESS_REMOVE)
    {
        if (settings) return XPERM_CS;
        if (sys) return XPERM_SW;
        if (policy.users_dir[0] && path_under(path, policy.users_dir))
        {
            const char *name = NULL;
            tcb_t task = get_current_task();
            if (task != NULL && task->parent_group != NULL) name = task->parent_group->permission_user.name;
            UserInfo identity;
            memset(&identity, 0, sizeof(identity));
            if (name) strncpy(identity.name, name, sizeof(identity.name) - 1);
            return own_home(path, &identity) ? XPERM_W : (XPERM_W | XPERM_NV);
        }
        return XPERM_W;
    }

    return 0;
}

static char *resolved_path(vfs_node_t node, const char *path)
{
    if (path != NULL && path[0] == '/') return NULL;
    if (node == NULL) return NULL;
    return vfs_get_fullpath(node);
}

static int check_path(vfs_node_t node, const char *path, AccessKind kind)
{
    if (!policy_loaded) return 0;
    char *allocated = resolved_path(node, path);
    const char *target = path != NULL && path[0] == '/' ? path : (allocated ? allocated : NULL);
    int missing = target ? (required_bits(target, kind) & ~permissions_current_mask()) : 0;
    free(allocated);
    return missing;
}

int permissions_file_error()
{
    tcb_t task = get_current_task();
    return task != NULL ? task->file_access_error : 0;
}

void permissions_clear_file_error()
{
    tcb_t task = get_current_task();
    if (task != NULL) task->file_access_error = 0;
}

bool permissions_file_access(vfs_node_t node, const char *path, vfs_access_op_t op)
{
    permissions_clear_file_error();
    AccessKind kind = ACCESS_READ;
    switch (op)
    {
    case VFS_ACCESS_READ: kind = ACCESS_READ; break;
    case VFS_ACCESS_WRITE: kind = ACCESS_WRITE; break;
    case VFS_ACCESS_REMOVE: kind = ACCESS_REMOVE; break;
    case VFS_ACCESS_MOUNT: kind = ACCESS_MOUNT; break;
    default: return true;
    }
    bool allowed = check_path(node, path, kind) == 0;
    if (!allowed)
    {
        tcb_t task = get_current_task();
        if (task != NULL) task->file_access_error = EACCES;
    }
    return allowed;
}

bool permissions_vfs_hook(vfs_node_t node, const char *path, vfs_access_op_t op)
{
    return permissions_file_access(node, path, op);
}

static AccessKind kind_from_access(uint64_t access)
{
    if (access & (XFA_REMOVE)) return ACCESS_REMOVE;
    if (access & (XFA_WRITE | XFA_CREATE)) return ACCESS_WRITE;
    return ACCESS_READ;
}

int permissions_query_file(const char *path, uint64_t access)
{
    if (!policy_loaded || path == NULL) return 0;
    return check_path(NULL, path, kind_from_access(access));
}

int permissions_request_file(const char *path, uint64_t access)
{
    int missing = permissions_query_file(path, access);
    if (missing <= 0) return missing;
    if (!backend_available) return missing;
    return request_permissions((uint8_t)missing, XUT_Custom, true);
}

// ---- policy loading ------------------------------------------------------

static uint8_t parse_mask(const char *value)
{
    while (*value == ' ' || *value == '\t') ++value;
    if (!strcmp(value, "all")) return XPERM_ALL;
    if (!strcmp(value, "none")) return 0;

    uint8_t mask = 0;
    const char *p = value;
    while (*p)
    {
        while (*p == ',' || *p == ' ' || *p == '\t') ++p;
        if (!p[0]) break;
        if (p[0] == 'R' && (p[1] == ',' || p[1] == '\0' || p[1] == ' ')) mask |= XPERM_R;
        else if ((p[0] == 'W') && (p[1] == ',' || p[1] == '\0' || p[1] == ' ')) mask |= XPERM_W;
        else if (p[0] == 'C' && p[1] == 'S') mask |= XPERM_CS;
        else if (p[0] == 'N' && p[1] == 'V') mask |= XPERM_NV;
        else if (p[0] == 'S' && p[1] == 'V') mask |= XPERM_SV;
        else if (p[0] == 'S' && p[1] == 'R') mask |= XPERM_SR;
        else if (p[0] == 'S' && p[1] == 'W') mask |= XPERM_SW;
        while (*p && *p != ',') ++p;
    }
    return mask;
}

static UserType parse_type(const char *value)
{
    if (!strcmp(value, "Root")) return XUT_Root;
    if (!strcmp(value, "System")) return XUT_System;
    if (!strcmp(value, "Admin")) return XUT_Admin;
    if (!strcmp(value, "Visitor")) return XUT_Visitor;
    if (!strcmp(value, "Custom")) return XUT_Custom;
    return XUT_Admin;
}

static void copy_value(char *dst, const char *src)
{
    while (*src == ' ' || *src == '\t') ++src;
    size_t len = strlen(src);
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t' || src[len - 1] == '\r')) --len;
    if (len >= PERMISSION_PATH_MAX) len = PERMISSION_PATH_MAX - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void add_prefix(char (*list)[PERMISSION_PATH_MAX], int capacity, const char *value)
{
    for (int i = 0; i < capacity; ++i)
    {
        if (list[i][0] == '\0') { copy_value(list[i], value); return; }
    }
}

static void parse_line(char *line)
{
    char *hash = strchr(line, '#');
    if (hash) *hash = '\0';
    char *eq = strchr(line, '=');
    if (eq == NULL) return;
    *eq = '\0';
    char *key = line, *value = eq + 1;
    size_t klen = strlen(key);
    while (klen > 0 && (key[klen - 1] == ' ' || key[klen - 1] == '\t')) key[--klen] = '\0';
    if (klen == 0) return;

    if (!strcmp(key, "grade.Root")) policy.grade_mask[XUT_Root] = parse_mask(value);
    else if (!strcmp(key, "grade.System")) policy.grade_mask[XUT_System] = parse_mask(value);
    else if (!strcmp(key, "grade.Admin")) policy.grade_mask[XUT_Admin] = parse_mask(value);
    else if (!strcmp(key, "grade.Visitor")) policy.grade_mask[XUT_Visitor] = parse_mask(value);
    else if (!strcmp(key, "launch.pre_login")) policy.pre_login_type = parse_type(value);
    else if (!strcmp(key, "launch.visitor")) policy.visitor_type = parse_type(value);
    else if (!strcmp(key, "sys_dir")) copy_value(policy.sys_dir, value);
    else if (!strcmp(key, "settings_dir")) copy_value(policy.settings_dir, value);
    else if (!strcmp(key, "users_dir")) copy_value(policy.users_dir, value);
    else if (!strcmp(key, "public_read")) add_prefix(policy.public_read, PERMISSION_PUBLIC_MAX, value);
    else if (!strcmp(key, "protected_write")) add_prefix(policy.protected_write, PERMISSION_PROTECTED_MAX, value);
}

void permissions_load_policy(const char *path)
{
    memset(&policy, 0, sizeof(policy));
    policy.pre_login_type = XUT_Admin;
    policy.visitor_type = XUT_Visitor;

    if (path == NULL) return;
    vfs_node_t file = vfs_open(path);
    if (file == NULL)
    {
        write_serial_fmt("PERM: %s not found, permission model disabled\n", path);
        return;
    }

    char buffer[4096];
    size_t got = vfs_read(file, buffer, 0, sizeof(buffer) - 1);
    vfs_close(file);
    if (got == (size_t)VFS_STATUS_FAILED || got == 0)
    {
        write_serial_fmt("PERM: %s unreadable, permission model disabled\n", path);
        return;
    }
    buffer[got] = '\0';

    char *line = buffer;
    while (line != NULL && *line != '\0')
    {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        parse_line(line);
        line = next;
    }

    policy_loaded = true;
    write_serial_fmt("PERM: loaded %s\n", path);
}

void permissions_init(const char *config_path)
{
    permissions_load_policy(config_path);
    vfs_add_access_hook(permissions_vfs_hook);
}
