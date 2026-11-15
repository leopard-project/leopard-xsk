#include <ahci/ahci.h>
#include <efi/boot.h>
#include <efi/efi.h>
#include <efi/fbc.h>
#include <fs/fatfs/fatfs.h>
#include <fs/partition.h>
#include <fs/vfs/devfs.h>
#include <fs/vfs/vfs.h>
#include <krlibc.h>
#include <mm/frame.h>
#include <mm/uaccess.h>
#include <openxj380/config.h>
#include <pci/pci.h>
#include <proto.hpp>
#include <power.h>
#include <rtc.h>
#include <syscall/pxapi.h>
#include <syscall/xapi_user.h>
#include <task/pcb.h>
#include <task/scheduler.h>
#include <syscall/syscall.h>
#include "../../build_settings.h"
#include <user/runfile.h>
#include <user/permissions.h>
#include <user/settings.h>
#include <user/user.h>
#include <errno.h>
#include <x3tp.h>
#include <ps2/keyboard.h>

extern UserInfo *current_user;
extern lock_queue *pcb_group_queue;
extern EFI_SYSTEM_TABLE *EFI_ST;
extern BOOT_CONFIG *EFI_BC;

uint64_t do_xapi_GetTime()
{
    tm t;
    time_read(&t);
    return mktime(&t);
}

/*
 * Hard cap on a single XAPI_GET_TASK_LIST request. The kernel-side staging
 * array below is sized from this cap, so a caller cannot force an unbounded
 * heap allocation; anything larger is simply truncated to 256 records.
 */
static constexpr size_t XAPI_TASK_LIST_MAX = 256;
static constexpr int    XAPI_INIT_PID      = 1;

static bool p_xapi_is_root_or_system(const UserInfo *identity)
{
    if (identity == NULL) return false;
    return identity->user_type == XUT_Root || identity->user_type == XUT_System;
}

/*
 * Same-login check: the process-owned permission identity must match by grade
 * and name. Names must be non-empty so a zero-initialised credential block
 * never matches another zero-initialised one; the grade must match too so a
 * custom account cannot impersonate a Root/System identity by name.
 */
static bool p_xapi_same_identity(const UserInfo *a, const UserInfo *b)
{
    if (a == NULL || b == NULL) return false;
    if (a->name[0] == '\0' || b->name[0] == '\0') return false;
    if (a->user_type != b->user_type) return false;

    if (strncmp(a->name, b->name, sizeof(a->name)) == 0) return true;

    /* Custom identities derive their stable uid from the account name, so an
       equal derived uid denotes the same account even if the name was edited. */
    if (a->user_type != XUT_Custom) return false;
    return user_uid(a) == user_uid(b);
}

/*
 * Kernel-level tasks (kernel threads, idle, the "XSK System" group) and PID1
 * are infrastructure: no user request may tear them down. kill_proc() enforces
 * this as well, but reporting EPERM here keeps the syscall contract honest.
 */
static bool p_xapi_process_protected(const pcb_t proc)
{
    if (proc == NULL) return true;
    if (proc->pid == XAPI_INIT_PID) return true;
    if (proc == kernel_group) return true;
    return proc->task_level != TASK_APPLICATION_LEVEL;
}

uint64_t do_xapi_KillProcess(uint64_t pid)
{
    tcb_t caller      = get_current_task();
    pcb_t caller_proc = caller != NULL ? caller->parent_group : NULL;
    if (caller_proc == NULL) return (uint64_t)-EPERM;

    pcb_t proc = found_pcb((int)pid);
    if (proc == NULL) return (uint64_t)-ESRCH;

    // PID1/init and kernel-level service groups are never user-killable, even
    // for Root. This mirrors the guard inside kill_proc().
    if (p_xapi_process_protected(proc)) return (uint64_t)-EPERM;

    /*
     * Policy:
     *   - killing yourself is always allowed (an app may exit via this call);
     *   - same permission identity (same login) is allowed;
     *   - cross-identity kills require Root/System credentials.
     *
     * XPERM_CS alone must not authorise this: Admin has XPERM_CS but must not
     * be able to terminate another identity's session. kill_proc() keeps its
     * own kernel/PID1 guard for every caller.
     */
    bool same_process  = (proc == caller_proc);
    bool privileged    = p_xapi_is_root_or_system(&caller_proc->permission_user);
    bool same_identity = p_xapi_same_identity(&proc->permission_user, &caller_proc->permission_user);
    if (!same_process && !privileged && !same_identity) return (uint64_t)-EPERM;

    kill_proc(proc, 0, false);
    return 0;
}

