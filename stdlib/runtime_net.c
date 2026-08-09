/*
 * HaoLang runtime -- net (v0.14.0; Windows dynload since v0.21.0)
 * ------------------------------------------------------------
 *  TCP/UDP sockets, cross-platform:
 *  - Windows: lazy LoadLibrary("ws2_32.dll") + GetProcAddress;
 *    no SDK import lib, no #pragma comment, no winsock2.h link deps.
 *  - Linux/macOS: BSD sockets via libc/musl.
 *  Returned strings are GC-managed (gc_alloc).
 *
 *  Socket handles live in HaoLang Long? boxed units (8-byte GC blocks).
 *  Windows: lazy WSAStartup; POSIX: no-op.
 *  Bool returns use int8_t (Bool=i8 since v0.26.1).
 */
#define WIN32_LEAN_AND_MEAN
#include "runtime_internal.h"

/* ============================================================
 *  helpers
 * ============================================================ */

static void hao_net_portstr(char* buf, size_t n, int32_t port) {
    snprintf(buf, n, "%d", (int)port);
}

/* host byte-order helpers; avoid ws2_32 htons/htonl exports */
static uint16_t hao_htons(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
}
static uint32_t hao_htonl(uint32_t x) {
    return ((x & 0x000000FFu) << 24) |
           ((x & 0x0000FF00u) << 8)  |
           ((x & 0x00FF0000u) >> 8)  |
           ((x & 0xFF000000u) >> 24);
}

#ifdef _WIN32
/* ============================================================
 *  Windows: hand-written Winsock ABI + dynload (no winsock2.h/.lib)
 * ============================================================ */

typedef uintptr_t hao_sock_t;
#define HAO_INVALID_SOCKET ((hao_sock_t)~(hao_sock_t)0)

#define HAO_AF_UNSPEC    0
#define HAO_AF_INET      2
#define HAO_SOCK_STREAM  1
#define HAO_SOCK_DGRAM   2
#define HAO_SOL_SOCKET   0xffff
#define HAO_SO_REUSEADDR 0x0004
#define HAO_SO_RCVTIMEO  0x1006
#define HAO_SO_SNDTIMEO  0x1005
#define HAO_INADDR_ANY   0u

typedef struct hao_in_addr {
    uint32_t s_addr;
} hao_in_addr;

typedef struct hao_sockaddr {
    uint16_t sa_family;
    char sa_data[14];
} hao_sockaddr;

typedef struct hao_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    hao_in_addr sin_addr;
    char sin_zero[8];
} hao_sockaddr_in;

typedef struct hao_addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    size_t ai_addrlen;
    char* ai_canonname;
    hao_sockaddr* ai_addr;
    struct hao_addrinfo* ai_next;
} hao_addrinfo;

/* WSADATA ??Win64 ??400 ?????????????????*/
#define HAO_WSADATA_BUF 512

#ifndef WINAPI
#define WINAPI __stdcall
#endif

typedef int           (WINAPI *hao_fn_WSAStartup)(uint16_t, void*);
typedef hao_sock_t    (WINAPI *hao_fn_socket)(int, int, int);
typedef int           (WINAPI *hao_fn_connect)(hao_sock_t, const hao_sockaddr*, int);
typedef int           (WINAPI *hao_fn_bind)(hao_sock_t, const hao_sockaddr*, int);
typedef int           (WINAPI *hao_fn_listen)(hao_sock_t, int);
typedef hao_sock_t    (WINAPI *hao_fn_accept)(hao_sock_t, hao_sockaddr*, int*);
typedef int           (WINAPI *hao_fn_closesocket)(hao_sock_t);
typedef int           (WINAPI *hao_fn_send)(hao_sock_t, const char*, int, int);
typedef int           (WINAPI *hao_fn_recv)(hao_sock_t, char*, int, int);
typedef int           (WINAPI *hao_fn_setsockopt)(hao_sock_t, int, int, const char*, int);
typedef int           (WINAPI *hao_fn_getaddrinfo)(const char*, const char*,
                                                   const hao_addrinfo*, hao_addrinfo**);
