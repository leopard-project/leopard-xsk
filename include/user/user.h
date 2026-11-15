#pragma once

#include <stdint.h>

#include "./settings.h"
#include "permission_types.h" // ABI: UserType / UserPermisson / XPERM_*

typedef struct
{
    char            name[64];   // 用户名
    UserType        user_type;  // 用户等级
    UserPermisson   user_prms;  // 用户权限（仅等级为Custom时可用）
    char          **envp;       // 环境变量指针
    size_t          envc;       // 环境变量数量
    int             fgproc;     // 前台进程
    char            password[64];   // 用户密码
} UserInfo;

typedef struct
{
    int         user_count; // 用户数量
    UserInfo    uinf[128];  // 用户信息
} UserRegisterList;

void init_user();
void copy_args(char *dst[], char *src[], int n);
uint32_t user_uid(const UserInfo *user);
uint32_t user_gid(const UserInfo *user);
UserInfo *task_effective_user();
bool user_session_needs_oobe();
int user_session_list(UserInfo *out, int max_count);
/* 按名字查注册表。供 home 目录归属/持久化使用；找不到返回 false。 */
bool user_lookup_name(const char *name, UserInfo *out);
/* uid → XUT 等级是固定映射；身份结构由 uid 推导，不依赖登录会话。 */
UserType user_type_from_uid(uint32_t uid);
bool user_identity_from_uid(uint32_t uid, UserInfo *out);
int user_session_login(const char *username, const char *password);
int user_session_check_password(const char *username, const char *password);
int user_session_create_first(const char *username, const char *password);
void user_session_use_root();
void user_session_use_login();
bool user_session_is_ready();
void user_session_set_ready(bool ready);

extern UserInfo  root_user;
extern UserInfo *current_user;