uint64_t do_xapi_GetTaskList(uint64_t buffer, uint64_t max_count)
{
    if (max_count == 0) return 0;
    if (buffer == 0) return (uint64_t)-EFAULT;
    if (max_count > XAPI_TASK_LIST_MAX) max_count = XAPI_TASK_LIST_MAX;

    page_directory_t *pagedir = xapi_current_pagedir();
    if (pagedir == NULL) return (uint64_t)-EFAULT;

    size_t capacity = 0;
    if (!xapi_checked_mul_size((size_t)max_count, sizeof(XapiTaskInfo), &capacity) ||
        capacity > (size_t)XAPI_TASK_LIST_MAX * sizeof(XapiTaskInfo))
        return (uint64_t)-EINVAL;

    // Reject a buffer that cannot hold even one record before allocating
    // anything; the exact range is re-validated once the real count is known.
    if (!user_range_mapped(pagedir, (const void *)buffer, sizeof(XapiTaskInfo)))
        return (uint64_t)-EFAULT;

    XapiTaskInfo *infos = (XapiTaskInfo *)calloc((size_t)max_count, sizeof(XapiTaskInfo));
    if (infos == NULL) return (uint64_t)-ENOMEM;

    tcb_t caller      = get_current_task();
    pcb_t caller_proc = caller != NULL ? caller->parent_group : NULL;

    /*
     * Enumeration policy: Root/System (and XPERM_CS holders) may see every
     * process. Everyone else sees only their own process group and processes
     * owned by the same permission identity; kernel-level groups are hidden.
     */
    bool enumerate_all = caller_proc == NULL ||
                         p_xapi_is_root_or_system(&caller_proc->permission_user) ||
                         permissions_have(XPERM_CS);

    size_t count = 0;
    if (pcb_group_queue != NULL) {
        // Snapshot only the cheap scalar fields while the queue is locked;
        // user copies happen after the unlock because they may fault/paging.
        spin_lock(&pcb_group_queue->lock);
        queue_foreach(pcb_group_queue, node)
        {
            pcb_t proc = (pcb_t)node->data;
            if (proc == NULL || count >= (size_t)max_count) continue;
            bool visible = enumerate_all ||
                           (proc == caller_proc) ||
                           (proc->task_level == TASK_APPLICATION_LEVEL &&
                            p_xapi_same_identity(&proc->permission_user, &caller_proc->permission_user));
            if (!visible) continue;

            XapiTaskInfo *info = &infos[count];
            memset(info, 0, sizeof(*info));
            info->pid            = (uint64_t)proc->pid;
            info->ppid           = proc->ppid;
            info->task_level     = (uint64_t)proc->task_level;
            info->process_status = (uint32_t)proc->status;
            strncpy(info->process_name, proc->name, sizeof(info->process_name) - 1);
            count++;
        }
        spin_unlock(&pcb_group_queue->lock);
    }

    uint64_t result = (uint64_t)count;
    if (count > 0)
    {
        size_t bytes = count * sizeof(XapiTaskInfo);
        if (!user_range_mapped(pagedir, (const void *)buffer, bytes) ||
            !copy_to_user_pagedir(pagedir, (void *)buffer, infos, bytes))
            result = (uint64_t)-EFAULT;
    }
    free(infos);
    return result;
}

void do_xapi_FlushTime()
{
    /* 无 RTC 写回支持：no-op */
}