typedef void          (WINAPI *hao_fn_freeaddrinfo)(hao_addrinfo*);
typedef int           (WINAPI *hao_fn_sendto)(hao_sock_t, const char*, int, int,
                                              const hao_sockaddr*, int);
typedef int           (WINAPI *hao_fn_recvfrom)(hao_sock_t, char*, int, int,
                                                hao_sockaddr*, int*);

static struct {
    void* lib;
    hao_fn_WSAStartup   WSAStartup;
    hao_fn_socket       socket;
    hao_fn_connect      connect;
    hao_fn_bind         bind;
    hao_fn_listen       listen;
    hao_fn_accept       accept;
    hao_fn_closesocket  closesocket;
    hao_fn_send         send;
    hao_fn_recv         recv;
    hao_fn_setsockopt   setsockopt;
    hao_fn_getaddrinfo  getaddrinfo;
    hao_fn_freeaddrinfo freeaddrinfo;
    hao_fn_sendto       sendto;
    hao_fn_recvfrom     recvfrom;
    int ready;   /* 1 = DLL + WSAStartup 完成 */
} g_ws;

/* 0=未开始 1=进行中 2=成功 3=失败（不再重试） */
static int g_ws_once = 0;

#define HAO_LOAD_SYM(field, name) do { \
    g_ws.field = (hao_fn_##field)hao_dl_sym(g_ws.lib, name); \
    if (!g_ws.field) goto fail_load; \
} while (0)

