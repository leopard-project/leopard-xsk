# ext4 使用注意事项

## 1. 不要启用 `orphan_file`

**格式化 ext4 时必须关闭 `orphan_file`。**

- `mke2fs`（约 1.47 起）默认启用它；该特性是 compat 位 `0x1000`。
- 内核 ext4 挂载只接受 compat `0x23c`，未知位会返回 `-EOPNOTSUPP`，
  启动时表现为 `Bound root refused: -95`。

做法：

```bash
# 新分区
mkfs.ext4 -O ^orphan_file -L LEOPARD /dev/sdX

# 已格式化、尚未写数据（已掉电过先 e2fsck -f）
tune2fs -O ^orphan_file /dev/sdX
e2fsck -fn /dev/sdX
```

确认：

```bash
dumpe2fs -h /dev/sdX | grep Features   # 不应出现 orphan_file
```

本项目内：

- `leopard-utils/mkfs-ext4` 不启用该特性，安装器路径默认正常。
- `leopard-os/img-tools/assemble_image.sh` 使用
  `mkfs.ext4 -O ^orphan_file ...` 生成 root/users。
- 用宿主 `mkfs.ext4` 手工建分区/做镜像时需要自己加这个参数。

`orphan_file` 是可选特性，关闭后文件系统仍是标准 ext4，Linux 照常挂载；
只是掉电后的孤立 inode 回收走传统路径。

## 2. 分区大小按 4 KiB 对齐

`mkfs-ext4` 的 formatter profile 以 4 KiB 块为单位，分区字节数必须是 4096 的倍数。
安装器会按 8 个扇区（4 KiB）对齐 ext4 分区；手工分区时注意同样对齐。

## 3. ESP 必须是合法 FAT32

FAT32 规范要求至少 65525 个簇。64 MiB 的 ESP 需要 512 字节簇（`spc=1`），
`mkfs-fat32` 会在小卷上自动降低簇大小；用其它工具时注意不要生成簇数不足的
“假 FAT32”，否则 FatFs 会报 `FR_NO_FILESYSTEM`。
