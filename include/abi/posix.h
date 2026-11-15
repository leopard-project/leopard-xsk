#pragma once

/*
 * POSIX/Linux ABI numeric constants shared by the kernel syscall layer and
 * loadable modules. Macro-only: no types, no includes, safe in freestanding
 * C and C++.
 *
 * Values must match the Linux/musl ABI byte-for-byte: userspace binaries
 * (musl, busybox, xbps) pass these numbers through raw syscalls.
 */

/* ---- open(2) / fcntl(2) ------------------------------------------------ */
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_GETLK         5
#define F_SETLK         6
#define F_SETLKW        7
#define F_SETOWN        8
#define F_GETOWN        9
#define F_SETSIG        10
#define F_GETSIG        11
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC      1

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_ACCMODE   3
#define O_CREAT     0100
#define O_EXCL      0200
#define O_NOCTTY    0400
#define O_TRUNC     01000
#define O_APPEND    02000
#define O_NONBLOCK  04000
#define O_DSYNC     010000
#define O_SYNC      04010000
#define O_RSYNC     04010000
#define O_DIRECTORY 0200000
#define O_NOFOLLOW  0400000
#define O_CLOEXEC   02000000

#define O_ASYNC     020000
#define O_DIRECT    040000
#define O_LARGEFILE 0100000
#define O_NOATIME   01000000
#define O_PATH      010000000
#define O_TMPFILE   020200000
#define O_NDELAY    O_NONBLOCK

/* <ioctl.h> owns FIOCLEX (Linux UAPI). Keep a guarded alias so modules that
 * used to get it transitively from <fs/vfs/vfs.h> keep working. */
#ifndef FIOCLEX
#    define FIOCLEX 0x5451
#endif

/* ---- lseek(2) ---------------------------------------------------------- */
#define SEEK_SET  0 /* Seek from beginning of file.  */
#define SEEK_CUR  1 /* Seek from current position.  */
#define SEEK_END  2 /* Seek from end of file.  */
#define SEEK_DATA 3
#define SEEK_HOLE 4

/* ---- getdents(2) ------------------------------------------------------- */
/* Separate namespace from the ELF DT_* dynamic tags in <elf.h>. */
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12
#define DT_WHT     14

/* ---- *at(2) path resolution ------------------------------------------- */
/* Separate namespace from the ELF AT_* auxv tags in <elf.h>. */
#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EMPTY_PATH       0x1000

/* ---- utimensat(2) ------------------------------------------------------ */
#define UTIME_NOW  ((1L << 30) - 1L)
#define UTIME_OMIT ((1L << 30) - 2L)
