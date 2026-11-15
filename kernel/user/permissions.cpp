#include <user/permissions.h>

#include <cpu/lock.h>
#include <errno.h>
#include <fs/vfs/vfs.h>
#include <krlibc.h>
#include <task/scheduler.h>
#include <user/file_access.h>
#include <user/user.h>

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

static bool application_context()
{
    tcb_t task = get_current_task();
    return task != NULL && task->task_level == TASK_APPLICATION_LEVEL;
}

static uint8_t mask_of(const UserInfo *user)
{
    // Fail closed: without a loaded policy only kernel/boot tasks keep the
    // allow-all path. Applications get an empty mask, so every path check
    // denies and consent requests cannot manufacture capabilities.
    if (!policy_loaded) return application_context() ? 0 : XPERM_ALL;
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
    const UserInfo *session = NULL;
    if (ready && session_published)
        session = &session_user;
    else if (current_user != NULL)
        session = current_user;
    if (session != NULL) out = *session;

    if (source == XPCS_VISITOR)
    {
        /* Visitor is an unauthenticated pseudo-identity, never the account
           identity. Mirror user_session_use_login(): name "Login", Visitor
           grade. This also prevents a Visitor from matching an Admin by name
           in kill/session checks. */
        memset(&out, 0, sizeof(out));
        strcpy(out.name, "Login");
        out.user_type = policy_loaded ? policy.visitor_type : XUT_Visitor;
    }
    else if (session == NULL)
    {
        /* No current user at all: never fall back to Admin. Use the same
           unauthenticated Visitor identity as user_session_use_login().
           user_session_use_root() sets current_user=root, so PID1 keeps Root. */
        memset(&out, 0, sizeof(out));
        strcpy(out.name, "Login");
        out.user_type = XUT_Visitor;
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

void permissions_process_switch_identity(pcb_t process, const UserInfo *identity)
{
    if (process == NULL || identity == NULL) return;

    UserInfo next = *identity;
    memset(next.password, 0, sizeof(next.password));
    if (next.user_type == XUT_Custom)
        next.user_prms = user_permissions_from_mask(mask_of(&next));
    else
        next.user_prms = user_permissions_from_mask(permissions_grade_mask(next.user_type));

    spin_lock(&permission_lock);
    process->permission_user = next;
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

static UserType xut_from_id(uint32_t id)
{
    switch (id)
    {
    case 0: return XUT_Root;
    case 1: return XUT_System;
    case 2: return XUT_Admin;
    case 1000: return XUT_Root; // legacy image owner (root-built image)
    case 65534: return XUT_Visitor;
    default: return XUT_Custom;
    }
}

static bool dac_allowed(vfs_node_t node, vfs_access_op_t op)
{
    if (node == NULL || op == VFS_ACCESS_LOOKUP) return true;

    tcb_t task = get_current_task();
    const UserInfo *cred =
        (task != NULL && task->parent_group != NULL) ? &task->parent_group->permission_user : NULL;
    UserType current_type = cred != NULL ? cred->user_type : XUT_Root;

    /* mount(2) is the equivalent of CAP_SYS_ADMIN: never Admin/Visitor/Custom,
       otherwise an Admin account could stack a filesystem over /system. */
    if (op == VFS_ACCESS_MOUNT) return current_type == XUT_Root || current_type == XUT_System;

    uint32_t mode = vfs_node_mode(node);
    bool is_dir = (node->type & file_dir) != 0;

    if (current_type == XUT_Root)
    {
        // Root bypasses DAC read/write, but exec still requires an x bit.
        if (op == VFS_ACCESS_EXEC && !is_dir && (mode & 0111) == 0) return false;
        return true;
    }

    uint8_t bits;
    UserType owner_type = xut_from_id(node->owner);
    UserType group_type = xut_from_id(node->group);
    if (owner_type == current_type &&
        (current_type != XUT_Custom || (cred != NULL && node->owner == user_uid(cred))))
        bits = (mode >> 6) & 7;
    else if (group_type == current_type &&
             (current_type != XUT_Custom || (cred != NULL && node->group == user_gid(cred))))
        bits = (mode >> 3) & 7;
    else
        bits = mode & 7;

    uint8_t required = 0;
    switch (op)
    {
    case VFS_ACCESS_READ:
    case VFS_ACCESS_LIST:
    case VFS_ACCESS_STAT:
    case VFS_ACCESS_EXPORT: required = 4; break;
    case VFS_ACCESS_WRITE:
    case VFS_ACCESS_REMOVE: required = 2; break;
    case VFS_ACCESS_EXEC: required = 1; break;
    case VFS_ACCESS_READ_WRITE: required = 6; break;
    default: return true;
    }
    return (bits & required) == required;
}

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
    if (!policy_loaded)
    {
        // Fail closed: kernel/boot tasks may continue (the hook is still
        // installed), but application access is denied when no policy loaded.
        return application_context() ? XPERM_ALL : 0;
    }
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

static bool path_allowed_for(vfs_node_t node, const char *path, vfs_access_op_t op)
{
    if (!policy_loaded) return !application_context();

    switch (op)
    {
    case VFS_ACCESS_LIST:
    case VFS_ACCESS_STAT:
    case VFS_ACCESS_EXPORT:
    case VFS_ACCESS_EXEC:
        return check_path(node, path, ACCESS_READ) == 0;
    case VFS_ACCESS_WRITE:
        return check_path(node, path, ACCESS_WRITE) == 0;
    case VFS_ACCESS_REMOVE:
        return check_path(node, path, ACCESS_REMOVE) == 0;
    case VFS_ACCESS_MOUNT:
        return check_path(node, path, ACCESS_MOUNT) == 0;
    case VFS_ACCESS_READ_WRITE:
        return check_path(node, path, ACCESS_READ) == 0 &&
               check_path(node, path, ACCESS_WRITE) == 0;
    case VFS_ACCESS_READ:
    default:
        return check_path(node, path, ACCESS_READ) == 0;
    }
}

bool permissions_file_access(vfs_node_t node, const char *path, vfs_access_op_t op)
{
    permissions_clear_file_error();

    /* Internal name lookup exposes no contents or listing. */
    if (op == VFS_ACCESS_LOOKUP) return true;

    /* Raw block devices are never exposed to unprivileged applications:
       read/write/remove/mount/export all require ChangeSettings. Node type,
       not a hardcoded device path, is the discriminator. */
    if (application_context() && node != NULL && (node->type & file_block))
    {
        tcb_t task = get_current_task();
        uint8_t mask = (task != NULL && task->parent_group != NULL)
                           ? mask_of(&task->parent_group->permission_user) : 0;
        if ((mask & XPERM_CS) == 0)
        {
            if (task != NULL) task->file_access_error = EACCES;
            return false;
        }
    }

    /* Temporary transition policy: file-level DAC and the existing path
       policy must BOTH allow. The path rules keep user-home isolation until
       image/package ownership and modes are finalized. */
    bool dac = dac_allowed(node, op);
    bool path_ok = path_allowed_for(node, path, op);
    bool allowed = dac && path_ok;
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

int permissions_query_file(const char *path, uint64_t access)
{
    if (path == NULL) return 0;
    if (!policy_loaded)
    {
        // Fail closed for applications. The path-only query cannot know the
        // node type, so raw block devices are enforced by the VFS hook only.
        return application_context() ? (int)XPERM_ALL : 0;
    }

    int missing = 0;
    if (access & XFA_READ) missing |= check_path(NULL, path, ACCESS_READ);
    if (access & XFA_EXPORT) missing |= check_path(NULL, path, ACCESS_READ);
    if (access & XFA_WRITE) missing |= check_path(NULL, path, ACCESS_WRITE);
    if (access & XFA_CREATE) missing |= check_path(NULL, path, ACCESS_WRITE);
    if (access & XFA_LIST) missing |= check_path(NULL, path, ACCESS_READ);
    if (access & XFA_STAT) missing |= check_path(NULL, path, ACCESS_READ);
    if (access & XFA_REMOVE) missing |= check_path(NULL, path, ACCESS_REMOVE);
    return missing;
}

int permissions_request_file(const char *path, uint64_t access)
{
    int missing = permissions_query_file(path, access);
    if (missing <= 0) return missing;
    if (!backend_available) return missing;
    return request_permissions((uint8_t)missing, XUT_Custom, true);
}

// ---- policy loading ------------------------------------------------------

static bool token_is(const char *p, size_t len, const char *name)
{
    size_t i = 0;
    for (; i < len && name[i] != '\0'; ++i)
        if (p[i] != name[i]) return false;
    return i == len && name[i] == '\0';
}

static bool parse_mask(const char *value, uint8_t *out)
{
    while (*value == ' ' || *value == '\t') ++value;
    size_t len = strlen(value);
    while (len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\t' || value[len - 1] == '\r'))
        --len;

    if (len == 0) { *out = 0; return true; } // Empty value grants nothing.
    if (token_is(value, len, "all")) { *out = XPERM_ALL; return true; }
    if (token_is(value, len, "none")) { *out = 0; return true; }

    uint8_t mask = 0;
    size_t i = 0;
    while (i < len)
    {
        while (i < len && (value[i] == ',' || value[i] == ' ' || value[i] == '\t')) ++i;
        size_t start = i;
        while (i < len && value[i] != ',') ++i;
        size_t end = i;
        while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r'))
            --end;
        if (end == start) return false; // Empty or trailing-comma token.

        const char *t = value + start;
        size_t tlen = end - start;
        if (token_is(t, tlen, "R")) mask |= XPERM_R;
        else if (token_is(t, tlen, "W")) mask |= XPERM_W;
        else if (token_is(t, tlen, "CS")) mask |= XPERM_CS;
        else if (token_is(t, tlen, "NV")) mask |= XPERM_NV;
        else if (token_is(t, tlen, "SV")) mask |= XPERM_SV;
        else if (token_is(t, tlen, "SR")) mask |= XPERM_SR;
        else if (token_is(t, tlen, "SW")) mask |= XPERM_SW;
        else return false;

        if (i < len) ++i;
    }
    *out = mask;
    return true;
}

static UserType clamp_unprivileged(UserType type)
{
    /* launch.pre_login / launch.visitor are unauthenticated fallbacks. A
       policy that asks for Root/System/Admin here is a privilege-escalation
       footgun, so clamp it to Visitor. */
    return (type == XUT_Root || type == XUT_System || type == XUT_Admin) ? XUT_Visitor : type;
}

static bool parse_type(const char *value, UserType *out)
{
    while (*value == ' ' || *value == '\t') ++value;
    size_t len = strlen(value);
    while (len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\t' || value[len - 1] == '\r'))
        --len;

    if (token_is(value, len, "Root")) *out = XUT_Root;
    else if (token_is(value, len, "System")) *out = XUT_System;
    else if (token_is(value, len, "Admin")) *out = XUT_Admin;
    else if (token_is(value, len, "Visitor")) *out = XUT_Visitor;
    else if (token_is(value, len, "Custom")) *out = XUT_Custom;
    else return false;
    return true;
}

static bool copy_value(char *dst, const char *src)
{
    while (*src == ' ' || *src == '\t') ++src;
    size_t len = strlen(src);
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t' || src[len - 1] == '\r')) --len;
    // Overflow is a policy error, never a silent truncation.
    if (len >= PERMISSION_PATH_MAX) return false;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return true;
}

static bool add_prefix(char (*list)[PERMISSION_PATH_MAX], int capacity, const char *value)
{
    for (int i = 0; i < capacity; ++i)
        if (list[i][0] == '\0') return copy_value(list[i], value);
    return false; // Prefix table full: reject the policy instead of dropping it.
}

static bool parse_line(char *line, bool *grade_seen)
{
    char *hash = strchr(line, '#');
    if (hash) *hash = '\0';
    char *eq = strchr(line, '=');
    if (eq == NULL) return true;
    *eq = '\0';
    char *key = line, *value = eq + 1;
    size_t klen = strlen(key);
    while (klen > 0 && (key[klen - 1] == ' ' || key[klen - 1] == '\t' || key[klen - 1] == '\r'))
        key[--klen] = '\0';
    if (klen == 0) return true;

    if (!strcmp(key, "grade.Root"))
    {
        if (!parse_mask(value, &policy.grade_mask[XUT_Root])) return false;
        grade_seen[XUT_Root] = true;
    }
    else if (!strcmp(key, "grade.System"))
    {
        if (!parse_mask(value, &policy.grade_mask[XUT_System])) return false;
        grade_seen[XUT_System] = true;
    }
    else if (!strcmp(key, "grade.Admin"))
    {
        if (!parse_mask(value, &policy.grade_mask[XUT_Admin])) return false;
        grade_seen[XUT_Admin] = true;
    }
    else if (!strcmp(key, "grade.Visitor"))
    {
        if (!parse_mask(value, &policy.grade_mask[XUT_Visitor])) return false;
        grade_seen[XUT_Visitor] = true;
    }
    else if (!strcmp(key, "launch.pre_login"))
    {
        if (!parse_type(value, &policy.pre_login_type)) return false;
    }
    else if (!strcmp(key, "launch.visitor"))
    {
        if (!parse_type(value, &policy.visitor_type)) return false;
    }
    else if (!strcmp(key, "sys_dir")) return copy_value(policy.sys_dir, value);
    else if (!strcmp(key, "settings_dir")) return copy_value(policy.settings_dir, value);
    else if (!strcmp(key, "users_dir")) return copy_value(policy.users_dir, value);
    else if (!strcmp(key, "public_read"))
        return add_prefix(policy.public_read, PERMISSION_PUBLIC_MAX, value);
    else if (!strcmp(key, "protected_write"))
        return add_prefix(policy.protected_write, PERMISSION_PROTECTED_MAX, value);
    // Unknown keys are ignored so newer configs do not brick older kernels.
    return true;
}

bool permissions_load_policy(const char *path)
{
    policy_loaded = false;
    memset(&policy, 0, sizeof(policy));
    policy.pre_login_type = XUT_Visitor;
    policy.visitor_type = XUT_Visitor;

    if (path == NULL)
    {
        write_serial_fmt("PERM: no policy path, refusing to load\n");
        return false;
    }

    vfs_node_t file = vfs_open(path);
    if (file == NULL)
    {
        write_serial_fmt("PERM: %s missing/unreadable, policy not loaded\n", path);
        return false;
    }

    char buffer[4096];
    size_t got = vfs_read(file, buffer, 0, sizeof(buffer) - 1);
    vfs_close(file);
    if (got == (size_t)VFS_STATUS_FAILED || got == 0)
    {
        write_serial_fmt("PERM: %s empty/unreadable, policy not loaded\n", path);
        return false;
    }
    if (got == sizeof(buffer) - 1)
    {
        // A full buffer may mean a truncated read; never parse a partial policy.
        write_serial_fmt("PERM: %s too large/truncated, policy not loaded\n", path);
        return false;
    }
    buffer[got] = '\0';

    bool grade_seen[XUT_Custom];
    memset(grade_seen, 0, sizeof(grade_seen));

    char *line = buffer;
    while (line != NULL && *line != '\0')
    {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        if (!parse_line(line, grade_seen))
        {
            write_serial_fmt("PERM: %s has an invalid line, policy not loaded\n", path);
            return false;
        }
        line = next;
    }

    for (int i = 0; i < XUT_Custom; ++i)
    {
        if (!grade_seen[i])
        {
            write_serial_fmt("PERM: %s missing a standard grade.* entry, policy not loaded\n", path);
            return false;
        }
    }

    policy.pre_login_type = clamp_unprivileged(policy.pre_login_type);
    policy.visitor_type   = clamp_unprivileged(policy.visitor_type);

    policy_loaded = true;
    write_serial_fmt("PERM: loaded %s\n", path);
    return true;
}

bool permissions_init(const char *config_path)
{
    // Install the hook even when loading fails: the hook is fail-closed for
    // applications, so continuing callers cannot bypass the model.
    bool loaded = permissions_load_policy(config_path);
    if (!vfs_add_access_hook(permissions_vfs_hook))
    {
        write_serial_fmt("PERM: VFS access hook table full, policy enforcement unavailable\n");
        return false;
    }
    return loaded;
}