/* 线程安全：仅一线程加载 ws2_32 并 WSAStartup，避免并发 memset/坏函数指针 */
static int hao_net_ensure(void) {
    int st = __atomic_load_n(&g_ws_once, __ATOMIC_ACQUIRE);
    if (st == 2) return 1;
    if (st == 3) return 0;
    int expected = 0;
    if (!__atomic_compare_exchange_n(&g_ws_once, &expected, 1, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while ((st = __atomic_load_n(&g_ws_once, __ATOMIC_ACQUIRE)) == 1)
            hao_win_switch_to_thread();
        return st == 2;
    }
    if (!g_ws.lib) {
        g_ws.lib = hao_dl_open("ws2_32.dll");
        if (!g_ws.lib) goto fail_load;
        HAO_LOAD_SYM(WSAStartup,   "WSAStartup");
        HAO_LOAD_SYM(socket,       "socket");
        HAO_LOAD_SYM(connect,      "connect");
        HAO_LOAD_SYM(bind,         "bind");
        HAO_LOAD_SYM(listen,       "listen");
        HAO_LOAD_SYM(accept,       "accept");
        HAO_LOAD_SYM(closesocket,  "closesocket");
        HAO_LOAD_SYM(send,         "send");
        HAO_LOAD_SYM(recv,         "recv");
        HAO_LOAD_SYM(setsockopt,   "setsockopt");
        HAO_LOAD_SYM(getaddrinfo,  "getaddrinfo");
        HAO_LOAD_SYM(freeaddrinfo, "freeaddrinfo");
        HAO_LOAD_SYM(sendto,       "sendto");
        HAO_LOAD_SYM(recvfrom,     "recvfrom");
    }
    {
        char wsa[HAO_WSADATA_BUF];
        memset(wsa, 0, sizeof(wsa));
        /* MAKEWORD(2,2) = 0x0202 */
        if (g_ws.WSAStartup(0x0202, wsa) != 0) goto fail_load;
        g_ws.ready = 1;
        __atomic_store_n(&g_ws_once, 2, __ATOMIC_RELEASE);
        return 1;
    }
fail_load:
    if (g_ws.lib) hao_dl_close(g_ws.lib);
    memset(&g_ws, 0, sizeof(g_ws));
    __atomic_store_n(&g_ws_once, 3, __ATOMIC_RELEASE);
    return 0;
}

#undef HAO_LOAD_SYM

static void hao_net_store(int64_t* unit, hao_sock_t s) { *unit = (int64_t)s; }
static hao_sock_t hao_net_load(const int64_t* unit) { return (hao_sock_t)*unit; }

static void hao_net_unit_finalize(void* user) {
    int64_t* unit = (int64_t*)user;
    if (!unit) return;
    hao_sock_t s = hao_net_load(unit);
    if (s != 0 && s != HAO_INVALID_SOCKET && g_ws.closesocket)
        g_ws.closesocket(s);
    hao_net_store(unit, HAO_INVALID_SOCKET);
}

static void hao_net_attach(int64_t* unit, hao_sock_t s) {
    if (!unit) return;
    hao_sock_t old = hao_net_load(unit);
    if (old != 0 && old != HAO_INVALID_SOCKET && g_ws.closesocket)
        g_ws.closesocket(old);
    hao_gc_clear_finalizer(unit);
    hao_net_store(unit, s);
    if (s != 0 && s != HAO_INVALID_SOCKET)
        hao_gc_set_finalizer(unit, hao_net_unit_finalize);
}

int8_t hao_net_connect(int64_t* unit, HaoString* host, int32_t port) {
    if (!host || !hao_net_ensure()) return 0;
    char portstr[16];
    hao_net_portstr(portstr, sizeof(portstr), port);
    hao_addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = HAO_AF_UNSPEC;
    hints.ai_socktype = HAO_SOCK_STREAM;
    /* getaddrinfo/connect 可能阻塞：与 accept/recv 一样 park，避免拖垮 STW */
    hao_gc_add_root(host);
    hao_gc_os_block_enter();
    if (g_ws.getaddrinfo(host->data, portstr, &hints, &res) != 0) {
        hao_gc_os_block_leave();
        hao_gc_remove_root(host);
        return 0;
    }
    hao_sock_t s = HAO_INVALID_SOCKET;
    for (hao_addrinfo* p = res; p; p = p->ai_next) {
        s = g_ws.socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == HAO_INVALID_SOCKET) continue;
        if (g_ws.connect(s, p->ai_addr, (int)p->ai_addrlen) == 0) break;
        g_ws.closesocket(s);
        s = HAO_INVALID_SOCKET;
    }
    g_ws.freeaddrinfo(res);
    hao_gc_os_block_leave();
    hao_gc_remove_root(host);
    if (s == HAO_INVALID_SOCKET) return 0;
    hao_net_attach(unit, s);
    return 1;
}

int8_t hao_net_listen_backlog(int64_t* unit, int32_t port, int32_t backlog);

int8_t hao_net_listen(int64_t* unit, int32_t port) {
    return hao_net_listen_backlog(unit, port, 128);
}

int8_t hao_net_listen_backlog(int64_t* unit, int32_t port, int32_t backlog) {
    if (!hao_net_ensure()) return 0;
    if (backlog <= 0) backlog = 128;
    hao_sock_t s = g_ws.socket(HAO_AF_INET, HAO_SOCK_STREAM, 0);
    if (s == HAO_INVALID_SOCKET) return 0;
    int on = 1;
    g_ws.setsockopt(s, HAO_SOL_SOCKET, HAO_SO_REUSEADDR, (const char*)&on, sizeof(on));
    hao_sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = HAO_AF_INET;
    addr.sin_addr.s_addr = hao_htonl(HAO_INADDR_ANY);
    addr.sin_port = hao_htons((uint16_t)port);
    if (g_ws.bind(s, (const hao_sockaddr*)&addr, (int)sizeof(addr)) != 0 ||
        g_ws.listen(s, backlog) != 0) {
        g_ws.closesocket(s);
        return 0;
    }
    hao_net_attach(unit, s);
    return 1;
}

