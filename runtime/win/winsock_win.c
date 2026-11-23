/* nfs5-web WEB2n: the game's Winsock 1.1 (WSOCK32.dll, by ordinal) on host sockets.
 *
 * Guest SOCKETs are small handles (0x100 + index in a table), the structures the game
 * passes (sockaddr_in, fd_set with 32-bit sockets, timeval) have the same layout, buffers
 * are used in place in guest memory.  A call that can block runs without the guest lock.
 * Winsock 1 numbers its IPPROTO_IP options differently from Winsock 2: mapped below.
 * NETLOG=1 logs every call except the frequent poll calls; NETLOG=2 all of them.
 * The browser build will have its own version of this file (relay over a WebSocket). */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void guest_wait_begin(void); void guest_wait_end(void);
void mem_commit(uint32_t, uint32_t);
uint32_t gheap_alloc(int heap, uint32_t n, int zero);
void nettest_hook(CPU *c, uint32_t ip);

#define NSOCK 256
#define SOCK_BASE 0x100u
typedef struct { SOCKET s; int used, nonblock; uint64_t tx, rx, txn, rxn; int type; } GS;
static GS g_s[NSOCK];
static volatile long g_started;              /* WSAStartup - WSACleanup */
static __thread int tl_err;
static uint64_t g_calls, g_tx, g_rx;
static int g_log = -1;

static uint32_t sk_new(SOCKET s, int type){
    for (int i = 0; i < NSOCK; i++) if (!g_s[i].used){ memset(&g_s[i], 0, sizeof g_s[i]); g_s[i].s = s; g_s[i].used = 1; g_s[i].type = type; return SOCK_BASE + (uint32_t)i; }
    closesocket(s); return 0xFFFFFFFFu;
}
static GS *sk(uint32_t h){ uint32_t i = h - SOCK_BASE; return (i < NSOCK && g_s[i].used) ? &g_s[i] : 0; }
static uint32_t find_host(SOCKET s){ for (int i = 0; i < NSOCK; i++) if (g_s[i].used && g_s[i].s == s) return SOCK_BASE + (uint32_t)i; return 0; }

#define A(i) MR32(c->esp + 4u * (uint32_t)(i))
#define P(p) ((p) ? (void*)(g_mem + (p)) : NULL)
#define RET_ERR(v) do { tl_err = WSAGetLastError(); *r = (v); return 1; } while (0)
#define FAIL_NOTSOCK(v) do { tl_err = WSAENOTSOCK; *r = (v); return 1; } while (0)

static int ip_opt_v1(int o){ switch (o){ case 2: return 9; case 3: return 10; case 4: return 11; case 5: return 12; case 6: return 13; case 7: return 4; case 8: return 3; case 9: return 14; } return o; }

static void addr_text(uint32_t sa, char *b, size_t n){
    if (!sa){ snprintf(b, n, "-"); return; }
    const uint8_t *p = g_mem + sa;
    snprintf(b, n, "%u.%u.%u.%u:%u", p[4], p[5], p[6], p[7], (p[2] << 8) | p[3]);
}

/* guest fd_set <-> host fd_set */
static void fds_in(uint32_t g, fd_set *h){ FD_ZERO(h); if (!g) return; uint32_t n = MR32(g); if (n > 64u) n = 64u;
    for (uint32_t i = 0; i < n; i++){ GS *s = sk(MR32(g + 4u + 4u * i)); if (s) FD_SET(s->s, h); } }
static void fds_out(uint32_t g, const fd_set *h){ if (!g) return; uint32_t k = 0;
    for (u_int i = 0; i < h->fd_count && k < 64u; i++){ uint32_t x = find_host(h->fd_array[i]); if (x) MR32(g + 4u + 4u * k++) = x; } MR32(g) = k; }

static uint32_t guest_string(const char *t){ size_t n = strlen(t) + 1; uint32_t p = gheap_alloc(1, (uint32_t)n, 1); memcpy(g_mem + p, t, n); return p; }

