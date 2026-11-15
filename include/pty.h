#pragma once

#include <cpu/lock.h>
#include <fs/vfs/vfs.h>
#include <id_alloc.h>
#include <llist.h>
#include <tty.h>

#define PTY_BUFF_SIZE 4096
#define MAX_PTY_DEVICE 256

typedef struct pty_handle {
    int                 id;
    vfs_node_t          master_node;
    vfs_node_t          slave_node;
    struct termios      term;
    struct winsize      win;
    struct vt_mode      vt_mode;
    spin_t              lock;
    int                 ctrl_pgid;      // foreground process group (0 = unset)
    int                 ctrl_sid;       // session that owns this as controlling tty (0 = none)
    int                 ctrl_owner_pid; // process that attached it via TIOCSCTTY
    bool                hup_sent;       // SIGHUP is delivered once when the master goes away
    int                 master_fds;
    int                 slave_fds;
    int                 tty_kbmode;
    size_t              ptr_master;
    size_t              ptr_slave;
    bool                locked;
    uint8_t            *master_buffer;
    uint8_t            *slave_buffer;
    struct llist_header list_node;
    // Per-byte record markers keep EOF distinct from user data, including NUL.
    size_t              canonical_start;
    uint8_t             input_flags[PTY_BUFF_SIZE];
} pty_handle_t;

void pty_init();

// Job-control helpers used by the syscall/procfs layers.
void pty_detach_controlling(size_t sid, size_t owner_pid);
int  pty_session_foreground_pgid(size_t sid);