int8_t hao_net_set_recv_timeout(int64_t* unit, int32_t timeout_ms) {
    if (!hao_net_ensure() || !unit) return 0;
    hao_sock_t s = hao_net_load(unit);
    if (s == HAO_INVALID_SOCKET) return 0;
    uint32_t ms = timeout_ms < 0 ? 0u : (uint32_t)timeout_ms;
    if (g_ws.setsockopt(s, HAO_SOL_SOCKET, HAO_SO_RCVTIMEO,
                        (const char*)&ms, sizeof(ms)) != 0) return 0;
    return 1;
}

int8_t hao_net_set_send_timeout(int64_t* unit, int32_t timeout_ms) {
    if (!hao_net_ensure() || !unit) return 0;
    hao_sock_t s = hao_net_load(unit);
    if (s == HAO_INVALID_SOCKET) return 0;
    uint32_t ms = timeout_ms < 0 ? 0u : (uint32_t)timeout_ms;
    if (g_ws.setsockopt(s, HAO_SOL_SOCKET, HAO_SO_SNDTIMEO,
                        (const char*)&ms, sizeof(ms)) != 0) return 0;
    return 1;
}

int8_t hao_net_set_timeout(int64_t* unit, int32_t timeout_ms) {
    return hao_net_set_recv_timeout(unit, timeout_ms)
        && hao_net_set_send_timeout(unit, timeout_ms);
}

int8_t hao_net_accept(int64_t* srv, int64_t* cli) {
    if (!hao_net_ensure()) return 0;
    hao_sock_t sv = hao_net_load(srv);
    if (sv == HAO_INVALID_SOCKET) return 0;
    hao_gc_os_block_enter();
    hao_sock_t c = g_ws.accept(sv, NULL, NULL);
    hao_gc_os_block_leave();
    if (c == HAO_INVALID_SOCKET) return 0;
    hao_net_attach(cli, c);
    return 1;
}

int32_t hao_net_send(int64_t* unit, HaoString* data) {
    if (!hao_net_ensure()) return -1;
    hao_sock_t s = hao_net_load(unit);
    if (s == HAO_INVALID_SOCKET || !data) return -1;
    size_t total = (size_t)data->len;
    size_t sent = 0;
    /* data 可能只活在 C 形参；os_block 期间挂显式根 */
    hao_gc_add_root(data);
    hao_gc_os_block_enter();
    while (sent < total) {
        int n = g_ws.send(s, data->data + sent, (int)(total - sent), 0);
        if (n <= 0) {
            hao_gc_os_block_leave();
            hao_gc_remove_root(data);
            return -1;
        }
        sent += (size_t)n;
    }
    hao_gc_os_block_leave();
    hao_gc_remove_root(data);
    return (int32_t)sent;
}

HaoString* hao_net_recv(int64_t* unit, int32_t max) {
    if (!hao_net_ensure()) return NULL;
    hao_sock_t s = hao_net_load(unit);
    if (s == HAO_INVALID_SOCKET || max <= 0) return NULL;
    if (max > INT32_MAX - 1) max = INT32_MAX - 1;
    HaoString* buf = hao_str_alloc(max);
    hao_gc_add_root(buf);
    hao_gc_os_block_enter();
    int n = g_ws.recv(s, buf->data, (int)max, 0);
    hao_gc_os_block_leave();
    hao_gc_remove_root(buf);
    /* Go/Java: n==0 EOF -> empty String; n<0 timeout/error -> null */
    if (n < 0) return NULL;
    if (n == 0) {
        buf->data[0] = '\0';
        buf->len = 0;
        return buf;
    }
    buf->data[n] = '\0';
    buf->len = n;
    return buf;
}

int32_t hao_net_close(int64_t* unit) {
    if (!unit) return 0;
    hao_gc_clear_finalizer(unit);
    hao_sock_t s = hao_net_load(unit);
    /* 0 = ????.hao ??????? closesocket(0) */
    if (s != 0 && s != HAO_INVALID_SOCKET && g_ws.closesocket)
        g_ws.closesocket(s);
    hao_net_store(unit, HAO_INVALID_SOCKET);
    return 1;
}