int winsock_call(CPU *c, const char *dll, const char *n, uint32_t *r){
    if (strcmp(dll, "WSOCK32.dll")) return 0;
    if (g_log < 0){ const char *e = getenv("NETLOG"); g_log = e ? atoi(e) : 0; }
    g_calls++;
    if (g_log >= 2 || (g_log == 1 && strcmp(n, "select") && strcmp(n, "recvfrom") && strcmp(n, "sendto") && strcmp(n, "htons") && strcmp(n, "ntohs")
                       && strcmp(n, "htonl") && strcmp(n, "ntohl") && strcmp(n, "__WSAFDIsSet") && strcmp(n, "WSAGetLastError")))
        fprintf(stderr, "[net] %s(0x%x, 0x%x, 0x%x)\n", n, A(0), A(1), A(2));

    if (!strcmp(n, "htons") || !strcmp(n, "ntohs")){ uint32_t v = A(0) & 0xFFFFu; *r = ((v >> 8) | (v << 8)) & 0xFFFFu; return 1; }
    if (!strcmp(n, "htonl") || !strcmp(n, "ntohl")){ *r = __builtin_bswap32(A(0)); return 1; }
    if (!strcmp(n, "WSAGetLastError")){ *r = (uint32_t)tl_err; return 1; }
    if (!strcmp(n, "WSAStartup")){
        nettest_hook(c, 0);
        WSADATA d; int e = WSAStartup(MAKEWORD(2, 2), &d);
        uint32_t p = A(1);
        if (p){ mem_commit(p, 400); memset(g_mem + p, 0, 400); MR16(p) = (uint16_t)A(0); MR16(p + 2) = 0x0101u;
                strcpy((char*)g_mem + p + 4, "WinSock 1.1"); strcpy((char*)g_mem + p + 261, "Running");
                MR16(p + 390) = 256; MR16(p + 392) = 65467; }
        if (!e) __atomic_add_fetch(&g_started, 1, __ATOMIC_SEQ_CST);
        *r = (uint32_t)e; return 1; }
    if (!strcmp(n, "WSACleanup")){ WSACleanup(); if (g_started > 0) __atomic_sub_fetch(&g_started, 1, __ATOMIC_SEQ_CST); *r = 0; return 1; }
    if (!strcmp(n, "socket")){
        SOCKET s = socket((int)A(0), (int)A(1), (int)A(2));
        if (s == INVALID_SOCKET) RET_ERR(0xFFFFFFFFu);
        *r = sk_new(s, (int)A(1)); return 1; }
    if (!strcmp(n, "closesocket")){ GS *s = sk(A(0)); if (!s) FAIL_NOTSOCK(0xFFFFFFFFu); int e = closesocket(s->s); s->used = 0; if (e) RET_ERR(0xFFFFFFFFu); *r = 0; return 1; }
    if (!strcmp(n, "bind")){ GS *s = sk(A(0)); if (!s) FAIL_NOTSOCK(0xFFFFFFFFu);
        char t[32]; addr_text(A(1), t, sizeof t);
        int e = bind(s->s, (struct sockaddr*)P(A(1)), (int)A(2));
        if (g_log) fprintf(stderr, "[net] bind %s -> %d\n", t, e ? WSAGetLastError() : 0);
        if (e) RET_ERR(0xFFFFFFFFu); *r = 0; return 1; }
    if (!strcmp(n, "listen")){ GS *s = sk(A(0)); if (!s) FAIL_NOTSOCK(0xFFFFFFFFu); if (listen(s->s, (int)A(1))) RET_ERR(0xFFFFFFFFu); *r = 0; return 1; }
    if (!strcmp(n, "connect")){ GS *s = sk(A(0)); if (!s) FAIL_NOTSOCK(0xFFFFFFFFu);
        char t[32]; addr_text(A(1), t, sizeof t);
        guest_wait_begin(); int e = connect(s->s, (struct sockaddr*)P(A(1)), (int)A(2)); int err = WSAGetLastError(); guest_wait_end();
        if (g_log) fprintf(stderr, "[net] connect %s -> %d\n", t, e ? err : 0);
        if (e){ tl_err = err; *r = 0xFFFFFFFFu; return 1; } *r = 0; return 1; }
    if (!strcmp(n, "accept")){ GS *s = sk(A(0)); if (!s) FAIL_NOTSOCK(0xFFFFFFFFu);
        int len = A(2) ? (int)MR32(A(2)) : 0;
        if (!s->nonblock) guest_wait_begin();
        SOCKET a = accept(s->s, (struct sockaddr*)P(A(1)), A(2) ? &len : NULL); int err = WSAGetLastError();
        if (!s->nonblock) guest_wait_end();
        if (a == INVALID_SOCKET){ tl_err = err; *r = 0xFFFFFFFFu; return 1; }
        if (A(2)) MR32(A(2)) = (uint32_t)len;
        *r = sk_new(a, SOCK_STREAM); return 1; }
    if (!strcmp(n, "send")){ GS *s = sk(A(0)); if (!s) FAIL_NOTSOCK(0xFFFFFFFFu);
        int k = send(s->s, (const char*)P(A(1)), (int)A(2), (int)A(3));
        if (k < 0) RET_ERR(0xFFFFFFFFu); s->tx += (uint64_t)k; s->txn++; g_tx += (uint64_t)k; *r = (uint32_t)k; return 1; }
    if (!strcmp(n, "sendto")){ GS *s = sk(A(0)); if (!s) FAIL_NOTSOCK(0xFFFFFFFFu);
        int k = sendto(s->s, (const char*)P(A(1)), (int)A(2), (int)A(3), (struct sockaddr*)P(A(4)), (int)A(5));
        if (g_log >= 2){ char t[32]; addr_text(A(4), t, sizeof t); fprintf(stderr, "[net] sendto %s %u bytes -> %d\n", t, A(2), k < 0 ? -WSAGetLastError() : k); }
        if (k < 0) RET_ERR(0xFFFFFFFFu); s->tx += (uint64_t)k; s->txn++; g_tx += (uint64_t)k; *r = (uint32_t)k; return 1; }
    if (!strcmp(n, "recv") || !strcmp(n, "recvfrom")){ GS *s = sk(A(0)); if (!s) FAIL_NOTSOCK(0xFFFFFFFFu);
        const int from = n[4] != 0;
        uint32_t pf = from ? A(4) : 0, pl = from ? A(5) : 0;
        int len = pl ? (int)MR32(pl) : 0, k, err;
        if (!s->nonblock) guest_wait_begin();
        k = from ? recvfrom(s->s, (char*)P(A(1)), (int)A(2), (int)A(3), (struct sockaddr*)P(pf), pl ? &len : NULL)
                 : recv(s->s, (char*)P(A(1)), (int)A(2), (int)A(3));
        err = WSAGetLastError();
        if (!s->nonblock) guest_wait_end();
        if (k < 0){ tl_err = err; *r = 0xFFFFFFFFu; return 1; }
        if (pl) MR32(pl) = (uint32_t)len;
        if (g_log >= 2){ char t[32]; addr_text(pf, t, sizeof t); fprintf(stderr, "[net] recv from %s %d bytes\n", t, k); }
        s->rx += (uint64_t)k; s->rxn++; g_rx += (uint64_t)k; *r = (uint32_t)k; return 1; }
    if (!strcmp(n, "select")){
        fd_set rs, ws, es; fds_in(A(1), &rs); fds_in(A(2), &ws); fds_in(A(3), &es);
        struct timeval tv, *ptv = NULL; uint32_t pt = A(4);
        if (pt){ tv.tv_sec = (long)MR32(pt); tv.tv_usec = (long)MR32(pt + 4); ptv = &tv; }
        const int blocks = !ptv || tv.tv_sec || tv.tv_usec;
        /* an empty select is an error on Windows */
        if (!rs.fd_count && !ws.fd_count && !es.fd_count){ *r = 0; return 1; }
        if (blocks) guest_wait_begin();
        int k = select(0, A(1) ? &rs : NULL, A(2) ? &ws : NULL, A(3) ? &es : NULL, ptv); int err = WSAGetLastError();
        if (blocks) guest_wait_end();
        if (k < 0){ tl_err = err; *r = 0xFFFFFFFFu; return 1; }
        fds_out(A(1), &rs); fds_out(A(2), &ws); fds_out(A(3), &es);
        *r = (uint32_t)k; return 1; }
    if (!strcmp(n, "__WSAFDIsSet")){ uint32_t g = A(1), k = g ? MR32(g) : 0; *r = 0; if (k > 64u) k = 64u;
        for (uint32_t i = 0; i < k; i++) if (MR32(g + 4u + 4u * i) == A(0)){ *r = 1; break; } return 1; }
    if (!strcmp(n, "ioctlsocket")){ GS *s = sk(A(0)); if (!s) FAIL_NOTSOCK(0xFFFFFFFFu);
        u_long v = A(2) ? MR32(A(2)) : 0;
        if (ioctlsocket(s->s, (long)A(1), &v)) RET_ERR(0xFFFFFFFFu);
        if (A(1) == 0x8004667Eu) s->nonblock = v != 0;   /* FIONBIO */
        if (A(2)) MR32(A(2)) = (uint32_t)v;
        *r = 0; return 1; }
    if (!strcmp(n, "setsockopt") || !strcmp(n, "getsockopt")){ GS *s = sk(A(0)); if (!s) FAIL_NOTSOCK(0xFFFFFFFFu);
        int lvl = (int)A(1), opt = (int)A(2); if (lvl == IPPROTO_IP) opt = ip_opt_v1(opt);
        int e;
        if (n[0] == 's') e = setsockopt(s->s, lvl, opt, (const char*)P(A(3)), (int)A(4));
        else { int len = A(4) ? (int)MR32(A(4)) : 0; e = getsockopt(s->s, lvl, opt, (char*)P(A(3)), &len); if (!e && A(4)) MR32(A(4)) = (uint32_t)len; }
        if (e) RET_ERR(0xFFFFFFFFu); *r = 0; return 1; }
    if (!strcmp(n, "getsockname")){ GS *s = sk(A(0)); if (!s) FAIL_NOTSOCK(0xFFFFFFFFu);
        int len = A(2) ? (int)MR32(A(2)) : 0;
        if (getsockname(s->s, (struct sockaddr*)P(A(1)), &len)) RET_ERR(0xFFFFFFFFu);
        if (A(2)) MR32(A(2)) = (uint32_t)len; *r = 0; return 1; }
    if (!strcmp(n, "shutdown")){ GS *s = sk(A(0)); if (!s) FAIL_NOTSOCK(0xFFFFFFFFu); if (shutdown(s->s, (int)A(1))) RET_ERR(0xFFFFFFFFu); *r = 0; return 1; }
    if (!strcmp(n, "inet_addr")){ *r = inet_addr((const char*)P(A(0))); return 1; }
    if (!strcmp(n, "inet_ntoa")){ static uint32_t buf; if (!buf) buf = gheap_alloc(1, 32, 1);
        uint32_t v = A(0); snprintf((char*)g_mem + buf, 32, "%u.%u.%u.%u", v & 255u, (v >> 8) & 255u, (v >> 16) & 255u, v >> 24); *r = buf; return 1; }
    if (!strcmp(n, "gethostname")){ if (gethostname((char*)P(A(0)), (int)A(1))) RET_ERR(0xFFFFFFFFu); *r = 0; return 1; }
    if (!strcmp(n, "gethostbyname")){
        char name[256]; strncpy(name, (const char*)P(A(0)), sizeof name - 1); name[sizeof name - 1] = 0;
        guest_wait_begin(); struct hostent *h = gethostbyname(name); int err = WSAGetLastError(); guest_wait_end();
        if (g_log) fprintf(stderr, "[net] gethostbyname(\"%s\") -> %s\n", name, h ? "ok" : "failed");
        if (!h){ tl_err = err; *r = 0; return 1; }
        /* struct hostent in guest memory: one block per name lookup, few of them in a game */
        int na = 0; while (h->h_addr_list[na] && na < 8) na++;
        uint32_t g = gheap_alloc(1, 16u + 4u + 4u * ((uint32_t)na + 1u) + 4u * (uint32_t)na, 1);
        uint32_t al = g + 20u, ad = al + 4u * ((uint32_t)na + 1u);
        MR32(g) = guest_string(h->h_name); MR32(g + 4) = g + 16u; MR16(g + 8) = (uint16_t)h->h_addrtype; MR16(g + 10) = 4; MR32(g + 12) = al;
        for (int i = 0; i < na; i++){ memcpy(g_mem + ad + 4u * (uint32_t)i, h->h_addr_list[i], 4); MR32(al + 4u * (uint32_t)i) = ad + 4u * (uint32_t)i; }
        *r = g; return 1; }
    return 0;
}
int winsock_session_active(void){ return g_started > 0; }
void winsock_report(void){
    if (!g_calls) return;
    fprintf(stderr, "\n=== WINSOCK: %llu calls, sent %llu bytes, received %llu bytes ===\n", (unsigned long long)g_calls, (unsigned long long)g_tx, (unsigned long long)g_rx);
    for (int i = 0; i < NSOCK; i++) if (g_s[i].used && (g_s[i].txn || g_s[i].rxn))
        fprintf(stderr, "    socket 0x%x: %llu packets sent (%llu B), %llu received (%llu B)\n", SOCK_BASE + (uint32_t)i,
                (unsigned long long)g_s[i].txn, (unsigned long long)g_s[i].tx, (unsigned long long)g_s[i].rxn, (unsigned long long)g_s[i].rx);
}

/* ON2: the browser build's control line to the server (runtime/web/winsock_web.c); none in the native build */
int online_ctl_recv(char *out, int max){ (void)out; (void)max; return 0; }
int online_ctl_send(const char *text){ (void)text; return 0; }
