# `src/net.h` + backends — the networking abstraction

| file | lines | platform |
|---|---|---|
| `net.h` | 73 | the contract |
| `net_posix.c` | ~290 | BSD sockets |
| `net_win32.c` | ~292 | Winsock 2 |

**This is the abstraction that makes Windows XP support cost one file.**

## The contract

Twelve functions. Opaque `net_listener` and `net_conn` handles.

```c
int  net_init(void);
void net_shutdown(void);
net_listener *net_listen(const char *host, int port, int backlog);
net_conn     *net_accept(net_listener *l, int timeout_ms, int *status);
net_conn     *net_connect(const char *host, int port, int timeout_ms);
int  net_read(net_conn *c, void *buf, size_t len, int timeout_ms);
int  net_write_all(net_conn *c, const void *buf, size_t len);
void net_set_nodelay(net_conn *c, int on);
void net_close(net_conn *c);
const char *net_peer(net_conn *c);
const char *net_last_error(void);
void net_listener_close(net_listener *l);
```

No socket descriptors, no `sockaddr`, no `WSADATA`, no `errno` crosses
this boundary. `server.c` and `mcp.c` include `net.h` and nothing else,
and are identical on both platforms.

You can verify the discipline:

```sh
grep -lE "socket|sockaddr|SOCKET|WSA|fd_set" src/server.c src/mcp.c
```

The only hit in `server.c` is the word "socket" in a comment.

## XP constraints, deliberately honoured

`net_win32.c` uses only what shipped with Windows XP:

* `select()` — **not** `WSAPoll()`, which is Vista+
* `gethostbyname()` — **not** `getaddrinfo()`, absent on Windows 2000
* the first argument to `select()` is ignored on Winsock, so it passes 0

Result: the `.exe` imports exactly **KERNEL32.dll, msvcrt.dll,
WS2_32.dll**.

## Porting

Implement these twelve functions plus the four in [sys-h.md](sys-h.md),
add a Makefile target, change nothing else.

## See also

- [server-c.md](server-c.md) · [mcp-c.md](mcp-c.md) · [sys-h.md](sys-h.md)