int8_t hao_net_udp_open(int64_t* unit) {
    if (!hao_net_ensure()) return 0;
    hao_sock_t s = g_ws.socket(HAO_AF_INET, HAO_SOCK_DGRAM, 0);
    if (s == HAO_INVALID_SOCKET) return 0;
    hao_net_attach(unit, s);
    return 1;
}

int8_t hao_net_udp_bind(int64_t* unit, int32_t port) {
    if (!hao_net_ensure()) return 0;
    hao_sock_t s = hao_net_load(unit);
    if (s == HAO_INVALID_SOCKET) return 0;
    hao_sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = HAO_AF_INET;
    addr.sin_addr.s_addr = hao_htonl(HAO_INADDR_ANY);
    addr.sin_port = hao_htons((uint16_t)port);
    return g_ws.bind(s, (const hao_sockaddr*)&addr, (int)sizeof(addr)) == 0 ? 1 : 0;
}

int32_t hao_net_udp_sendto(int64_t* unit, HaoString* host, int32_t port,
                           HaoString* data) {
    if (!hao_net_ensure()) return -1;
    hao_sock_t s = hao_net_load(unit);
    if (s == HAO_INVALID_SOCKET || !host || !data) return -1;
    char portstr[16];
    hao_net_portstr(portstr, sizeof(portstr), port);
    hao_addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = HAO_AF_INET;
    hints.ai_socktype = HAO_SOCK_DGRAM;
    hao_gc_add_root(host);
    hao_gc_add_root(data);
    hao_gc_os_block_enter();
    if (g_ws.getaddrinfo(host->data, portstr, &hints, &res) != 0) {
        hao_gc_os_block_leave();
        hao_gc_remove_root(host);
        hao_gc_remove_root(data);
        return -1;
    }
    int64_t sent = -1;
    for (hao_addrinfo* p = res; p; p = p->ai_next) {
        int n = g_ws.sendto(s, data->data, (int)data->len, 0,
                            p->ai_addr, (int)p->ai_addrlen);
        if (n >= 0) { sent = (int64_t)n; break; }
    }
    g_ws.freeaddrinfo(res);
    hao_gc_os_block_leave();
    hao_gc_remove_root(host);
    hao_gc_remove_root(data);
    return sent;
}

HaoString* hao_net_udp_recvfrom(int64_t* unit, int32_t max) {
    if (!hao_net_ensure()) return NULL;
    hao_sock_t s = hao_net_load(unit);
    if (s == HAO_INVALID_SOCKET || max <= 0) return NULL;
    if (max > INT32_MAX - 1) max = INT32_MAX - 1;
    HaoString* buf = hao_str_alloc(max);
    hao_gc_add_root(buf);
    hao_gc_os_block_enter();
    int n = g_ws.recvfrom(s, buf->data, (int)max, 0, NULL, NULL);
    hao_gc_os_block_leave();
    hao_gc_remove_root(buf);
    if (n < 0) return NULL;
    buf->data[n] = '\0';
    buf->len = n;
    return buf;
}

#else
/* ============================================================
 *  POSIX?BSD socket?musl ???/ libc????????
 * ============================================================ */

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

typedef int hao_sock_t;
#define HAO_INVALID_SOCKET ((hao_sock_t)-1)
#define HAO_CLOSESOCKET(s) close(s)

static void hao_net_store(int64_t* unit, hao_sock_t s) { *unit = (int64_t)s; }
static hao_sock_t hao_net_load(const int64_t* unit) { return (hao_sock_t)*unit; }

static void hao_net_unit_finalize(void* user) {
    int64_t* unit = (int64_t*)user;
    if (!unit) return;
    hao_sock_t s = hao_net_load(unit);
    if (s != 0 && s != HAO_INVALID_SOCKET)
        HAO_CLOSESOCKET(s);
    hao_net_store(unit, HAO_INVALID_SOCKET);
}