// Helpers prefixed with p_xapi_ are private to this translation unit.
void p_xapi_output_kernel(const char *str)
{
#if OPENXJ380_INPUT_OUTPUT_DISABLED
    (void)str;
    return;
#else
    if (str == NULL) return;

    pcb_t front_p = get_current_task()->parent_group;
    pcb_t caller = front_p;
    while (true)
    {
        if (front_p == NULL || front_p == kernel_group)
        {
            write_serial_string(str);
            return;
        }
        if (front_p->terminal->console_owner)
        {
            /* The terminal owner (getty console host) renders child output
             * directly.  Child processes still write into the terminal's
             * X3TP terminal output buffer. */
            if (front_p == get_current_task()->parent_group) {
                write_serial_string(str);
                return;
            }

            // found it!
            if (str == NULL) {
                return;
            }

            char  *output    = front_p->terminal->output;
            size_t  out_cap  = sizeof(front_p->terminal->output);
            size_t  out_len  = strlen(output);
            size_t  str_len  = strlen(str);
            size_t  copy_len = str_len;

            if (out_len < out_cap - 1)
            {
                size_t avail = out_cap - out_len - 1;
                if (copy_len > avail) copy_len = avail;
                memcpy(output + out_len, str, copy_len);
                output[out_len + copy_len] = '\0';
            }

            front_p->terminal->output_lock = false;

            while (true)
            {
                if (front_p->terminal->output_lock) break;
                scheduler_yield();
            }
            memset(front_p->terminal->output, 0, sizeof(front_p->terminal->output));
            return;
        }
        front_p = front_p->parent_task;
    }
#endif
}

static void p_xapi_free_argv(char **argv)
{
    if (argv == NULL) return;
    for (size_t i = 0; argv[i] != NULL; i++)
    {
        free(argv[i]);
    }
    free(argv);
}

static int p_xapi_copy_argv_from_user(char **user_argv, char ***out)
{
    if (out == NULL) return -EINVAL;
    *out = NULL;
    if (user_argv == NULL) return 0;

    page_directory_t *pagedir = xapi_current_pagedir();
    if (pagedir == NULL) return -EFAULT;

    char **argv = (char **)calloc(XAPI_RUN_ARG_MAX + 1, sizeof(char *));
    if (argv == NULL) return -ENOMEM;

    for (size_t i = 0; i < XAPI_RUN_ARG_MAX; i++)
    {
        char *user_arg = NULL;
        if (!copy_from_user_pagedir(pagedir, &user_arg, &user_argv[i], sizeof(user_arg)))
        {
            p_xapi_free_argv(argv);
            return -EFAULT;
        }
        if (user_arg == NULL)
        {
            *out = argv;
            return 0;
        }
        int ret = xapi_copy_string_from_user(&argv[i], user_arg, XAPI_USER_STRING_MAX);
        if (ret < 0)
        {
            p_xapi_free_argv(argv);
            return ret;
        }
    }

    p_xapi_free_argv(argv);
    return -E2BIG;
}

static size_t p_xapi_strnlen(const char *str, size_t max_len)
{
    if (str == NULL) return 0;
    size_t len = 0;
    while (len < max_len && str[len] != '\0') len++;
    return len;
}

void do_xapi_GetSystemVersion(uint64_t str)
{
    if (str == 0) return;
    copy_to_user_pagedir(xapi_current_pagedir(), (void *)str, OS_VERSION, strlen(OS_VERSION) + 1);
}

void do_xapi_GetCurrentUser(uint64_t dst)
{
    if (dst == 0 || current_user == NULL) return;
    xapi_type_UserInfo info;
    memset(&info, 0, sizeof(info));
    strncpy(info.name, current_user->name, sizeof(info.name) - 1);
    info.user_type = current_user->user_type;
    copy_to_user_pagedir(xapi_current_pagedir(), (void *)dst, &info, sizeof(info));
}

uint64_t do_xapi_UserOobeRequired()
{
    return user_session_needs_oobe() ? 1 : 0;
}

