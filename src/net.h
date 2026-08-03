/* net.h -- platform-independent networking interface for infer.
 *
 * Nothing above this header may include <sys/socket.h>, <winsock2.h> or
 * any other OS networking header. Every platform provides one .c file
 * implementing exactly the functions below:
 *
 *   net_posix.c   -- Linux / BSD / other Unix (BSD sockets)
 *   net_win32.c   -- Windows XP and later (Winsock 2)
 *
 * The build picks one; the rest of the program is unchanged.
 *
 * ANSI C (C89) only in the interface. The implementation files are the
 * only place where OS-specific headers and types may appear.
 */

#ifndef INFER_NET_H
#define INFER_NET_H

#include <stddef.h>

/* Opaque handles. The implementation defines the structs. */
typedef struct net_listener net_listener;
typedef struct net_conn     net_conn;

/* Return codes. */
#define NET_OK        0
#define NET_ERR      (-1)
#define NET_CLOSED   (-2)
#define NET_TIMEOUT  (-3)

/* One-time process-wide start-up / shutdown.
 * On Windows this performs WSAStartup / WSACleanup; on POSIX it installs
 * the SIGPIPE handler. Must be called before any other net_* function. */
int  net_init(void);
void net_shutdown(void);

/* Create a listening TCP socket bound to `host` (NULL or "0.0.0.0" for
 * all interfaces) and `port`, with the given backlog.
 * Returns NULL on failure; a human-readable reason is left in
 * net_last_error(). */
net_listener *net_listen(const char *host, int port, int backlog);
void          net_listener_close(net_listener *l);

/* Connect out to a TCP server (used by the MCP client). Returns NULL on
 * failure, with the reason in net_last_error(). `host` may be a dotted
 * quad or a hostname; name resolution uses gethostbyname(), which is
 * available on every target including Windows XP and Win95. */
net_conn *net_connect(const char *host, int port, int timeout_ms);

/* Block until a client connects. Returns NULL on error.
 * If `timeout_ms` >= 0 the call returns NULL with NET_TIMEOUT stored in
 * *status when no client arrived in time. `status` may be NULL. */
net_conn *net_accept(net_listener *l, int timeout_ms, int *status);

/* Read up to `len` bytes. Returns the number of bytes read (> 0),
 * NET_CLOSED at end of stream, NET_TIMEOUT, or NET_ERR. */
int  net_read(net_conn *c, void *buf, size_t len, int timeout_ms);

/* Write all `len` bytes. Returns NET_OK, or NET_ERR / NET_CLOSED. */
int  net_write_all(net_conn *c, const void *buf, size_t len);

/* Disable Nagle so streamed tokens are delivered promptly. */
void net_set_nodelay(net_conn *c, int on);

void net_close(net_conn *c);

/* Peer address as a printable string, e.g. "127.0.0.1:51234". */
const char *net_peer(net_conn *c);

/* Last error string for the calling thread (static storage). */
const char *net_last_error(void);

#endif /* INFER_NET_H */
