/* net_win32.c -- Winsock 2 backend for net.h (Windows XP and later).
 *
 * This is the ONLY file in the Win32 build that touches OS networking
 * headers. Everything above net.h is shared verbatim with the POSIX
 * build.
 *
 * Deliberately restricted to the Winsock 2 API that shipped with
 * Windows XP / ws2_32.dll:
 *   - no getaddrinfo() (XP has it, but 2000 does not; inet_addr is used)
 *   - no WSAPoll()     (Vista+); select() is used instead
 *
 * Build (MinGW targeting XP):
 *   i686-w64-mingw32-gcc -std=c89 -D_WIN32_WINNT=0x0501 ... -lws2_32
 *
 * ANSI C (C89) plus the Winsock API. No third-party libraries.
 */

#include "net.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501     /* Windows XP */
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct net_listener {
    SOCKET fd;
};

struct net_conn {
    SOCKET fd;
    char   peer[64];
};

static char net_err[256];
static int  net_started = 0;

static void set_err(const char *what) {
    int e = WSAGetLastError();
    sprintf(net_err, "%.80s: winsock error %d", what, e);
}

const char *net_last_error(void) {
    return net_err[0] ? net_err : "no error";
}

int net_init(void) {
    WSADATA wsa;
    int rc;

    net_err[0] = '\0';
    if (net_started) return NET_OK;

    rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        sprintf(net_err, "WSAStartup failed: %d", rc);
        return NET_ERR;
    }
    net_started = 1;
    return NET_OK;
}

void net_shutdown(void) {
    if (net_started) {
        WSACleanup();
        net_started = 0;
    }
}

net_listener *net_listen(const char *host, int port, int backlog) {
    net_listener *l;
    struct sockaddr_in addr;
    SOCKET fd;
    BOOL yes = TRUE;

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) {
        set_err("socket");
        return NULL;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &yes, sizeof(yes));

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
            closesocket(fd);
            return NULL;
        }
        addr.sin_addr.s_addr = a;
    }

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) == SOCKET_ERROR) {
        set_err("bind");
        closesocket(fd);
        return NULL;
    }
    if (listen(fd, backlog) == SOCKET_ERROR) {
        set_err("listen");
        closesocket(fd);
        return NULL;
    }

    l = (net_listener *) malloc(sizeof(net_listener));
    if (!l) {
        strcpy(net_err, "out of memory");
        closesocket(fd);
        return NULL;
    }
    l->fd = fd;
    return l;
}

void net_listener_close(net_listener *l) {
    if (!l) return;
    closesocket(l->fd);
    free(l);
}

/* Resolve a host name or dotted quad. gethostbyname() rather than
 * getaddrinfo(): the latter is absent on Windows 2000 and earlier, and
 * this file targets XP and below. */
static int resolve_host(const char *host, struct in_addr *out) {
    unsigned long a;
    struct hostent *he;

    a = inet_addr(host);
    if (a != INADDR_NONE) { out->s_addr = a; return 0; }
    he = gethostbyname(host);
    if (!he || he->h_addrtype != AF_INET || !he->h_addr_list[0]) return -1;
    memcpy(out, he->h_addr_list[0], sizeof(struct in_addr));
    return 0;
}

net_conn *net_connect(const char *host, int port, int timeout_ms) {
    struct sockaddr_in addr;
    net_conn *c;
    SOCKET fd;

    (void) timeout_ms;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short) port);
    if (resolve_host(host, &addr.sin_addr) != 0) {
        sprintf(net_err, "cannot resolve '%.100s'", host);
        return NULL;
    }

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) { set_err("socket"); return NULL; }

    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) == SOCKET_ERROR) {
        set_err("connect");
        closesocket(fd);
        return NULL;
    }

    c = (net_conn *) malloc(sizeof(net_conn));
    if (!c) { strcpy(net_err, "out of memory"); closesocket(fd); return NULL; }
    c->fd = fd;
    sprintf(c->peer, "%.40s:%d", host, port);
    return c;
}

/* 1 = readable, 0 = timeout, -1 = error. */
static int wait_readable(SOCKET fd, int timeout_ms) {
    fd_set rd;
    struct timeval tv;
    int r;

    if (timeout_ms < 0) return 1;

    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    /* On Winsock the first argument to select() is ignored. */
    r = select(0, &rd, NULL, NULL, &tv);
    if (r == SOCKET_ERROR) return -1;
    return r > 0 ? 1 : 0;
}

net_conn *net_accept(net_listener *l, int timeout_ms, int *status) {
    struct sockaddr_in addr;
    int alen = (int) sizeof(addr);
    net_conn *c;
    SOCKET fd;
    int w;

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

    fd = accept(l->fd, (struct sockaddr *) &addr, &alen);
    if (fd == INVALID_SOCKET) {
        set_err("accept");
        if (status) *status = NET_ERR;
        return NULL;
    }

    c = (net_conn *) malloc(sizeof(net_conn));
    if (!c) {
        strcpy(net_err, "out of memory");
        closesocket(fd);
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

    n = recv(c->fd, (char *) buf, (int) len, 0);
    if (n == 0) return NET_CLOSED;
    if (n == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e == WSAECONNRESET || e == WSAECONNABORTED) return NET_CLOSED;
        set_err("recv");
        return NET_ERR;
    }
    return n;
}

int net_write_all(net_conn *c, const void *buf, size_t len) {
    const char *p = (const char *) buf;
    size_t left = len;

    while (left > 0) {
        int n = send(c->fd, p, (int) left, 0);
        if (n == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAECONNRESET || e == WSAECONNABORTED ||
                e == WSAESHUTDOWN) return NET_CLOSED;
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
    BOOL v = on ? TRUE : FALSE;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, (const char *) &v, sizeof(v));
}

void net_close(net_conn *c) {
    if (!c) return;
    closesocket(c->fd);
    free(c);
}

const char *net_peer(net_conn *c) {
    return c ? c->peer : "?";
}
