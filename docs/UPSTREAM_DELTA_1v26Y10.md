# OpenXJ380 `1v26Y10` → `a1fcc2f` 上游差异分类

> 基线：tag `1v26Y10`（d585f42，2026-10-04）
> 终点：`a1fcc2f`（2026-10-26），即 XACT 2026v5 对应的源码树
> 范围：7 个非 merge 提交，37 个文件
> 原则：内核只收原语；IME/产品策略归 `ime-kmod` 或用户态仓库

## 一、提交清单与归属

| 提交 | 主题 | 归属 | 处理 |
|---|---|---|---|
| `42c99c2` | PTY 回显 + 前台信号修复 | 内核原语 | ✅ 已等价移植（OpenXSKernel `44282fa`，逐文件比对一致） |
| `62ca2e4` | IME v1 内核子系统 + 访问保护 + 可恢复互斥 | 业务 + 原语 | 业务不进内核；原语已用通用钩子等价实现 |
| `68064d7` | IME v2 会话 + 用户词库存储 | 业务（+1 处安全修复） | 业务归 `ime-kmod`；用户名路径校验单独移植 |
| `b33c79e` | 保留 `XAPI_SET_SECRET` (7486) | 内核 ABI | ✅ 随 `a1c44d1` 一并移植 |
| `a1c44d1` | 进程生命周期截屏保护 | 内核原语/安全 | ✅ 已移植（`.cpp/.h` + pcb 钩子 + syscall + mmap 修复） |
| `45a5d26` | IME 单点 Shift 检测 | 业务 | ❌ 不进内核 → `ime-kmod` |
| `a1fcc2f` | SDK 暴露 XTTTP 终端原语 | SDK | ❌ 不进内核；XACT v5 已包含，`ime_utf8.h` 内核已有 |

## 二、逐项分类

### 内核原语（应进 OpenXSKernel）

1. **PTY 回显/前台信号**（`42c99c2`）
   `pty.cpp`、`pty.h`、`tty.h`、`signal.h`、`signal.cpp`、`handler.S`、`pcb.cpp` 与上游逐字节一致；
   `sys.cpp` 的两处 `ssize_t` 错误透传与 `sigexit` 已在本仓库对应位置存在。

2. **进程级截屏保护**（`a1c44d1`）
   - 新增 `include/screen_capture.h`、`kernel/screen_capture.cpp`
   - `pcb` 增加 `screen_secret` / `screen_raw_mapped` / `screen_capture_next`
   - `pcb.cpp`：退出、失败清理、fork 继承三处钩子
   - `syscall.cpp`：`XAPI_SET_SECRET(7486)` 接线（`b33c79e` 的占位被替换为真实实现）
   - `sys.cpp`：`vfs_map()` 失败返回 0 时回收 VMA 并返回 `EACCES`/`ENOMEM`
   - 该组只提供机制与导出符号，具体产品策略（谁调用 `ScreenCaptureAccess`）在图形/产品层

