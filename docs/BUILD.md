# OpenXSKernel 构建指南

本文说明从零编译纯内核 `out/kernel.krl` 的完整流程。本仓库**只构建内核**，不含引导器、
用户态或磁盘镜像——那些在 OpenXSBoot / LeopardOS 仓库。

## 1. 环境要求

| 组件 | 用途 |
|------|------|
| Python 3 | 生成构建图 |
| Clang / LLD | 内核、驱动编译与链接 |
| Ninja | 构建驱动 |
| NASM | `.asm` 汇编 |

`liballoc-x86_64.a` 为仓库预置第三方库，无需下载。打包（`ninja pack`）额外需要
`xbps-create` 在 PATH 上（来自 `leopard-xbps` 仓库）。

## 2. 工具链安装（Debian/Ubuntu）

```bash
sudo apt-get install -y clang lld llvm ninja-build nasm python3
```

## 3. 编译步骤

```bash
# 1. 生成构建图（构建图不手改；修改需改 tools/gen_ninja.py）
python3 tools/gen_ninja.py

# 2. 构建内核
ninja
```

`gen_ninja.py` 同时生成 gitignored 的 `kernel/build_config.h`，其中的
`CONFIG_KN_VERSION` 从当前 git tag 派生（例如 `v2.2.1` -> `XSK 2.2.1`）。
OpenXJ380 固定为 `XSK 2.1.0`；不要手改生成文件，升级版本只打 tag 后重新生成。

产物：`out/kernel.krl`（内核 = kernel + 内建驱动 + 字体）。

可选目标：

```bash
ninja pack    # 打 xbps 包（gzip 压缩；需 xbps-create 在 PATH）
ninja clean   # 清理 out/、kernel/build_config.h、.pkg/
```

构建环境变量：`COMPILER_PREFIX`、`CC`、`CPP`、`NASM`、`LD`、`OBJC`、
`OPT_LEVEL`、`DIAGNOSTICS_COLOR`、`BUILTIN_XHCI`。

## 4. 运行验证

本仓库不直接跑 QEMU；完整的 live ISO 组装与启动验证流程见 **LeopardOS 仓库**
（`make_iso.sh` + QEMU）。内核被 OpenXSBoot 加载，PID1 由 `boot.conf` 的 `init=`
指定，内核解析 `BOOT_CONFIG.cmdline` 的 `init=` 后启动。

## 5. 常见问题

| 现象 | 原因与解决 |
|------|-----------|
| `ninja pack` 报 `xbps-create: not found` | 把 `leopard-xbps/out` 加进 PATH |
| 产物是 zstd 压缩 | 旧构建；`gen_ninja.py` 已切 gzip（本次会话决策，见根 AGENTS.md） |
| `build.ninja` 改了不生效 | 它是生成文件；改 `tools/gen_ninja.py` 后重新生成 |

## 6. 相关文件

- 构建图生成器：`tools/gen_ninja.py`
- 链接脚本：`linker.ld`
- 内核入口/启动：`kernel/main.cpp`
