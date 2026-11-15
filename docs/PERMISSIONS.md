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

VFS 原语：`vfs_check_access()` 统一清错误、调用 hook、保留 `EACCES`；
`vfs_check_child_creation()` 用解析后的 parent+name 生成 canonical path 再检查。
`vfs_access_op_t` 的 0..8 与 OpenXJ380 `ImeFileOperation` 对齐：
READ/WRITE/REMOVE/MOUNT/LOOKUP/LIST/STAT/READ_WRITE/EXPORT；XSK 追加 `9 = EXEC`。

内核不出现具体产品路径或等级默认值。策略加载是 fail-closed 的：
`permissions.conf` 缺失、不可读、为空、被截断或任一行解析失败时，策略不会被标记为已加载，
内核在启动阶段直接拒绝继续（见第 1.1 节），运行期应用访问也会被拒绝。

### 1.1 加载与失败处理

`permissions_load_policy()` 返回 `bool`，只有在文件被完整读取且全部内容解析有效时才置
`policy_loaded=true`；`permissions_init()` 返回同样的结果，且无论加载成败都会安装
VFS 访问钩子。`kernel/main.cpp` 在 `permissions_init("/system/config/permissions.conf")`
返回 false 时打印 `BOOT: FATAL: permissions policy unavailable` 并 `cli; hlt`，不会进入 PID1。

以下情况一律视为加载失败（不再“禁用模型并放行”）：

- 文件不存在、不可读或为空；
- `vfs_read()` 返回 `sizeof(buffer)-1`：可能被截断，按过大/无效处理；
- `launch.pre_login` / `launch.visitor` 的值不是 `Root/System/Admin/Visitor/Custom`；
- `grade.*` 的掩码含未知标记；
- 路径值超过 `PERMISSION_PATH_MAX`（不再静默截断）；
- `public_read` / `protected_write` 前缀表已满（不再静默丢弃）；
- 缺少任一标准等级（`grade.Root`、`grade.System`、`grade.Admin`、`grade.Visitor`）。

运行期若策略未加载：内核/启动任务（`task_level != TASK_APPLICATION_LEVEL`）仍走放行路径，
应用任务则得到空掩码并在 `permissions_file_access()` / `permissions_query_file()` 中全部拒绝。

### 1.2 文件级 DAC（XUT 版）

权限原语是 inode 的 `owner/group/mode`，而不是产品路径表：

- `vfs_node_mode()` / `vfs_node_set_mode()` 是 `mode` 与 `permissions` 的唯一入口，
  两份字段不再各自漂移；
- 身份来自 `pcb->permission_user`（XUT），映射为稳定数值 id：
  `Root=0`、`System=1`、`Admin=2`、`Visitor=65534`、`Custom=10000+hash%50000`；
  镜像里旧构建用户 `1000` 视为 `Root`；
- 判定顺序：`Root` 放行读写（普通文件执行仍要求任一 x 位）→ owner 位 →
  group 位 → other 位；`XUT_Custom` 的 owner/group 还必须 id 相等，
  否则所有 Custom 用户会共享归属权限；
- 目录 `r` = 可列出、`x` = 可穿越、`w` = 可增删子项；
  创建/删除/rename 检查的是父目录 `w|x`，不是子文件自身；
- `chmod` 仅 owner 或 Root；非 Root 设置的 setuid/setgid 会被清掉；
  `chown` 仅 Root，非 Root 只能把自己的文件换到自己的组，改属主会清 setuid/setgid；
- `exec` 在 `create_user_process_from_file_as()` 中单独检查 `VFS_ACCESS_EXEC`，
  可读但无 x 位的文件不能执行；`mount` 等价于 `CAP_SYS_ADMIN`，只允许 Root/System，
  Admin 不能把自制文件系统叠到 `/system` 上；
- `execve` 对 ELF 应用 `S_ISUID`：`process_execve()` 读取文件 inode 的 owner，
  用 `user_identity_from_uid()` 反推 XUT 并切换 `pcb->permission_user`；
  脚本（`#!`）不继承 setuid，`S_ISGID` 不单独处理（XSK 的 gid 恒等于 uid）；
- `setuid/setgid/setresuid/setresgid` 的语义不是维护三套 id，而是把进程身份切到
  uid 对应的 XUT：非 Root 只能“切到自己”，Root 可切到任意已知 uid；
  `getuid/geteuid` 都返回当前身份的 `user_uid()`；