3. **用户名路径穿越校验**（`68064d7` 中 `kernel/user/runfile.cpp`）
   `init_user_dirs()` 拒绝含 `/`、`\`、`.`、`..` 的账户名，防止 `/users/<name>` 越界。
   同一 hunk 里的 `ime_config_depth` 属于 IME 业务，未移植。

### 业务（不进内核）

| 上游文件 | 内容 | 去向 |
|---|---|---|
| `kernel/input_method.cpp` (812) | IME 状态机/焦点/事件队列/投递 | `ime-kmod` |
| `kernel/ime_config.cpp` (550) | IME 配置 registry | `ime-kmod` |
| `kernel/ime_user_store.cpp` (342) | 每用户词库存储 | `ime-kmod` |
| `include/input_method.h` / `include/xime.h` / `kernel/ime_internal.h` | IME ABI 与内部结构 | `ime-kmod` |
| `pcb`/`tcb` 的 `ime_role`、`ime_launch_generation`、`ime_storage_depth`、`ime_config_depth`、`ime_launch_role` | 进程内 IME 身份/信任作用域 | `ime-kmod` 侧表 + 静态 pid（见 `AGENTS.md`） |
| `x3tp.h` 的 `getch_*` 环形缓冲 | 挂在终端上的 getch 队列 | 内核已有等价原语 `input_sink`（SPSC） |
| `message.cpp` 的 `ime_text_sink` / `ime_filter_message` | 固定函数式 IME 过滤 | 内核已有通用拦截器 `input_route_set_interceptor` |
| `user.cpp` 的 `ime_process_loaded()` | IME 受控拉起钩子 | 需要时再做通用进程启动钩子 |
| `XAPI_IME_* 7474-7485` | IME syscall 面 | kmod 私有常量；内核 default 分支已转交 kmod 钩子 |
| `45a5d26` Shift 检测 | IME 行为 | `ime-kmod` |

### 已存在的通用等价实现（无需重复移植）

上游在 IME 提交里把访问控制硬编码成 `ime_vfs_access()`，本仓库已泛化为：

- `vfs_access_hook_t` / `vfs_resolved_name_hook_t`（`include/fs/vfs/vfs.h`）
- `vfs_access_allowed()` / `vfs_notify_resolved_name()`（`driver/fs/vfs/vfs.cpp`）
- 调用点与上游一一对齐：`mkdir/link/symlink/mkfile/delete/rename/resize/open/open_no_follow/map/mount/read/write/write_fast/ioctl/unmount`，以及 `sys_rename/renameat/pivot_root`
- `vfs_node.ime_protection` 字段保留为“内核态对象策略标记”
- 可恢复互斥 `mutex_lock_recoverable()` / `mutex_abort_for_process()`（`6876001` 已移植）
- `ime_utf8_scalar_bytes()`（`include/ime_utf8.h`）已存在且与上游一致

## 三、未处理项与备注

- `XAPI_GETLINE (7418)`：XACT v5 的 `xapi_Getline()` 走该号，本内核尚未接线；当前组件均直接
  调用 `XAPI_INPUT`（带 capacity/flags），暂不受影响。
- `XAPI_QUERY_FILE_ACCESS` 等 7496-7502：XACT v5 运行时在 `open/exec/xapi_OpenFile` 前调用，
  本内核未实现时会返回 `-ENOSYS` 并被运行时视为“拒绝”。迁移 SDK 前必须处理
  （内核放行 stub 或 userspace 兼容符号）。
- `XAPI_CAPTURE_SCREEN (7473)`、`XAPI_GET_SYSTEM_STATS (7472)`：常量已在，实现仍随 GUI 移除而缺失。

---

# 第二部分：`a1fcc2f` → `origin/main`（2026-11-12）

> 范围：`a1fcc2f..da07c5f`，32 个非 merge 提交，284 个文件，+47312/-4209
> 这一批是 XACT 2026v5 之后的上游演进；`depend/` 里的对象仍停在 v5 时间点。

## 4. 总览：按特性域归类

| 特性域 | 提交 | 规模 | 性质 | 建议 |
|---|---|---|---|---|
| 权限/凭证模型 | `8ee255a` `4d17634` `3801920` `36f8fee` `f5f2980` `33c7c11` `3094948` `083f2e3` `540578d` `d8fedf9` | ~2.5k 内核 | **原语 + 产品策略混杂** | 先做契约决策（见 §6） |
| VFS/文件系统 | `0a0a02b` `b6757cf` `f5c0526` `540578d` | ~27k（ext4 为主） | 原语 | ext4 暂缓；`f5c0526`/`540578d` 可选 |
| 调度/中断/管道 | `848d049` `cc43bf9` | ~3.5k | 原语（核心修复） | **优先**，需手工合并 |
| 音频驱动 | `e279d78` `169504e` `1033946` `848d049`(部分) | ~1k | 驱动原语（产品功能） | 暂缓，Leopard 无音频 |
| 账户认证 | `1147765` | ~1.3k | 安全原语 + 账户格式 | 需要再定账户体系 |
| 网络 | `17cb845` `2b42bcc` `7a78242` `cc43bf9`(xnet 部分) | ~2.3k | 内核服务原语 | 暂缓，Leopard 无网络 |
| IME | `8cfd887` `d2d7297` | ~1k | **业务** | → `ime-kmod` |
| 动态运行时 | `dc03483` `a08467d` `6e299c7` | ~3k 内核 + 用户态 | 加载器原语 + SDK | 暂缓（静态链接不需要） |
| JSON/XML 命名 | `892d572` | 216 | SDK/API | 随 SDK 同步 |
| 镜像/CI/工具 | `39c6f05` `342acd9` `bcaa92b` `b85751b` | ~570 | 构建工具 | 不进内核 |

## 5. 重点域拆解

### 5.1 权限/凭证模型（XACT v5 的前置依赖）

这是本批最重要的内核原语，也是 XACT v5 运行时 `xj380_auto_file_access()` 所依赖的
`XAPI_QUERY_FILE_ACCESS(7500)` 的实现来源。

**内核机制（应进 OpenXSKernel）**

- `pcb` 增加 `permission_user`（进程自有身份，不再别名登录账户）、`permission_revision`
- `include/user/permission_types.h`：`UserPermisson` + 7 位掩码 `XPERM_*`（`8ee255a` 增 `VisitAnotherDir`）
- `kernel/user/permissions.cpp`：凭证派生、`lift/request`、同意队列 FIFO、`permissions_file_access()`
- `kernel/syscall/xapi/xpermissions.cpp` + `syscall.cpp`：7496-7502
- `kernel/user/special_permissions.cpp`：7502 的按应用/可执行文件授权缓存
- `include/user/file_access.h`、`include/user/process_launch.h`
- VFS 按**解析后的路径**授权：`vfs_check_access()` / `permissions_file_access()`（`3801920`/`f5f2980`）
- `540578d`：设置写入成功后的通知钩子（可与既有 `vfs_access_hook` 并列）

**夹带在里面的产品策略（按红线应外移）**

- `XUT_Visitor` 默认权限、`XUT_Admin` 等级表
- 公开前缀 `/system/resources`、`/etc` 免授权读
- `settings.json` 属主例外、IME 锚点/私有目录、`permission.dat` 存储格式
- 安装器/应用目录布局（`/apps/.../protected`、`private`）

这些策略目前写死在 `kernel/user/permissions.cpp`。两条路：

- **A. 原样移植**：最快，和上游/XACT 完全一致；代价是承认“产品策略进内核”的债务。
- **B. 机制/策略拆分**：内核只保留凭证 + 掩码 + 同意队列 + `permissions_file_access` 钩子，
  等级表与公开前缀放到用户态 `permissions.conf`，经现有 config 化原则注入；工作量更大。

### 5.2 调度/管道/消息（核心修复）

`848d049`（2026-11-04）改动了 `pcb.cpp`(475/437)、`scheduler.cpp`、`message.cpp`(247/33)、
`pipefs.cpp`、`serial_port.cpp`、`apic.cpp`、`page.cpp`、`smp.cpp`、`fd_lifetime.cpp` 等，
主题是“音频场景下的运行时卡顿”：IRQ/FPU/嵌套抢占状态、进程准备移出长临界区、
EEVDF 运行时间记账、APIC 定时器校准、串口有界 worker、可抢占的文件/IME I/O。

`cc43bf9` 的 FPU 部分（`handler.S` 在异常/syscall/定时器入口 `fxsave64/fxrstor64`，
`scheduler.cpp` 改为 `timer_handle_fpu`）是独立、明确的正确性修复。

**冲突点**：本仓库的 `kernel/syscall/message.cpp` 已改成 `input_sink` + `input_route_dispatch`
架构，与上游 `do_message_unfiltered` + `ime_text_sink` 版本不同；`pcb.cpp`/`scheduler.cpp`
也有本地修改。必须手工合并，不能整文件覆盖。

### 5.3 ext4 与原生安装（大件，暂缓）

`0a0a02b` 新增 `driver/fs/ext4/`（约 26k 行，Apache-2.0，首个原生 ext4 读写/JBD2/xattr/格式化）
以及 `kernel/installer/*`（约 3.5k 行）。需要区分：

- `driver/fs/ext4/**`、`driver/block_access.*`、`driver/block_source.*`、AHCI/NVMe flush：
  **文件系统/块设备原语**
- `kernel/installer/**`、`include/installer/**`、`include/efi/installer_handoff.h`：
  **安装器业务流程**（Leopard 有自己的 `leopard-installer`，不应复制）

Leopard 当前是 FAT32 + xbps，ext4 属于“以后要做原生根分区”时才需要。

### 5.4 账户认证

`1147765` 把明文 `usereg.dat` 换成 PBKDF2-HMAC-SHA256 版本化数据库 + RSA-OAEP 挑战，
并移除旧明文 syscall。属于安全原语，但直接决定 oobe/getty 的登录链路与
`xapi_UserCreateFirst`/`xapi_UserLogin` 语义；需要和 Leopard 的 `/system/config/usereg.dat`
方案一起决策。

### 5.5 IME（业务，确认不进内核）

- `8cfd887`：拼音偏好隔离、私有存储迁移
- `d2d7297`：新增 `include/input_method_platform.h`，把产品 ID/路径/JSON/偏好语义从内核
  抽成“平台契约”，保留通用会话路由/能力检查/VFS 保护/不透明存储

方向与本仓库一致（机制/策略分离），但实现仍在内核态；按 `AGENTS.md` 的 kmod 方案，
这部分归 `ime-kmod`，内核只需已有的 `input_sink`/拦截器/VFS 钩子。

### 5.6 动态运行时

- `dc03483`：用户态 ELF 动态链接器（PT_INTERP、重定位、构造函数、RELRO）
- `a08467d`：`libxapi.so.1` + 动态 CRT
- `6e299c7`：EPF 格式内核加载、共享库解释、TLS、C++ thread_local 析构

内核侧新增 `kernel/user/epf.cpp`(863)、`include/epf*.h`、TLS 相关 pcb/prep 逻辑。
Leopard 目前全部静态 ELF，暂不需要；若将来要用 XACT 的动态 SDK 再评估。

## 6. 建议的落地顺序

1. **P0（解锁 XACT v5）**：权限 syscall 面 7496-7502 + `UserPermisson` 掩码。
   先选 §5.1 的 A（原样）或 B（机制/策略拆分）；若只想立刻跑通 v5，可先做
   “查询=0（放行）、提升/请求=0”的最小原语 stub，之后再替换为完整模型。
2. **P1（正确性）**：`cc43bf9` 的 FPU 保存/恢复；单独可验证。
3. **P2（稳定性）**：`848d049` 的调度/管道/串口修复，手工合并 `message.cpp`/`pcb.cpp`/`scheduler.cpp`。
4. **P3（安全）**：`1147765` 账户认证，和 oobe/getty 一起改。
5. **暂缓**：ext4/installer、音频、xnet、EPF/rtld、IME 业务、CI/镜像工具。

## 7. 与当前 OpenXSKernel 的已知冲突面

| 文件 | 上游改动 | 本地现状 | 处理 |
|---|---|---|---|
| `kernel/syscall/message.cpp` | IME text sink + 上游 getch 队列 | `input_sink` + `input_route_dispatch` + SPSC | 手工合并，保留本地架构 |
| `kernel/task/scheduler.cpp` | FPU + EEVDF + APIC 校准 | 本地已有调度修改 | 逐 hunk 合并 |
| `kernel/task/pcb.cpp` | 凭证/EPF/线程准备 | 本地已有 screen_capture/进程修复 | 逐 hunk 合并 |
| `driver/fs/vfs/vfs.cpp` | `permissions_file_access` 全面替换 `ime_vfs_access` | 本地 `vfs_access_allowed` + 钩子 | 设计对齐后再改 |
| `kernel/user/user.cpp` | 凭证/认证重构 | 本地 root/oobe 简化模型 | 随 P0/P3 一起改 |
| `include/syscall/pxapi.h` | 7496-7502 等 | 本地只有到 7486 | 按 P0 增补 |

---

# 第三部分：`da07c5f` → `93d76a7`（2026-11-13）

> 范围：7 个非 merge 提交，主题几乎全是安装器原生化（FAT32/ext4 双路径）。
> 结论：只移植 3 处内核原语修复，其余归安装器/构建工具。

## 8. 分类

| 提交 | 主题 | 归属 | 处理 |
|---|---|---|---|
| `716b054` | 读取可执行镜像时保留 VFS 错误 | **内核原语** | ✅ 已移植（配合 `permissions_file_error()`） |
| `370dea6` | pipefs 挂载根保持目录语义 | **内核原语** | ✅ 已移植（本树无 `vfs_close_cached_backend`，改为直接复位类型/大小） |
| `214d373`（部分） | `kernel/cpu/common.cpp` 给 `insw` 加 `"memory"` clobber | **内核原语** | ✅ 已移植（仅上游改的 `insw`；`outsw` 同类问题未动，遵守“不改上游 bug”） |
| `76f1c06` | 安装器支持 VMware 介质与大 ext4 包 | 安装器业务 + IDE/AHCI/block 驱动 | ❌ 暂缓（QEMU/AHCI 路径不需要） |
| `214d373` / `695bdee` | FAT32/ext4 有界安装、原生命令限制为 ext4 | 安装器业务 | ❌ 不移植 |
| `214d373` | `driver/fs/fatfs`、`driver/fs/ext4`、`driver/{ahci,ide}/` | 绑定根校验/安装 I/O | ❌ 暂缓（属原生安装路径） |
| `b5bf6f6` | 默认镜像打包改为 ext4 + FAT32 ESP | 构建工具 | ❌ 不移植（Leopard 仍 FAT32 + xbps） |
| `15860ea` | 修复 CI 写死偏移 | CI 工具 | ❌ 不移植 |

## 9. 本次移植的细节

1. **VFS 错误透传**（`716b054`）
   - `tcb` 增加 `file_access_error`；
   - `permissions_file_access()` 入口清零、拒绝时置 `EACCES`；
   - `read_vfs_file()` 读不完整时返回 `-permissions_file_error()`（否则 `-EIO`），并打印 inode/size/fsid 诊断。

2. **pipefs 挂载根目录语义**（`370dea6`）
   - `pipefs_open()`：根节点为 `file_dir`，子节点才是 `file_pipe`；
   - `pipefs_mount()`：复位 `type=file_dir`、`size=0`；
   - `pipefs_stat()`：补 NULL 守卫并特判根节点。

3. **`insw` 编译器屏障**（`214d373`）
   - `asm volatile("cld; rep; insw" ... : "memory")`，防止编译器把内存访问重排到端口 I/O 两侧。

---

# 第四部分：`93d76a7` → `2422496`（2026-11-13 晚）

> 范围：3 个非 merge 提交（`26d8676`、`ce9352f`、`69827b4`）。
> 结论：2 处内核修复已移植；语言 ID 与安装器身份生成归用户态。

## 10. 分类

| 提交 | 主题 | 归属 | 处理 |
|---|---|---|---|
| `69827b4` | 启动交接前先发布 IRQ 策略 | **内核原语** | ✅ 已移植（`main.cpp` 中 `no_interrupt=false` 提前到 `scheduler_start()` 之前） |
| `ce9352f`（部分） | CPUID 最大叶号保护 `rdseed/rdrand` | **内核原语** | ✅ 已移植（`rng.cpp` 的 `try_rdseed64`/`try_rdrand64`，叶 1 前检查 `maximum_leaf >= 1`） |
| `ce9352f`（其余） | `get_identifier_random_bytes()` 软件兜底、安装器身份生成 | 安装器业务 | ❌ 不移植（Leopard 安装器在用户态，且有独立随机源） |
| `26d8676` | 7 种语言 ID、settings 归一化、profile 种子 | 用户态/产品策略 | ❌ 暂不移植（内核只保留 zh/en 常量；等 OOBE 多语言需求） |
| `26d8676`（工具） | `native_rtld.py` 允许产品追加源/依赖 | 构建工具 | ❌ 不移植（Leopard SDK 独立构建） |

## 11. 备注

- 本批上游继续把安装器下沉到 `kernel/installer` 与 `boot/bootx64.c`，与 Leopard 的
  “内核只留原语、安装器在用户态” 拆分相反，后续同类提交默认按安装器业务过滤。
- `ce9352f` 的 `get_hardware_random_bytes()` 严格硬件熵接口未引入：本树
  `get_random_bytes()` 已是“硬件优先 + ChaCha20 软件池”，安装器身份码由用户态生成。
