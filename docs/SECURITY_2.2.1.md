# leopard-xsk 2.2.1 — security patch

This release hardens the interfaces introduced by the X3TP/PTY console work and
the doas helper. It is a focused security update: no new syscall numbers and no
new product policy in the kernel.

## 1. PTY slave permissions

The login chain runs on a PTY (`conhost -> getty -> shell`). Slaves were
created and re-opened as `0666`, so any process could open another session's
`/dev/pts/N`, read typed passwords and inject commands.

- `/dev/pts/N` is no longer world read/write.
- The slave starts with the creator's identity; `TIOCSCTTY` re-stamps it for
  the session that owns it; after authentication,
  `pty_set_controlling_owner()` hands it to the logged-in identity.
- Final mode is `0620` (owner rw, group w) when a group is known, otherwise
  `0600`.
- `/dev/ptmx` remains `0666` (normal Unix behaviour).
- `pts_open()` no longer resets the node's DAC on every open.

## 2. Console and framebuffer devices

- `/dev/console` is `root:root 0600` (was `root:Admin 0660`). Only the trusted
  console host, which runs as root, reads raw X3TP events. Admin processes can
  no longer keylog the login prompt or steal/consume console events.
- `/dev/fb0` is `root:root 0600` (was `root:Admin 0660`). Raw scanout access
  is limited to the console/compositor host and no longer defeats secret
  capture mode for every Admin application.

## 3. `XAPI_SET_SECRET` and the framebuffer mapping

`screen_capture_register_mapping()` used to count every raw framebuffer
mapping. conhost maps `/dev/fb0` at boot and lives forever, so
`XAPI_SET_SECRET` always returned `-EBUSY`.

- Root/System mappings are now treated as trusted and do not reserve capture
  mode.
- Non-root mappings keep the old behaviour: they register as raw-mapped and
  make `screen_capture_set_secret()` fail with `-EBUSY`.
- `XAPI_SET_SECRET` works again on a booted system with conhost running.

## 4. Password storage

`/system/config/usereg.dat` used to store account passwords as plaintext.

- New entries use `XSH1` inside `UserInfo.password[64]`:

  ```text
  [0..3]   magic "XSH1"
  [4..7]   uint32 little-endian PBKDF2 iteration count (50000 for new hashes)
  [8..23]  16-byte random salt from the kernel RNG
  [24..55] 32-byte PBKDF2-HMAC-SHA256 derived key
  [56..63] zero
  ```

- Verification accepts iteration counts in `[1000, 1000000]`; out-of-range
  values fail closed without a long loop.
- Digest comparison is constant time. Temporary password/digest buffers are
  wiped.
- Legacy plaintext entries are still accepted and are rewritten as hashes
  after the next successful login (or successful doas password check), so an
  upgraded image removes plaintext without a separate migration tool.
- `current_user` no longer carries password material; user-list and
  `GetCurrentUser` copies never exposed it.
- The registry stays `root:root 0600`.

## 5. setuid write semantics

When a non-root process changes the contents of a regular file via
`write/pwrite/writev/ftruncate/copy_file_range/sendfile` or `O_TRUNC`, the
kernel clears `S_ISUID/S_ISGID` on that file. Pipes, sockets and device nodes
are excluded. Root writes keep the bits (matching Unix `CAP_FSETID`
behaviour).

## 6. doas

`leopard-doas 1.0_2`:

- The config must be owned by uid 0 and not group/world writable
  (`mode & 022 == 0`), checked with `fstat()` on the descriptor that is read.
  An insecure or unreadable config is a hard failure; there is no fallback.
- The elevated command no longer inherits the caller's environment. Only
  `TERM`, `LANG`, `LC_*` and `COLORTERM` survive; `PATH`, `USER`, `LOGNAME`
  and `HOME` are forced to fixed values; `LD_*`, `IFS`, `ENV`, `BASH_ENV`,
  `SHELL`, `PWD` and `OLDPWD` are dropped.
- Command lookup uses only the fixed safe directories, never the caller's
  `PATH`.

## Compatibility

- `x3tp_event_t` (24 bytes) and the `/dev/console` read ABI are unchanged.
- `UserInfo` on-disk layout is unchanged (`password[64]` is reused); old
  registries migrate in place.
- No new `XAPI_*`/`SXAH_*` numbers; no new exported symbols.
- `/dev/stdio`, `/dev/tty`, `/dev/null`, `/dev/urandom` keep their existing
  modes.

## Known issue (tracked for 2.2.2)

- Opening and closing `/dev/pts/N` from a second process can terminate the
  session that owns that slave. This is a pre-existing PTY node/fd lifetime
  issue (`pts_close`/refcount), not introduced by the DAC change; the 2.2.1
  mode/owner work reduces the reachable set to the session user but does not
  fix the lifetime bug. It is an availability issue for same-user processes,
  not a privilege boundary crossing.

## Test coverage

- QEMU: OOBE/login/logout, doas password check, doas config refusal, doas
  environment, PTY/console/fb DAC, `XAPI_SET_SECRET`, IME fast path.
- PBKDF2/SHA-256 implementation checked against RFC 6234 / RFC 7914 test
  vectors on the host.
- Legacy `usereg.dat` (plaintext) is accepted, then rewritten as `XSH1` after
  a successful login.
