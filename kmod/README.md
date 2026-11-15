# 树内内核模块（kmod）

`OpenXSKernel` 允许把可选功能做成本仓库内的可加载模块，构建成 `.sys`，但**单独打包**，
不进入 `OpenXSKernel` 内核包。

## 目录布局

```
kmod/<name>/
  module.conf          # 元数据，可选
  *.cpp / *.c          # 模块源码；可用 sources= 指定子集
```

`module.conf` 示例：

```ini
name=example
version=2.1.0_1
description=OpenXSKernel example module
package=1
includes=include,third_party/example/include
cflags=-std=gnu11
cxxflags=-std=gnu++17
ldflags=-Wl,-z,muldefs
sources=src/a.c,src/b.cpp
```

- `package=0`：只构建 `out/kmod/<name>-kmod.sys`，不生成 xbps（适合 skeleton）。
- 默认 `package=1`：生成独立包 `<name>-kmod-<version>.x86_64.xbps`，
  安装到 `/mod/<name>-kmod.sys`。
- 有 `sources=` 时只编译列出的源文件；否则递归收集 `.c` / `.cpp`。

## 构建 / 打包

```sh
python3 tools/gen_ninja.py
ninja kmods                # 所有模块 .sys
ninja kmod-<name>          # 单个模块
ninja pack-kmods           # 所有模块包
ninja pack-kmod-<name>     # 单个模块包
ninja pack                 # 仅内核包
```

模块包依赖 `OpenXSKernel>=2.2.0_2`，但不会自动进入默认镜像；需要时在镜像 manifest
里单独加入包名。

## 模块 ABI 约定

- 入口：`dlmain`（必需）和 `dlstart`（可选），必须 unmangled +
  `visibility("default")`。
- 模块通过内核导出符号（`EXPORT_SYMBOL` / `_OBJECT` / `_F`）使用内核原语。
- 模块编译 flags 由 `tools/gen_ninja.py` 的 `kmod_cxx` / `kmod_cc` 规则统一提供：
  `-fPIC -fvisibility=hidden` + 内核头路径。
- 模块只能引用内核导出表；不要把业务代码链进 `kernel.krl`。
- POSIX 常量（`O_*` / `F_*` / `SEEK_*` / `DT_*` / `AT_*` / `UTIME_*`）已从
  `include/fs/vfs/vfs.h` 移到 `include/abi/posix.h`。模块请显式
  `#include <abi/posix.h>`，不要依赖 `vfs.h` 的传递包含。
- 当前可用的 VFS 导出：`vfs_open` / `vfs_close` / `vfs_read` / `vfs_write` /
  `vfs_add_access_hook`。`rootdir`、`normalize_path`、`vfs_set_access_hook` 以及
  创建/删除/截断原语（`vfs_mkdir` / `vfs_mkfile` / `vfs_delete` / `vfs_resize`）
  已移除；依赖它们的模块需要改代码：`vfs_set_access_hook` → `vfs_add_access_hook`，
  根目录改用 `vfs_open("/")`。

### 内核导出符号（模块 ABI）

`kmod/` 模块当前实际引用的导出（以 `nm -u out/kmod/*.sys` 为准）都在以下分组内：

