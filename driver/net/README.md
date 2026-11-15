# driver/net — network pseudo-filesystems (nmfs, dnsfs)

Network status and configuration are exposed **as files**, in the Unix
"everything is a file" tradition. There is no private ioctl, syscall, or other
out-of-band kernel control channel for the network service: user space (the
NetworkManager daemon and its clients) configures the stack by `open`/`read`/
`write`-ing ordinary paths under `/run`, exactly like `/proc` or `/sys` on
Linux. Shell tools (`cat`, `echo >`) work without special libraries.

```
os_open("/run/NetworkManager/status");  /* query   */
os_write("/run/NetworkManager/ipv4");   /* apply   */
```

`nmfs.cpp` (NetworkManager runtime FS) and `dnsfs.cpp` (resolver FS) register
themselves with the VFS and are mounted with `vfs_mount_virtual()`. The rest of
the kernel only sees generic VFS nodes, so the network service keeps working
with any filesystem client (shell, busybox, package manager) that can open a
file.

## Mounted paths

| Path | Access | Meaning |
|------|--------|---------|
| `/run/NetworkManager/status` | read-only | Full multi-section status report (device, IPv4, IPv6, connectivity). |
| `/run/NetworkManager/state` | read-only | Compact state summary: `state=`, `connectivity=`, `managed=`. |
| `/run/NetworkManager/ipv4` | read/write | Read = current IPv4 configuration; write = apply a new configuration. |
| `/run/NetworkManager/ipv6` | read-only | Current IPv6 addresses (`link-local=`, `global=`). |
| `/run/dns/resolve` | read/write | Write = perform a lookup; read = last lookup result. |
| `/run/dns/server` | read/write | Read = current DNS server; write = set the DNS server. |

The directories (`/run/NetworkManager`, `/run/dns`) are created by the setup
functions; the files are children of those directories and are the stable
interface.

### `/run/NetworkManager/status` (read)

INI-like text, one section per concern:

```ini
[device]
managed=true
interface=e0
state=connected          # starting | connected | configuring | disconnected
carrier=true
mac=52:54:00:12:34:56
mtu=1500

[ipv4]
method=auto              # auto | manual | disabled
dhcp-state=bound
address=10.0.2.15
netmask=255.255.255.0
gateway=10.0.2.2
dns=10.0.2.3

[ipv6]
link-local=fe80:...
global=

[connectivity]
state=full               # full | none | unknown
```

If the network stack is not available the file still opens and reports
`state=unavailable`. Writes are rejected.

### `/run/NetworkManager/state` (read)

Small machine-readable summary, convenient to poll:

```ini
state=connected
connectivity=full
managed=true
```

If the network stack is unavailable it reports only `state=unavailable`.

### `/run/NetworkManager/ipv4` (read/write)

Read returns the live configuration in the same `key=value` form as the
`[ipv4]` section above.

Write applies a configuration. The payload is a **complete document**, not a
delta; one `write()` must contain all the keys it wants to set:

```ini
method=auto                # or: dhcp | manual | static
address=192.168.1.10       # required when method=manual
netmask=255.255.255.0      # required when method=manual
gateway=192.168.1.1        # optional; 0.0.0.0 clears it
```

- `method` is mandatory; `manual`/`static` additionally require `address` and
  `netmask`.
- Unknown keys, malformed addresses, or an incomplete payload make the write
  fail; nothing is applied.
- Payloads are bounded (256 bytes including the terminating NUL); a payload
  larger than that is truncated before parsing, so send the document in one
  write.
- `#` starts a comment line; blank lines are ignored.

### `/run/NetworkManager/ipv6` (read)

```ini
link-local=fe80:...
global=
```

Read-only; writes are rejected.

### `/run/dns/resolve` (read/write)

Write performs a synchronous lookup. The payload is one request:

| Payload | Family |
|---------|--------|
| `example.com` | IPv4 (`AF_INET`) |
| `ipv4 example.com` / `a example.com` | IPv4 |
| `ipv6 example.com` / `aaaa example.com` | IPv6 |
| `auto example.com` / `unspec example.com` | IPv6, falling back to IPv4 |

Read returns the last cached answer as text (an address, or an empty string
when no lookup has succeeded yet). A failed lookup still consumes the request
and leaves the answer empty.

### `/run/dns/server` (read/write)

Read returns the current server in resolv.conf syntax:

```
nameserver 10.0.2.3
```

Write sets the server: an IPv4 address, or `auto` / `dhcp` / an empty value to
go back to the DHCP-provided server. Invalid addresses are rejected.

## File semantics and permissions

These are ordinary regular files:

- `stat()` reports `S_IFREG`, a real `st_size` (the length of the current
  view) and `st_nlink = 1`. They are not character devices and no longer have
  an unknown/zero size.
- `read()` is offset based: it returns the bytes from the requested offset and
  returns `0` at or after the end of the current view, so `cat file`, `head`,
  and any read-until-EOF loop terminate normally. The view length is
  recomputed on every access, so after a successful write the next read sees
  the new content together with its new length.
- Like a regular file, a `write()` leaves the file position at the end of the
  written data; use `lseek(fd, 0, SEEK_SET)` before reading the new view again
  on the same descriptor (a fresh `open()` also works).
- The content is generated from the live network stack on every read; nothing
  is cached on disk.

| Path | Owner / mode | Who may write |
|------|--------------|---------------|
| `/run/NetworkManager` (dir) | `root:root 0755` | root |
| `/run/dns` (dir) | `root:root 0755` | root |
| `status`, `state`, `ipv6` | `root:root 0444` | nobody (read-only) |
| `ipv4`, `resolve`, `server` | `root:root 0644` | root only |

Everyone may read; only Root may apply configuration. A refused write is
reported through the normal syscall error path (`EIO`); there is no alternative
control channel.

## Implementation notes

- The nodes are created as `file_none` (the VFS type that maps to `S_IFREG`).
  The `stat` callback fills `size` from the same builder/state the `read`
  callback uses, so the published size always matches the readable bytes, and
  `stat` keeps `type = file_none` instead of flipping back to a stream type.
- Moving these filesystems from `driver/fs/vfs/` to `driver/net/` is an
  **internal kernel-layout change only**. The inode/interface contract is the
  paths above, and it does not change: no mount point, file name, content
  format, access mode, or file type was altered by the move.

## Policy for future work

The files are the interface. When NetworkManager needs new status or
configuration, **extend the files** — add a key to `status`, add a file under
`/run/NetworkManager`, or add a section to `ipv4` — instead of adding a new
syscall, ioctl number, or `/dev`-style private channel. Kernel-side additions
must stay generic VFS primitives; distribution or daemon policy belongs in user
space.
