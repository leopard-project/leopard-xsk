# X3TP — 内核输入/终端标准

> 状态：**已实现（2026-11-15）**。取代本文早先的 `input_route_dispatch` + 焦点表 +
> router 线程方案（保留在文末“历史方案”仅作决策记录）。
>
> 代码：`include/x3tp.h`、`kernel/x3tp.cpp`、
> `driver/fs/vfs/dev.cpp`（`/dev/console`）、`driver/ps2/keyboard.cpp`、
> `driver/input/vkbd.cpp`。

## 目标与红线

- 中断上下文直接路由，**不跑 router 内核线程**。
- 数据路径**不加 mutex/spinlock**；per-source 单生产者，队列满即丢键。
- mono-session 优先；multiseat 只加场景表项，IRQ 路径不变。
- 内核只做“事件搬运”：不做 IME、候选词、词库、终端渲染。
- 不导出 `x3tp_route_*` / `x3tp_terminal_*`，模块不可见。

## 数据流

```text
PS/2 IRQ ─┐
USB worker ─┼─> x3tp_route_irq(source, event) ─> source ring (SPSC)
VKBD write ─┘        │
                     ├─ g_input_routes[source] = { seat0, endpoint0 }   （静态表）
                     ├─ 写 endpoint 消费的 source ring
                     ├─ sequence = fetch_add(endpoint->sequence)
                     └─ notify 0 -> 1 时调用 endpoint->wake()

/dev/console（seat0/endpoint0）← conhost read()/poll()
```

`x3tp_route_irq()` 只做查表 + 写环 + 发布 head + 一次唤醒 latch，没有分配、
没有调度器唤醒、没有焦点查找。

## 数据结构

`include/x3tp.h`：

| 类型 | 作用 |
|---|---|
| `x3tp_event_t` | 24 字节读取 ABI 记录（`static_assert` 锁死） |
| `input_source_t` | 每 source 一条 SPSC 环（256 事件），head/tail 分 cache line，`dropped` 计数 |
| `input_seat_t` | `active`、`endpoint`、`source_mask` |
| `input_endpoint_t` | `active`/`closed`、`notify` 唤醒 latch、`sequence`、`wake` hook |
| `input_route_t` | `source -> {seat, endpoint}` 启动期常量表 |

静态池：`INPUT_SEAT_MAX = 8`、`INPUT_ENDPOINT_MAX = 8`、
`INPUT_SOURCE_PS2/USB/VKBD`。运行期不分配、不释放，更不会在 IRQ 里释放。

## `/dev/console` ABI

- `DEVICE_STREAM`，只有 read/poll；write 返回 `-ENOSYS`，ioctl 返回 `-ENOTTY`。
- `read()`：返回若干完整 24 字节记录；seat0 无数据返回 `-EAGAIN`；
  缓冲区 `< 24` 返回 `-EINVAL`（消费者必须读 24 的整数倍）。
- `poll()`：seat0 任一 ring 非空 → `POLLIN`（level-triggered）；endpoint closed →
  `EPOLLHUP`。
- 权限 `root:Admin 0660`：conhost 属 Admin 可读，Visitor 拒绝。
- 记录布局：

```c
typedef struct input_event {
    uint32_t type;      /* 1 CHAR, 2 KEYDOWN, 3 KEYUP */
    uint32_t value;     /* CHAR: ime_utf8_pack 的 UTF-8；KEY: KEY_* */
    uint32_t flags;     /* INPUT_FLAG_* 修饰键位 */
    uint32_t reserved;  /* 0 */
    uint64_t sequence;  /* 每 endpoint 单调递增 */
} x3tp_event_t;
```

事件语义：

- `CHAR` 是“可提交文本/控制字节”，只表达按下结果，不配对 key-up。
- `KEYDOWN`/`KEYUP` 只用于有稳定键码的 `KEY_*`（修饰键、方向键、功能键等），
  由消费者映射成 ANSI 序列。
- 没有 write/ioctl：commit text 由 conhost 写 PTY，不从输入端点回流。

## source 接入约定

- **一 source 一生产者**。PS/2 中断、单个 xHCI worker、vtty 写端各自独占自己的
  ring；多控制器/多 worker 需要拆 source 或外部串行化。
- PS/2：`<128` 的按下字节发 `CHAR`；`>=128` 的 `KEY_*` 发 `KEYDOWN/KEYUP`。
- USB：同样 CHAR/KEY 拆分，source 为 `INPUT_SOURCE_USB`。
- `/dev/vkbd`：每个 UTF-8 标量一条 `CHAR`；写入端加锁保证单生产者。
- 队列满：丢弃并累加 `dropped`，不阻塞中断。

## 多 seat 扩展

当前只有 `seat0 -> endpoint0 -> /dev/console`。扩展方式是启动/场景建立时：

