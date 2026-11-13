# 权限模型：内核机制 / 用户态策略

> 目标：内核只保留凭证、掩码、同意队列与 VFS 决策钩子；等级表、公共/保护路径
> 等产品策略全部来自启动期配置 `/system/config/permissions.conf`。

## 1. 分层

| 层 | 位置 | 内容 |
|---|---|---|
| ABI | `include/user/permission_types.h`、`include/user/file_access.h` | `UserType`、`UserPermisson`、`XPERM_*`、`XFA_*`；只定义布局，不含语义 |
| 机制 | `kernel/user/permissions.cpp` | 进程凭证、掩码运算、同意队列、路径分类算法、VFS 钩子 |
| 策略 | `/system/config/permissions.conf` | 等级基础权限、登录前/访客等级、公共读前缀、系统/设置/用户目录、保护写前缀 |
| 集成 | `driver/fs/vfs/vfs.cpp` | 通用访问钩子链；任一钩子拒绝即拒绝 |

内核不出现具体产品路径或等级默认值；`permissions.conf` 缺失时策略禁用（放行），
行为与旧内核一致。

## 2. 配置格式

```ini
grade.Root=all
grade.System=all
grade.Admin=all
grade.Visitor=R,W
launch.pre_login=Admin
launch.visitor=Visitor
sys_dir=/system
settings_dir=/config
users_dir=/users
public_read=/etc
protected_write=/system/config
```

- `grade.*`：`all`、`none` 或 `R,W,CS,NV,SV,SR,SW` 的组合。
- `launch.pre_login`：登录会话建立前，内核拉起进程使用的等级。
- `launch.visitor`：访客身份启动使用的等级。
- `sys_dir`：其下读需 `SR`、写需 `SW`。
- `settings_dir` / `protected_write`：写/删需 `CS`。
- `users_dir`：跨用户访问需 `NV`；自己的家目录按普通 `R/W`。
- `public_read`：任意应用可读（仍受写权限约束）。

## 3. 进程凭证

- `pcb->permission_user` 是进程自有身份，不再别名登录账户；`permission_revision`
  在 exec 时递增，用来作废旧的同意请求。
- 启动来源（`ProcessCredentialSource`）：
  - `XPCS_LOGIN`：跟随当前登录（`fork`/`exec` 之外的核内拉起）。
  - `XPCS_INHERIT`：`fork` 复制父进程凭证（默认）。
  - `XPCS_VISITOR`：访客等级（为后续 XAPI_Run 语义预留）。
- 线程的 `user_info` 统一指向所属 pcb 的 `permission_user`。

## 4. Syscall 面（XACT 2026v5）

| 号 | 名称 | 行为 |
|---|---|---|
| 7486 | `XAPI_SET_SECRET` | 进程级截屏保护（独立子系统） |
| 7496 | `XAPI_LIFT_PERMISSIONS` | 请求等级掩码；无同意后端时返回缺失位掩码 |
| 7497 | `XAPI_GET_CURRENT_PERMISSIONS` | 复制当前 `UserPermisson` |
| 7498 | `XAPI_REQUEST_PERMISSIONS` | 请求指定能力位 |
| 7499 | `XAPI_SPAWN_INHERITED` | 预留，暂 `-ENOSYS` |
| 7500 | `XAPI_QUERY_FILE_ACCESS` | 只判定不落盘；0=允许，正数=缺失位，负数=错误 |
| 7501 | `XAPI_REQUEST_FILE_ACCESS` | 同上，必要时进入同意队列 |
| 7502 | `XAPI_REQUEST_SPECIAL_PERMISSION` | 暂无特殊权限存储，返回 0 |

同意队列（`permissions_prompt/answer/set_backend`）已实现但当前无前端后端；
`permissions_set_backend(false)` 时请求立即返回缺失位，不阻塞。

## 5. 与上游的差异

- 上游把等级表、公共前缀、`settings.json`、`permission.dat` 等写死在
  `kernel/user/permissions.cpp`；本实现全部移到配置。
- 特殊权限（相册）存储、文件选择器、launch identity 的完整策略尚未移植；
  7502 暂返回 0，`XAPI_SPAWN_INHERITED` 暂未接线。
- 待办：把 `XAPI_Run/RunArgs` 切到 `XPCS_VISITOR`（需要先有同意后端，否则会
  阻断 initctl 等需要写 `/system/config` 的工具）。