- 新节点默认 `0777/0666 & ~umask`，`umask` 默认 `022`；owner = 当前身份，
  group = 父目录 setgid 或当前身份；ext4 把这些值写进持久 inode，
  tmpfs 存在 entry 里，FAT 统一为 `root:root 0755/0644`；
- `/tmp` 由 tmpfs 挂载为 `root:root 01777`；`/dev` 块设备 `0600`、流设备 `0666`。

过渡期 `permissions_file_access()` 采用 **DAC 与路径策略同时允许**：路径规则继续提供
`/users` 家目录隔离，直到镜像/包安装的 owner/mode 全部就绪；后续可把路径策略退化为
可选的 ACL/LSM 层。

文件系统驱动的块设备 I/O 走 `vfs_read_raw()` / `vfs_write_raw()` / `vfs_ioctl_raw()`：
挂载时已完成 `VFS_ACCESS_MOUNT` 授权，后续按调用者 DAC 去检查底层块设备会把普通用户
写自己文件的操作一并拒绝。用户态仍只能通过 `file_block` 节点 + `XPERM_CS` 规则访问裸盘。

## 2. 配置格式

```ini
grade.Root=all
grade.System=all
grade.Admin=all
grade.Visitor=R,W
launch.pre_login=Visitor
launch.visitor=Visitor
sys_dir=/system
settings_dir=/config
users_dir=/users
public_read=/etc
protected_write=/system/config
```

- `grade.*`：`all`、`none` 或 `R,W,CS,NV,SV,SR,SW` 的组合；四个标准等级都必须出现。
- `launch.pre_login`：登录会话建立前，内核拉起进程使用的等级；无已认证身份时回退为
  `Login`/Visitor，配置成 Root/System/Admin 会被 clamp 为 Visitor。
- `launch.visitor`：访客身份启动使用的等级；同样不允许 Root/System/Admin，避免
  “访客 fallback”变成提权。
- `sys_dir`：其下读需 `SR`、写需 `SW`。
- `settings_dir` / `protected_write`：写/删需 `CS`。
- `users_dir`：跨用户访问需 `NV`；自己的家目录按普通 `R/W`。
- `public_read`：任意应用可读（仍受写权限约束）。
- 未知键被忽略（向前兼容）；已知键的非法值会让整份策略加载失败。

### 2.1 裸块设备

`permissions_file_access()` 对应用任务（`task_level == TASK_APPLICATION_LEVEL`）检查
`node->type & file_block`：没有 `XPERM_CS` 时拒绝除 `VFS_ACCESS_LOOKUP` 外的所有操作，
并在 `file_access_error` 写入 `EACCES`。设备名不参与判断，因此任意新增块设备自动受保护。
`permissions_query_file()` 只有路径、没有节点类型，无法做同等判定；此类路径仅按
`permissions.conf` 的路径策略返回缺失位，真正的块设备限制由 VFS 钩子（带 node）执行。

## 3. 进程凭证

- `pcb->permission_user` 是进程自有身份，不再别名登录账户；`permission_revision`
  在 exec 时递增，用来作废旧的同意请求。
- 启动来源（`ProcessCredentialSource`）：
  - `XPCS_LOGIN`：跟随当前登录（`fork`/`exec` 之外的核内拉起）。
  - `XPCS_INHERIT`：`fork` 复制父进程凭证（默认）。
  - `XPCS_VISITOR`：访客等级（为后续 XAPI_Run 语义预留）。
- PID1 由内核在 `user_session_use_root()` 之后以 **Root** 拉起；`leopard-rc`/`getty`/`mountd`
  通过 fork/exec 继承 Root。`XAPI_USER_LOGIN` 成功后，getty 会在当前进程上切换到账户身份，
  再 exec conhost/shell；后续 `XAPI_Run/RunArgs/runfile` 使用 `XPCS_INHERIT`，不会重新拿 login/Admin。
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
- 上游策略加载失败时保持“模型禁用/放行”；本实现改为启动期拒绝继续（`cli; hlt`），
  运行期应用 fail-closed，内核/启动任务仍放行。
- 裸块设备按 VFS 节点类型限制（`file_block` + `XPERM_CS`），不依赖设备名或 `/dev/`
  路径字符串。
- 特殊权限（相册）存储、文件选择器、launch identity 的完整策略尚未移植；
  7502 暂返回 0，`XAPI_SPAWN_INHERITED` 暂未接线。
- 待办：把 `XAPI_Run/RunArgs` 切到 `XPCS_VISITOR`（需要先有同意后端，否则会
  阻断 initctl 等需要写 `/system/config` 的工具）。