| 分组 | 导出符号 |
|---|---|
| VFS | `vfs_open` `vfs_close` `vfs_read` `vfs_write` `vfs_add_access_hook` |
| alloc | `malloc` `free` `calloc` `realloc` `aligned_alloc`（经 `__wrap_*`）、`alloc_frames` `free_frames` `alloc_frames_dma32` `free_frames_dma32` |
| string/libc | `memset` `memcpy` `memcmp` `memchr` `strlen` `strnlen` `strcpy` `strncpy` `strcat` `strcmp` `strncmp` `strchr` `strrchr` `strstr` `strdup` `atoi` `strtol` `isspace` `isdigit` `cmd_parse_limit` |
| console/log | `printk` `pr_debug` `pr_info` `pr_warn` `pr_notice` `sprintf` `snprintf` `write_serial_fmt` `write_serial_string` |
| task/sched | `get_current_task` `scheduler_yield` `scheduler_sleep_ns` `scheduler_wake_task` `create_kernel_thread` `delay_ms_hp` `nanoTime` `bootNanoTime` `mutex_create` `mutex_lock` `mutex_trylock` `mutex_unlock` `mutex_lock_recoverable` `mutex_abort_for_process` |
| memory/paging | `phys_to_virt` `virt_to_phys` `driver_phys_to_virt` `driver_virt_to_phys` `page_map_to` `page_map_range` `page_map_range_to` `page_map_range_to_random` `page_virt_to_phys` `get_current_directory` `translate_address` `page_table_get_flags` `lazy_tryalloc` |
| net/socket | `regist_netdev` `get_default_netdev` `netdev_send` `netdev_recv` `socket_provider_register` |
| device/pci | `regist_device` `delete_device` `pci_find_class` `pci_find_class_n` `partition_device_added` |
| input driver | `keyboard_usb_key_event` `mouse_inject_report`（xhci 使用） |
| input/IME router | **not exported**: `input_route_*`, `input_legacy_*`, `OpenXJ380Socket_*` — kernel-internal; Input Core v2 routes `source -> seat -> endpoint`, userspace reads `/dev/console` |
| virtual keyboard | user space uses `/dev/vkbd`（UTF-8 text, `root:root 0600`），not a symbol ABI |
| user/session | `user_session_is_ready`、`current_user`（对象） |
| xapi_json | `xapi_parseJson` `xapi_freeJsonValue` `xapi_freeJsonDocument` `xapi_jsonObjectGet` `xapi_jsonArraySize` `xapi_jsonArrayAt` |

- **已移除导出：`create_user_process_from_file`**。它只服务内核内部 PID1/runfile
  启动；模块启动进程请走 syscall/RUN，内核不承诺进程创建 ABI。
- **重复导出清理**：`driver/pci/pci.cpp` 不再重复导出 `malloc` / `free`，统一由
  `kernel/memory/heap.cpp` 的 `__wrap_*` 提供（原先两组 ksymtab 条目指向同一地址）。
- **移除导出后模块必须重新构建**：`.sys` 在加载时解析符号，仍引用已移除导出的
  模块会以 unresolved-symbol 加载失败。内核导出表变更后请重新
  `ninja kmods`，并用 `nm -u out/kmod/*.sys` 复核模块引用。

- **Personality 模块（`graphics.sys` / `network-kmod` / `ime-kmod`）尚不能完整实现**：
  legacy 单槽 `OpenXJ380Socket_RegisterSyscallHook` 也已从导出表移除（函数仍在核内），
  且它原本只对 `switch` 未命中的 syscall 生效；GUI XAPI 的已知编号仍硬编码返回
  `-ENOSYS`。不要往 `syscall.cpp` 里加模块私有 `case`；方案与限制见根 `AGENTS.md`
  的 “OPTIONAL PERSONALITY MODULES + SYSCALL HOOKS” 一节。

## 加载

内核启动时扫描 `/mod/*.sys`，由 dlinker 加载并调用 `dlmain`。模块包安装到 `/mod`。
当前加载发生在 root 挂载之后、`scheduler_start` 之前；`module_setup` 会先
`vfs_open()` 目标文件以触发 ext4 的懒 inode 更新，避免直接读 child 节点得到 size=0。
如果模块需要阻塞 I/O 或长时间初始化，仍建议单独评估 early-boot 行为。

## 当前树内模块

| 目录 | 产物 | 包 | 说明 |
|---|---|---|---|
| `kmod/e1000` | `out/kmod/e1000-kmod.sys` | `e1000-kmod` | Intel E1000 网络驱动 |
| `kmod/netserver` | `out/kmod/netserver-kmod.sys` | `netserver-kmod` | lwIP 网络协议栈 |
| `kmod/xhci` | `out/kmod/xhci-kmod.sys` | `xhci-kmod` | xHCI USB 主机控制器 |
| `kmod/ime` | `out/kmod/ime-kmod.sys` | —（`package=0`） | IME 兼容 skeleton；真实业务在 `ime-kmod` |