uint64_t do_xapi_UserList(uint64_t buffer, uint64_t max_count)
{
    if (buffer == 0) return (uint64_t)-EINVAL;
    if (max_count > 128) max_count = 128;

    UserInfo users[128];
    memset(users, 0, sizeof(users));
    int count = user_session_list(users, (int)max_count);
    if (count < 0) return (uint64_t)count;

    xapi_type_LoginUserInfo out[128];
    memset(out, 0, sizeof(out));
    for (int i = 0; i < count; i++)
    {
        strncpy(out[i].name, users[i].name, sizeof(out[i].name) - 1);
        out[i].user_type = users[i].user_type;
    }

    if (count > 0 && !copy_to_user_pagedir(xapi_current_pagedir(), (void *)buffer, out,
                                           sizeof(xapi_type_LoginUserInfo) * (size_t)count))
    {
        return (uint64_t)-EFAULT;
    }
    return (uint64_t)count;
}

uint64_t do_sxah_check_password(uint64_t username, uint64_t password)
{
    tcb_t task = get_current_task();
    if (task == NULL || task->parent_group == NULL) return (uint64_t)-ESRCH;

    /* 只对已提权（effective Root）的进程开放密码校验：
       doas 由 setuid exec 拿到 Root，再回来核对自己用户的密码。
       普通进程拿不到这个 oracle。 */
    if (task->parent_group->permission_user.user_type != XUT_Root)
        return (uint64_t)-EPERM;

    char *kusername = NULL;
    char *kpassword = NULL;
    int   ret;

    ret = xapi_copy_string_from_user(&kusername, (const char *)username,
                                     sizeof(((UserInfo *)0)->name));
    if (ret < 0) return (uint64_t)ret;
    ret = xapi_copy_string_from_user(&kpassword, (const char *)password,
                                     sizeof(((UserInfo *)0)->password));
    if (ret < 0) {
        free(kusername);
        return (uint64_t)ret;
    }

    ret = user_session_check_password(kusername, kpassword);
    memset(kpassword, 0, sizeof(((UserInfo *)0)->password));
    free(kpassword);
    free(kusername);
    return (uint64_t)ret;
}

uint64_t do_xapi_UserLogin(uint64_t username, uint64_t password)
{
    char *kusername = NULL;
    char *kpassword = NULL;
    int ret = xapi_copy_string_from_user(&kusername, (const char *)username, sizeof(((UserInfo *)0)->name));
    if (ret < 0) return (uint64_t)ret;
    ret = xapi_copy_string_from_user(&kpassword, (const char *)password, sizeof(((UserInfo *)0)->password));
    if (ret < 0)
    {
        free(kusername);
        return (uint64_t)ret;
    }

    ret = user_session_login(kusername, kpassword);
    memset(kpassword, 0, sizeof(((UserInfo *)0)->password));
    free(kusername);
    free(kpassword);
    return (uint64_t)ret;
}

uint64_t do_xapi_UserCreateFirst(uint64_t username, uint64_t password)
{
    char *kusername = NULL;
    char *kpassword = NULL;
    int ret = xapi_copy_string_from_user(&kusername, (const char *)username, sizeof(((UserInfo *)0)->name));
    if (ret < 0) return (uint64_t)ret;
    ret = xapi_copy_string_from_user(&kpassword, (const char *)password, sizeof(((UserInfo *)0)->password));
    if (ret < 0)
    {
        free(kusername);
        return (uint64_t)ret;
    }

    ret = user_session_create_first(kusername, kpassword);
    memset(kpassword, 0, sizeof(((UserInfo *)0)->password));
    free(kusername);
    free(kpassword);
    return (uint64_t)ret;
}

void do_xapi_Output(char *str)
{
#if OPENXJ380_INPUT_OUTPUT_DISABLED
    (void)str;
    return;
#else
    char *kstr = NULL;
    if (xapi_copy_string_from_user(&kstr, str, XAPI_USER_STRING_MAX) < 0) return;
    p_xapi_output_kernel(kstr);
    free(kstr);
#endif
}

