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

模块包依赖 `OpenXSKernel>=2.1.0_1`，但不会自动进入默认镜像；需要时在镜像 manifest
里单独加入包名。

## 模块 ABI 约定

- 入口：`dlmain`（必需）和 `dlstart`（可选），必须 unmangled +
  `visibility("default")`。
- 模块通过内核导出符号（`EXPORT_SYMBOL` / `_OBJECT` / `_F`）使用内核原语。
- 模块编译 flags 由 `tools/gen_ninja.py` 的 `kmod_cxx` / `kmod_cc` 规则统一提供：
  `-fPIC -fvisibility=hidden` + 内核头路径。
- 模块只能引用内核导出表；不要把业务代码链进 `kernel.krl`。

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