static void hao_net_attach(int64_t* unit, hao_sock_t s) {
    if (!unit) return;
    hao_sock_t old = hao_net_load(unit);
    if (old != 0 && old != HAO_INVALID_SOCKET)
        HAO_CLOSESOCKET(old);
    hao_gc_clear_finalizer(unit);
    hao_net_store(unit, s);
    if (s != 0 && s != HAO_INVALID_SOCKET)
        hao_gc_set_finalizer(unit, hao_net_unit_finalize);
}

int8_t hao_net_connect(int64_t* unit, HaoString* host, int32_t port) {
    if (!host) return 0;
    char portstr[16];
    hao_net_portstr(portstr, sizeof(portstr), port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hao_gc_add_root(host);
    hao_gc_os_block_enter();
    if (getaddrinfo(host->data, portstr, &hints, &res) != 0) {
        hao_gc_os_block_leave();
        hao_gc_remove_root(host);
        return 0;
    }
    hao_sock_t s = HAO_INVALID_SOCKET;
    for (struct addrinfo* p = res; p; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == HAO_INVALID_SOCKET) continue;
        if (connect(s, p->ai_addr, (int)p->ai_addrlen) == 0) break;
        HAO_CLOSESOCKET(s);
        s = HAO_INVALID_SOCKET;
    }
    freeaddrinfo(res);
    hao_gc_os_block_leave();
    hao_gc_remove_root(host);
    if (s == HAO_INVALID_SOCKET) return 0;
    hao_net_attach(unit, s);
    return 1;
}

int8_t hao_net_listen_backlog(int64_t* unit, int32_t port, int32_t backlog);

int8_t hao_net_listen(int64_t* unit, int32_t port) {
    return hao_net_listen_backlog(unit, port, 128);
}

int8_t hao_net_listen_backlog(int64_t* unit, int32_t port, int32_t backlog) {
    hao_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == HAO_INVALID_SOCKET) return 0;
    if (backlog <= 0) backlog = 128;
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = hao_htonl(INADDR_ANY);
    addr.sin_port = hao_htons((uint16_t)port);
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(s, backlog) != 0) {
        HAO_CLOSESOCKET(s);
        return 0;
    }
    hao_net_attach(unit, s);
    return 1;
}

int8_t hao_net_set_recv_timeout(int64_t* unit, int32_t timeout_ms) {
    if (!unit) return 0;
    hao_sock_t s = hao_net_load(unit);
    if (s == HAO_INVALID_SOCKET) return 0;
    struct timeval tv;
    if (timeout_ms < 0) timeout_ms = 0;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)) != 0)
        return 0;
    return 1;
}

int8_t hao_net_set_send_timeout(int64_t* unit, int32_t timeout_ms) {
    if (!unit) return 0;
    hao_sock_t s = hao_net_load(unit);
    if (s == HAO_INVALID_SOCKET) return 0;
    struct timeval tv;
    if (timeout_ms < 0) timeout_ms = 0;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv)) != 0)
        return 0;
    return 1;
}

int8_t hao_net_set_timeout(int64_t* unit, int32_t timeout_ms) {
    return hao_net_set_recv_timeout(unit, timeout_ms)
        && hao_net_set_send_timeout(unit, timeout_ms);
}

int8_t hao_net_accept(int64_t* srv, int64_t* cli) {
    hao_sock_t sv = hao_net_load(srv);
    if (sv == HAO_INVALID_SOCKET) return 0;
    hao_gc_os_block_enter();
    hao_sock_t c = accept(sv, NULL, NULL);
    hao_gc_os_block_leave();
    if (c == HAO_INVALID_SOCKET) return 0;
    hao_net_attach(cli, c);
    return 1;
}

int32_t hao_net_send(int64_t* unit, HaoString* data) {
    hao_sock_t s = hao_net_load(unit);
    if (s == HAO_INVALID_SOCKET || !data) return -1;
    size_t total = (size_t)data->len;
    size_t sent = 0;
    hao_gc_add_root(data);
    hao_gc_os_block_enter();
    while (sent < total) {
        int n = (int)send(s, data->data + sent, (int)(total - sent), 0);
        if (n <= 0) {
            hao_gc_os_block_leave();
            hao_gc_remove_root(data);
            return -1;
        }
        sent += (size_t)n;
    }
    hao_gc_os_block_leave();
    hao_gc_remove_root(data);
    return (int32_t)sent;
}