int do_xapi_Input(char *str, size_t capacity, uint64_t flags)
{
#if OPENXJ380_INPUT_OUTPUT_DISABLED
    (void)str;
    (void)capacity;
    (void)flags;
    return -ENOSYS;
#else
    if (str == NULL) return -EFAULT;
    if (capacity == 0 || capacity > XAPI_USER_STRING_MAX) return -EINVAL;
    if ((flags & ~((uint64_t)XAPI_INPUT_NO_ECHO)) != 0) return -EINVAL;

    pcb_t front_p = get_current_task()->parent_group;
    while (true)
    {
        if (front_p == NULL || front_p == kernel_group)
        {
            char input[XAPI_USER_STRING_MAX];
            size_t index = 0;
            size_t discarded = 0;
            while (true)
            {
                uint8_t value = get_keyboard_input();
                if (value == 0) { scheduler_yield(); continue; }
                if (value == '\b')
                {
                    if (discarded > 0)
                    {
                        discarded--;
                    }
                    else if (index > 0)
                    {
                        index--;
                    }
                    else continue;
                    if ((flags & XAPI_INPUT_NO_ECHO) == 0) write_serial_string("\b \b");
                    continue;
                }
                if (value == '\n') { write_serial_string("\n"); break; }
                if (value >= 32 && value < 127)
                {
                    if (index < capacity - 1) input[index++] = (char)value;
                    else if (discarded != (~(size_t)0)) discarded++;
                    if ((flags & XAPI_INPUT_NO_ECHO) == 0)
                    {
                        char echo[2] = {(char)value, '\0'};
                        write_serial_string(echo);
                    }
                }
            }
            input[index] = '\0';
            bool copied = copy_to_user_pagedir(xapi_current_pagedir(), str, input, index + 1);
            if ((flags & XAPI_INPUT_NO_ECHO) != 0) memset(input, 0, sizeof(input));
            return copied ? 0 : -EFAULT;
        }
        if (front_p->terminal->console_owner)
        {
            front_p->terminal->view.input_flags = (uint32_t)flags;
            front_p->terminal->view.wait_for_input = true;
            // 等待命令行输入
            while (true)
            {
                if (front_p->terminal->view.input_lock) break;
                scheduler_yield();
            }
            size_t input_len = p_xapi_strnlen(front_p->terminal->view.line, sizeof(front_p->terminal->view.line) - 1);
            while (input_len > 0 && (front_p->terminal->view.line[input_len - 1] == '\n' ||
                                     front_p->terminal->view.line[input_len - 1] == '\r'))
                input_len--;
            front_p->terminal->view.line[input_len] = '\0';
            if (input_len >= capacity) input_len = capacity - 1;
            bool copied = copy_to_user_pagedir(xapi_current_pagedir(), str, front_p->terminal->view.line, input_len);
            char terminator = '\0';
            if (copied)
                copied = copy_to_user_pagedir(xapi_current_pagedir(), str + input_len, &terminator, 1);
            memset(front_p->terminal->view.line, 0, sizeof(front_p->terminal->view.line));
            front_p->terminal->view.input_lock = false;
            front_p->terminal->view.wait_for_input = false;
            front_p->terminal->view.input_flags = 0;
            return copied ? 0 : -EFAULT;
        }
        front_p = front_p->parent_task;
    }
#endif
}

char do_xapi_Getch()
{
#if OPENXJ380_INPUT_OUTPUT_DISABLED
    return 0;
#else
    pcb_t front_p = get_current_task()->parent_group;
    while (true)
    {
        if (front_p == NULL || front_p == kernel_group)
        {
            pr_warn("A program call xapi_Getch, but cannot found parent terminal. \n");
            return 0;
        }
        if (front_p->terminal->console_owner)
        {
            while (true)
            {
                int value = x3tp_terminal_getch(&front_p->terminal->view);
                if (value >= 0) return (char)value;
                scheduler_yield();
            }
        }
        front_p = front_p->parent_task;
    }
#endif
}

