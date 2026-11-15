#pragma once

#include <fs/vfs/vfs.h>
#include <task/pcb.h>
#include <user/permission_types.h>

// Permission mechanism only: credentials, masks, consent queue and the VFS
// decision hook. Which grade gets which bits, and which paths are public,
// protected or system-owned, come from permissions.conf (policy data).

enum ProcessCredentialSource
{
    XPCS_LOGIN,   // Follow the current login session.
    XPCS_INHERIT, // Copy the parent process credentials.
    XPCS_VISITOR  // Start as the configured visitor identity.
};

#define PERMISSION_PATH_MAX 128
#define PERMISSION_PUBLIC_MAX 16
#define PERMISSION_PROTECTED_MAX 8

typedef struct
{
    char public_read[PERMISSION_PUBLIC_MAX][PERMISSION_PATH_MAX];
    char protected_write[PERMISSION_PROTECTED_MAX][PERMISSION_PATH_MAX];
    char sys_dir[PERMISSION_PATH_MAX];
    char settings_dir[PERMISSION_PATH_MAX];
    char users_dir[PERMISSION_PATH_MAX];
    uint8_t grade_mask[XUT_Custom]; // Root, System, Admin, Visitor
    UserType pre_login_type;
    UserType visitor_type;
} PermissionPolicy;

struct PermissionPrompt
{
    uint64_t token;
    size_t pid, tid;
    uint64_t ppid;
    char name[32], path[256], user[64];
    UserType current_type, target_type;
    uint8_t current, requested, asking;
    bool individual;
};

// Load permissions.conf and install the VFS decision hook. Returns false on a
// missing, empty, truncated or invalid file; the hook is installed regardless
// and denies application access while no policy is loaded (fail-closed).
bool permissions_init(const char *config_path);
bool permissions_load_policy(const char *path);
const PermissionPolicy *permissions_policy();

uint8_t permissions_grade_mask(UserType type);
void permissions_session_publish(const UserInfo *info);
void permissions_process_init(pcb_t process, pcb_t parent, ProcessCredentialSource source);
void permissions_process_exec(pcb_t process);
// Drop/change the current process credentials (login/getty transition).
void permissions_process_switch_identity(pcb_t process, const UserInfo *identity);
void permissions_thread_exit(tcb_t task);

uint8_t permissions_current_mask();
UserType permissions_current_type();
bool permissions_have(uint8_t required);

int permissions_lift(UserType type);
int permissions_request(uint8_t requested);
bool permissions_prompt(PermissionPrompt *out);
bool permissions_modal_active();
void permissions_answer(uint64_t token, bool approved);
void permissions_set_backend(bool available);

bool permissions_file_access(vfs_node_t node, const char *path, vfs_access_op_t op);
bool permissions_vfs_hook(vfs_node_t node, const char *path, vfs_access_op_t op);

// Last denial on this thread, so callers can preserve EACCES instead of EIO.
int  permissions_file_error();
void permissions_clear_file_error();

// XAPI_QUERY_FILE_ACCESS / XAPI_REQUEST_FILE_ACCESS helper.
// Returns 0 when permitted, a positive missing-bit mask, or a negative errno.
int permissions_query_file(const char *path, uint64_t access);
int permissions_request_file(const char *path, uint64_t access);