HaoString* hao_net_recv(int64_t* unit, int32_t max) {
    hao_sock_t s = hao_net_load(unit);
    if (s == HAO_INVALID_SOCKET || max <= 0) return NULL;
    if (max > INT32_MAX - 1) max = INT32_MAX - 1;
    HaoString* buf = hao_str_alloc(max);
    hao_gc_add_root(buf);
    hao_gc_os_block_enter();
    int n = (int)recv(s, buf->data, (int)max, 0);
    hao_gc_os_block_leave();
    hao_gc_remove_root(buf);
    if (n < 0) return NULL;
    if (n == 0) {
        buf->data[0] = '\0';
        buf->len = 0;
        return buf;
    }
    buf->data[n] = '\0';
    buf->len = n;
    return buf;
}

int32_t hao_net_close(int64_t* unit) {
    if (!unit) return 0;
    hao_gc_clear_finalizer(unit);
    hao_sock_t s = hao_net_load(unit);
    if (s != 0 && s != HAO_INVALID_SOCKET)
        HAO_CLOSESOCKET(s);
    hao_net_store(unit, HAO_INVALID_SOCKET);
    return 1;
}

int8_t hao_net_udp_open(int64_t* unit) {
    hao_sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == HAO_INVALID_SOCKET) return 0;
    hao_net_attach(unit, s);
    return 1;
}

int8_t hao_net_udp_bind(int64_t* unit, int32_t port) {
    hao_sock_t s = hao_net_load(unit);
    if (s == HAO_INVALID_SOCKET) return 0;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = hao_htonl(INADDR_ANY);
    addr.sin_port = hao_htons((uint16_t)port);
    return bind(s, (struct sockaddr*)&addr, sizeof(addr)) == 0 ? 1 : 0;
}

int32_t hao_net_udp_sendto(int64_t* unit, HaoString* host, int32_t port,
                           HaoString* data) {
    hao_sock_t s = hao_net_load(unit);
    if (s == HAO_INVALID_SOCKET || !host || !data) return -1;
    char portstr[16];
    hao_net_portstr(portstr, sizeof(portstr), port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hao_gc_add_root(host);
    hao_gc_add_root(data);
    hao_gc_os_block_enter();
    if (getaddrinfo(host->data, portstr, &hints, &res) != 0) {
        hao_gc_os_block_leave();
        hao_gc_remove_root(host);
        hao_gc_remove_root(data);
        return -1;
    }
    int64_t sent = -1;
    for (struct addrinfo* p = res; p; p = p->ai_next) {
        int n = (int)sendto(s, data->data, (int)data->len, 0,
                            p->ai_addr, (int)p->ai_addrlen);
        if (n >= 0) { sent = (int64_t)n; break; }
    }
    freeaddrinfo(res);
    hao_gc_os_block_leave();
    hao_gc_remove_root(host);
    hao_gc_remove_root(data);
    return sent;
}

HaoString* hao_net_udp_recvfrom(int64_t* unit, int32_t max) {
    hao_sock_t s = hao_net_load(unit);
    if (s == HAO_INVALID_SOCKET || max <= 0) return NULL;
    if (max > INT32_MAX - 1) max = INT32_MAX - 1;
    HaoString* buf = hao_str_alloc(max);
    hao_gc_add_root(buf);
    hao_gc_os_block_enter();
    int n = (int)recvfrom(s, buf->data, (int)max, 0, NULL, NULL);
    hao_gc_os_block_leave();
    hao_gc_remove_root(buf);
    if (n < 0) return NULL;
    buf->data[n] = '\0';
    buf->len = n;
    return buf;
}

#endif /* _WIN32 */