void do_xapi_Endline()
{
    p_xapi_output_kernel("\n");
}

void do_xapi_Printline(char *str)
{
    char *kstr = NULL;
    if (xapi_copy_string_from_user(&kstr, str, XAPI_USER_STRING_MAX) < 0) return;
    size_t len = p_xapi_strnlen(kstr, XAPI_USER_STRING_MAX - 2);
    kstr[len] = '\n';
    kstr[len + 1] = '\0';
    p_xapi_output_kernel(kstr);
    free(kstr);
}

void do_xapi_OutputSerial(char *str)
{
#if OPENXJ380_INPUT_OUTPUT_DISABLED
    (void)str;
    return;
#else
    char *kstr = NULL;
    if (xapi_copy_string_from_user(&kstr, str, XAPI_USER_STRING_MAX) < 0) return;
    write_serial_string(kstr);
    free(kstr);
#endif
}

void do_xapi_Printf(char *str)
{
    do_xapi_Output(str);
}

void do_xapi_Sleep(uint64_t ms)
{
    uint64_t nano = ms * 1000000ULL;
    if (ms != 0 && nano / 1000000ULL != ms) nano = (uint64_t)-1;
    scheduler_sleep_ns(nano);
}

void do_xapi_GetTimeX(uint64_t tt)
{
    if (!tt) { return; }
    tm local;
    memset(&local, 0, sizeof(local));
    time_read(&local);
    copy_to_user_pagedir(get_current_task()->parent_group->pagedir, (void *)tt, &local, sizeof(local));
}

extern uint64_t memory_total_size;

uint64_t do_xapi_GetMemorySize()
{
    return memory_total_size / 1024;
}

void do_xapi_Run(char *path)
{
    page_directory_t *caller_pagedir = get_current_task()->parent_group->pagedir;
    char *upath = NULL;
    if (xapi_copy_string_from_user(&upath, path, XAPI_USER_PATH_MAX) < 0) return;
    char  *fpath = vfs_cwd_path_build(upath);
    free(upath);
    if (fpath == NULL) return;
    runfile(fpath);
    switch_page_directory(caller_pagedir);
    free(fpath);
}

uint64_t do_xapi_RunArgs(char *path, char **argv)
{
    page_directory_t *caller_pagedir = get_current_task()->parent_group->pagedir;
    char *upath = NULL;
    int ret = xapi_copy_string_from_user(&upath, path, XAPI_USER_PATH_MAX);
    if (ret < 0) return (uint64_t)ret;

    char *fpath = vfs_cwd_path_build(upath);
    free(upath);
    if (fpath == NULL) return (uint64_t)-ENOENT;

    char **kargv = NULL;
    ret = p_xapi_copy_argv_from_user(argv, &kargv);
    if (ret < 0)
    {
        free(fpath);
        return (uint64_t)ret;
    }
    write_serial_fmt("[busybox-debug] do_xapi_RunArgs fpath=%s\n", fpath);
    if (kargv != NULL)
    {
        for (int i = 0; kargv[i] != NULL; i++)
        {
            write_serial_fmt("[busybox-debug] do_xapi_RunArgs argv[%d]=%s\n", i, kargv[i]);
        }
    }

    ret = create_user_process_from_file_as(fpath, get_current_task()->parent_group, kargv, (int)XPCS_INHERIT);
    switch_page_directory(caller_pagedir);
    p_xapi_free_argv(kargv);
    free(fpath);
    return (uint64_t)ret;
}

uint64_t do_xapi_MapMemory(uint64_t ptr, uint64_t size, uint32_t flags)
{
    uint64_t ret = -1;
    if (size == 0 || check_user_overflow(ptr, size)) return ret;
    flags |= PTE_USER;
    if (ptr == NULL)
    {
        ret = page_alloc_random(get_current_task()->parent_group->pagedir, size, flags);
    }
    else
    {
        page_map_range_to_random(get_current_task()->parent_group->pagedir, ptr, size, flags);
        ret = ptr;
    }
    return ret;
}