```c
x3tp_route_bind(INPUT_SOURCE_PS2, seat_id, endpoint_id);
x3tp_route_bind(INPUT_SOURCE_USB, seat_id, endpoint_id);
```

`x3tp_route_irq()` 不改。seat/endpoint 结构是静态池，`x3tp_endpoint_open/close`
只切标志；未来每个座位一个 `/dev/console`（或 seat 文件）即可。
真正的 seat manager / 热插拔策略属于用户态，内核不实现。

## 与 legacy 路径的关系

- 旧 XTTTP/XAPI（`xapi_Getch`、`xapi_Input`、`SXAH_*`）仍保留，供 login 和旧应用使用。
- 独立 `x3tp_terminal_view_t` 模块已删除；它的字节环、行缓冲和 `xapi_Getch` 兼容排空并入
  X3TP，类型为 `x3tp_terminal_view_t`。`SXAH_MARK_IS_TERMINAL` 只设置 legacy
  `is_shell`，不注册/切换焦点。
- `x3tp_terminal_getch()` 在字节环为空时把 seat0 `CHAR` 事件 drain 到调用者自己的
  `x3tp_terminal_view_t`。这是兼容 drain，不是焦点模型；它与 `/dev/console` 同时读会
  竞争同一 ring，只应作为过渡期存在。
- 键盘驱动不再维护 `kb_fifo` / `kb_synth` 第二条队列；`get_keyboard_input()` 直接
  读当前终端（或 kernel default）的 `x3tp_terminal_view_t`。`xapi_Getch`、`xapi_Input`
  的 `XAPI_INPUT_NO_ECHO`、`/dev/stdio` 的 legacy 行输入都因此落在同一 X3TP
  实现上。
- **重要**：login（`leopard-getty`）读 legacy 队列时，输入核心 ring 仍保留一份
  平行副本。新的 `/dev/console` 消费者打开时必须先读空丢弃，否则登录时的用户名/
  密码会被回放到 shell。conhost 在 `input_open_path()` 里做了这一步。

## 已知限制

1. USB 自动重复只在 legacy `kb_synth` 里合成，`/dev/console` 只看到物理按下。
2. 多个 xHCI 控制器会违反“一 source 一生产者”，需要按控制器拆 source。
3. endpoint 的 open/close 没有绑定 fd 生命周期（`device_t` 无 open/close 回调），
   endpoint0 在内核生命周期内保持 active。
4. 不支持阻塞读：空读永远 `-EAGAIN`，消费者必须 poll。
5. keypad 数字目前以 `CHAR` 出现，没有独立的 `KEY_*`，应用键盘模式不可达。
6. `KEY_*` 常量仍只在 `driver/ps2/keyboard.h`，未提升为 `x3tp.h` 公共 ABI。

## 已知上游限制：unix socket 双端 poll 死锁

`driver/fs/vfs/unixsock.cpp` 的 `unix_poll()` 先锁本 socket 再锁 peer
（`spin_lock(&sk->lock)` → `spin_lock(&sk->peer->lock)`），与 OpenXJ380 一致。
两个进程同时 poll 同一连接的两端时会出现 ABBA 死锁，且死锁发生在关中断的
spinlock 内，表现为整机卡死、键盘 IRQ 不再响应。

这不是 X3TP 的问题，也不应在本仓库修上游代码。**用户态规避规则**：

- 一条 socket 连接只能由一端 poll；另一端用阻塞 `read()`（内核在等待时会释放
  socket 锁，不会参与锁序反转）。
- 新增用户态 socket 协议时先确认这一点，否则会随机整机死锁。
- **会话型 IPC 优先用 pipe**：pipefs 有真正的等待队列，`pipefs_poll` 只锁
  pipe 本身、没有 peer 锁，是树内最快的阻塞 IPC。`leopard-ime` 现在由
  conhost/compositor 创建 stdin/stdout 管道对并 fork manager，不再使用命名
  socket；只有 engine 回复等单一方向才用 poll。

## 历史方案（2026-10-28，已废弃）

早先设计是“源 / 拦截 / 路由 / 汇”四件套：`input_route_dispatch()` 查全局焦点
sink 表，可选拦截器（IME 过滤）走慢路径，router 线程作为焦点 endpoint 的唯一
生产者。2026-11-15 用 X3TP 取代，原因是：

- router 线程把 IRQ 到消费者之间多了一次调度，延迟和复杂度都不划算；
- 全局焦点表把 mono-session 简单场景复杂化，且焦点语义应由用户态（未来
  `graphics.sys`）决定；
- 拦截器把 IME 业务又拉回 ring 0，违反“内核只做输入路由”。

对应删除项：`input_router_init()`、router 线程、`g_router_lock`、`g_focus_id`
与焦点表、`input_route_dispatch*`、`input_route_set_interceptor`、
`x3tp termainal view_push_focused`、`x3tp termainal view_focus`、`INPUT_SOURCE_IME`。
