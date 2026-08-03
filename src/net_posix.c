/* net_posix.c -- BSD sockets backend for net.h (Linux and other Unix).
 *
 * This is the ONLY file in the POSIX build that touches OS networking
 * headers. To port infer to another platform, write a sibling file that
 * implements the same net.h interface (see net_win32.c for Windows XP).
 *
 * ANSI C (C89) plus the POSIX socket API. No third-party libraries.
 */

#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

struct net_listener {
    int fd;
};

struct net_conn {
    int  fd;
    char peer[64];
};

static char net_err[256];

static void set_err(const char *what) {
    const char *e = strerror(errno);
    sprintf(net_err, "%.80s: %.150s", what, e ? e : "unknown error");
}

const char *net_last_error(void) {
    return net_err[0] ? net_err : "no error";
}

int net_init(void) {
    /* A client that hangs up mid-response must not kill the server. */
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    net_err[0] = '\0';
    return NET_OK;
}

void net_shutdown(void) {
}

net_listener *net_listen(const char *host, int port, int backlog) {
    net_listener *l;
    struct sockaddr_in addr;
    int fd, yes = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        set_err("socket");
        return NULL;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *) &yes, sizeof(yes));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short) port);

    if (host == NULL || host[0] == '\0' ||
        strcmp(host, "0.0.0.0") == 0 || strcmp(host, "*") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        unsigned long a = inet_addr(host);
        if (a == INADDR_NONE) {
            sprintf(net_err, "bad bind address '%.100s'", host);
            close(fd);
            return NULL;
        }
        addr.sin_addr.s_addr = a;
    }

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        set_err("bind");
        close(fd);
        return NULL;
    }
    if (listen(fd, backlog) < 0) {
        set_err("listen");
        close(fd);
        return NULL;
    }

    l = (net_listener *) malloc(sizeof(net_listener));
    if (!l) {
        strcpy(net_err, "out of memory");
        close(fd);
        return NULL;
    }
    l->fd = fd;
    return l;
}

void net_listener_close(net_listener *l) {
    if (!l) return;
    close(l->fd);
    free(l);
}

/* Resolve a host name or dotted quad into an IPv4 address.
 * gethostbyname() is deprecated in favour of getaddrinfo(), but
 * getaddrinfo() does not exist on Windows 2000/95 and this project
 * keeps one code shape across all targets. */
static int resolve_host(const char *host, struct in_addr *out) {
    unsigned long a;
    struct hostent *he;

    a = inet_addr(host);
    if (a != INADDR_NONE) {
        out->s_addr = a;
        return 0;
    }
    he = gethostbyname(host);
    if (!he || he->h_addrtype != AF_INET || !he->h_addr_list[0]) return -1;
    memcpy(out, he->h_addr_list[0], sizeof(struct in_addr));
    return 0;
}

net_conn *net_connect(const char *host, int port, int timeout_ms) {
    struct sockaddr_in addr;
    net_conn *c;
    int fd;

    (void) timeout_ms;   /* blocking connect; the OS timeout applies */

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short) port);
    if (resolve_host(host, &addr.sin_addr) != 0) {
        sprintf(net_err, "cannot resolve '%.100s'", host);
        return NULL;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { set_err("socket"); return NULL; }

    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        set_err("connect");
        close(fd);
        return NULL;
    }

    c = (net_conn *) malloc(sizeof(net_conn));
    if (!c) { strcpy(net_err, "out of memory"); close(fd); return NULL; }
    c->fd = fd;
    sprintf(c->peer, "%.40s:%d", host, port);
    return c;
}

/* Wait for readability. 1 = ready, 0 = timeout, -1 = error. */
static int wait_readable(int fd, int timeout_ms) {
    fd_set rd;
    struct timeval tv;
    int r;

    if (timeout_ms < 0) return 1;

    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    do {
        r = select(fd + 1, &rd, NULL, NULL, &tv);
    } while (r < 0 && errno == EINTR);

    if (r < 0) return -1;
    return r > 0 ? 1 : 0;
}

net_conn *net_accept(net_listener *l, int timeout_ms, int *status) {
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    net_conn *c;
    int fd, w;

    if (status) *status = NET_OK;

    w = wait_readable(l->fd, timeout_ms);
    if (w == 0) {
        if (status) *status = NET_TIMEOUT;
        return NULL;
    }
    if (w < 0) {
        set_err("select");
        if (status) *status = NET_ERR;
        return NULL;
    }

    do {
        fd = accept(l->fd, (struct sockaddr *) &addr, &alen);
    } while (fd < 0 && errno == EINTR);

    if (fd < 0) {
        set_err("accept");
        if (status) *status = NET_ERR;
        return NULL;
    }

    c = (net_conn *) malloc(sizeof(net_conn));
    if (!c) {
        strcpy(net_err, "out of memory");
        close(fd);
        if (status) *status = NET_ERR;
        return NULL;
    }
    c->fd = fd;
    sprintf(c->peer, "%.40s:%d", inet_ntoa(addr.sin_addr),
            (int) ntohs(addr.sin_port));
    return c;
}

int net_read(net_conn *c, void *buf, size_t len, int timeout_ms) {
    int w, n;

    w = wait_readable(c->fd, timeout_ms);
    if (w == 0) return NET_TIMEOUT;
    if (w < 0) {
        set_err("select");
        return NET_ERR;
    }

    do {
        n = (int) recv(c->fd, buf, len, 0);
    } while (n < 0 && errno == EINTR);

    if (n == 0) return NET_CLOSED;
    if (n < 0) {
        set_err("recv");
        return NET_ERR;
    }
    return n;
}

int net_write_all(net_conn *c, const void *buf, size_t len) {
    const char *p = (const char *) buf;
    size_t left = len;

    while (left > 0) {
        int n;
        do {
            n = (int) send(c->fd, p, left, 0);
        } while (n < 0 && errno == EINTR);

        if (n < 0) {
            if (errno == EPIPE || errno == ECONNRESET) return NET_CLOSED;
            set_err("send");
            return NET_ERR;
        }
        if (n == 0) return NET_CLOSED;
        p    += n;
        left -= (size_t) n;
    }
    return NET_OK;
}

void net_set_nodelay(net_conn *c, int on) {
    int v = on ? 1 : 0;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, (void *) &v, sizeof(v));
}

void net_close(net_conn *c) {
    if (!c) return;
    close(c->fd);
    free(c);
}

const char *net_peer(net_conn *c) {
    return c ? c->peer : "?";
}
