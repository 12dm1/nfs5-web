/* nfs5-web ON1: the game's Winsock 1.1 (WSOCK32.dll, by ordinal) in the browser, on a virtual network.
 *
 * The browser has no sockets, so the game's datagrams go through the LAN mailbox (net_proto.h, after nfsu2-web) to the
 * page (web/src/net/lan.ts) and over one WebSocket to the server's switch (web/online/vnet.mjs).  The page gets a
 * virtual address 10.77.x.y there.  What the game needs for the Internet mode:
 *   - UDP (SOCK_DGRAM): every datagram becomes a NET_KIND_UDP frame; the switch hands the ones for the entry port to
 *     the NetGather server (web/online/netgather.mjs) and the others to the virtual address they are for.
 *   - names: gethostbyname of any name but our own gives the server's address NET_SERVER_VIP (the game resolves
 *     porsche-entry.earacing.com, the registry's "Primary Address"; no patch needed as on the PC);
 *   - ICMP (SOCK_RAW, IPPROTO_ICMP): the server-list rows are pinged; an echo goes to the switch as NET_KIND_LAN_ICMP,
 *     the server answers it, recvfrom gives an IPv4 header + the ICMP message as a raw socket does on Windows;
 *   - TCP: the client's fallback connect is refused at once (WSAECONNREFUSED): everything goes over UDP.
 * Guest SOCKETs are small handles as in runtime/win/winsock_win.c.  Received frames are taken from the RX ring into
 * per-socket queues while the guest lock is held (one consumer); a call that waits releases the lock and waits on
 * rx_bell.  NETLOG=1 logs the calls (except the frequent ones), NETLOG=2 every datagram. */
#include "cpu.h"
#include "host_abi.h"
#include "net_proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <strings.h>

void guest_wait_begin(void); void guest_wait_end(void);
void mem_commit(uint32_t, uint32_t);
uint32_t gheap_alloc(int heap, uint32_t n, int zero);

#define NET_SERVER_VIP 0x0A4D0001u          /* 10.77.0.1: the entry server (vnet.mjs SERVER_VIP) */

/* Winsock numbers */
enum { W_EWOULDBLOCK = 10035, W_ENOTSOCK = 10038, W_EMSGSIZE = 10040, W_EAFNOSUPPORT = 10047, W_EADDRINUSE = 10048,
       W_ENETDOWN = 10050, W_ENOTCONN = 10057, W_ECONNREFUSED = 10061, W_EINVAL = 10022, W_HOST_NOT_FOUND = 11001,
       W_EOPNOTSUPP = 10045 };
enum { T_STREAM = 1, T_DGRAM = 2, T_RAW = 3 };
#define FIONBIO_  0x8004667Eu
#define FIONREAD_ 0x4004667Fu

/* the mailbox: in the runtime's own memory, registered once */
static uint8_t g_mbox_mem[NET_MAILBOX_BYTES] __attribute__((aligned(64)));
static net_mailbox *const g_mb = (net_mailbox *)g_mbox_mem;
static int g_registered;

#define QLEN 64
typedef struct { uint32_t ip; uint16_t port; uint16_t len; uint8_t *data; } Dgram;
typedef struct {
    int used, type, nonblock, bound, refused;
    uint16_t port;
    Dgram q[QLEN]; uint32_t qh, qt;       /* received, not yet read */
    uint64_t tx, rx, txn, rxn, drop;
} GS;
#define NSOCK 64
#define SOCK_BASE 0x100u
static GS g_s[NSOCK];
static volatile long g_started;
static __thread int tl_err;
static uint64_t g_calls, g_tx, g_rx;
static int g_log = -1;
static uint16_t g_next_port = 49152;

static double now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }
static uint32_t ld(volatile uint32_t *p){ return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void st(volatile uint32_t *p, uint32_t v){ __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static uint32_t vip(void){ uint32_t v = ld(&g_mb->vip); return v ? v : 0x0A4DFFFEu; }

static void mbox_init(void){
    if (g_registered) return;
    memset(g_mbox_mem, 0, sizeof g_mbox_mem);
    g_mb->magic = NET_MAGIC; g_mb->version = NET_VERSION; g_mb->hdr_bytes = NET_HDR_BYTES; g_mb->slot_bytes = NET_SLOT_BYTES;
    g_mb->tx_slots = NET_TX_SLOTS; g_mb->rx_slots = NET_RX_SLOTS;
    g_mb->tx_off = NET_HDR_BYTES; g_mb->rx_off = NET_HDR_BYTES + NET_TX_SLOTS * NET_SLOT_BYTES;
    g_mb->mode = NET_MODE_LAN;
    nfsu2_register(HOST_KIND_LAN, g_mbox_mem, NET_MAILBOX_BYTES);
    g_registered = 1;
}
static net_slot *tx_slot(uint32_t i){ return (net_slot *)(g_mbox_mem + NET_HDR_BYTES + (i % NET_TX_SLOTS) * NET_SLOT_BYTES); }
static net_slot *rx_slot(uint32_t i){ return (net_slot *)(g_mbox_mem + g_mb->rx_off + (i % NET_RX_SLOTS) * NET_SLOT_BYTES); }

/* queue a frame for the page; 0 when the ring is full or the page is not there */
static int tx_frame(uint32_t kind, uint32_t dst_ip, uint16_t src_port, uint16_t dst_port, const void *p, uint32_t n){
    if (n > NET_MAX_PAYLOAD) { tl_err = W_EMSGSIZE; return 0; }
    uint32_t head = g_mb->tx_head;
    if (head - ld(&g_mb->tx_tail) >= NET_TX_SLOTS){ g_mb->tx_offline_drop++; return 1; }    /* as a lost datagram */
    net_slot *s = tx_slot(head);
    s->len = n; s->kind = kind; s->src_ip = vip(); s->dst_ip = dst_ip; s->src_port = src_port; s->dst_port = dst_port;
    s->t_ms = 0;
    memcpy(s->data, p, n);
    st(&g_mb->tx_head, head + 1);
    __atomic_add_fetch(&g_mb->tx_bell, 1, __ATOMIC_RELEASE);
    __builtin_wasm_memory_atomic_notify((int *)&g_mb->tx_bell, 1);
    return 1;
}

static void q_push(GS *s, uint32_t ip, uint16_t port, const uint8_t *p, uint32_t n){
    if (s->qh - s->qt >= QLEN){ s->drop++; return; }
    Dgram *d = &s->q[s->qh % QLEN];
    d->data = malloc(n ? n : 1); memcpy(d->data, p, n); d->len = (uint16_t)n; d->ip = ip; d->port = port;
    s->qh++;
}
/* ICMP message -> what a raw socket reads: a 20-byte IPv4 header first */
static void q_push_icmp(GS *s, uint32_t from, const uint8_t *p, uint32_t n){
    uint8_t b[NET_MAX_PAYLOAD + 20];
    if (n > NET_MAX_PAYLOAD) return;
    memset(b, 0, 20);
    b[0] = 0x45; b[2] = (uint8_t)((n + 20) >> 8); b[3] = (uint8_t)(n + 20); b[8] = 64; b[9] = 1;
    b[12] = (uint8_t)(from >> 24); b[13] = (uint8_t)(from >> 16); b[14] = (uint8_t)(from >> 8); b[15] = (uint8_t)from;
    uint32_t me = vip(); b[16] = (uint8_t)(me >> 24); b[17] = (uint8_t)(me >> 16); b[18] = (uint8_t)(me >> 8); b[19] = (uint8_t)me;
    memcpy(b + 20, p, n);
    q_push(s, from, 0, b, n + 20);
}

/* take what the page received into the sockets' queues (guest lock held) */
/* ON2: a text line channel between the runtime (nettest.c online_auto) and the server (web/online/vnet.mjs), UDP port
 * 9443 on both sides; no game socket uses it */
#define CTL_PORT 9443
#define CTL_N 16
#define CTL_LEN 512
static char g_ctl[CTL_N][CTL_LEN];
static uint32_t g_ctl_h, g_ctl_t;
static void drain(void);
int online_ctl_recv(char *out, int max){
    drain();
    if (g_ctl_t == g_ctl_h || max <= 0) return 0;
    snprintf(out, (size_t)max, "%s", g_ctl[g_ctl_t % CTL_N]); g_ctl_t++;
    return 1;
}
int online_ctl_send(const char *text){
    mbox_init();
    return tx_frame(NET_KIND_UDP, NET_SERVER_VIP, CTL_PORT, CTL_PORT, text, (uint32_t)strlen(text));
}
static void drain(void){
    if (!g_registered) return;
    uint32_t tail = g_mb->rx_tail, head = ld(&g_mb->rx_head);
    for (; tail != head; tail++){
        const net_slot *f = rx_slot(tail);
        uint32_t n = f->len; if (n > NET_MAX_PAYLOAD) n = 0;
        if (f->kind == NET_KIND_UDP && f->dst_port == CTL_PORT){       /* ON2: the server's control line for the runtime */
            if (g_ctl_h - g_ctl_t < CTL_N){ uint32_t k = n < CTL_LEN - 1 ? n : CTL_LEN - 1;
                memcpy(g_ctl[g_ctl_h % CTL_N], f->data, k); g_ctl[g_ctl_h % CTL_N][k] = 0; g_ctl_h++; }
            continue;
        }
        if (f->kind == NET_KIND_UDP){
            for (int i = 0; i < NSOCK; i++) if (g_s[i].used && g_s[i].type == T_DGRAM && g_s[i].bound && g_s[i].port == f->dst_port){
                q_push(&g_s[i], f->src_ip, f->src_port, f->data, n);
                if (g_log >= 2) fprintf(stderr, "[net] rx %u.%u.%u.%u:%u -> :%u %u bytes\n", f->src_ip >> 24, (f->src_ip >> 16) & 255, (f->src_ip >> 8) & 255, f->src_ip & 255, f->src_port, f->dst_port, n);
                break;
            }
        } else if (f->kind == NET_KIND_LAN_ICMP){
            for (int i = 0; i < NSOCK; i++) if (g_s[i].used && g_s[i].type == T_RAW) q_push_icmp(&g_s[i], f->src_ip, f->data, n);
        }
    }
    st(&g_mb->rx_tail, tail);
}
/* wait for frames from the page, at most ms (guest lock released) */
static void wait_rx(double ms){
    uint32_t bell = ld(&g_mb->rx_bell);
    if (ld(&g_mb->rx_head) != g_mb->rx_tail) return;
    if (ms > 50) ms = 50;      /* short waits: the caller looks at its deadline again */
    guest_wait_begin();
    __builtin_wasm_memory_atomic_wait32((int *)&g_mb->rx_bell, (int)bell, (long long)(ms * 1e6));
    guest_wait_end();
}

static uint32_t sk_new(int type){
    for (int i = 0; i < NSOCK; i++) if (!g_s[i].used){ memset(&g_s[i], 0, sizeof g_s[i]); g_s[i].used = 1; g_s[i].type = type; return SOCK_BASE + (uint32_t)i; }
    return 0xFFFFFFFFu;
}
static GS *sk(uint32_t h){ uint32_t i = h - SOCK_BASE; return (i < NSOCK && g_s[i].used) ? &g_s[i] : 0; }
static void sk_free(GS *s){ while (s->qt != s->qh){ free(s->q[s->qt % QLEN].data); s->qt++; } s->used = 0; }
static int port_used(uint16_t p, int type){ for (int i = 0; i < NSOCK; i++) if (g_s[i].used && g_s[i].bound && g_s[i].type == type && g_s[i].port == p) return 1; return 0; }
static void autobind(GS *s){
    if (s->bound) return;
    do { s->port = g_next_port++; if (g_next_port < 49152) g_next_port = 49152; } while (port_used(s->port, s->type));
    s->bound = 1;
}
static int readable(GS *s){ return s->qh != s->qt || (s->type == T_STREAM && s->refused); }

#define A(i) MR32(c->esp + 4u * (uint32_t)(i))
#define P(p) ((p) ? (void*)(g_mem + (p)) : NULL)
#define ERR(e, v) do { tl_err = (e); *r = (v); return 1; } while (0)
#define SOCK_OR_FAIL(s, h) GS *s = sk(h); if (!s) ERR(W_ENOTSOCK, 0xFFFFFFFFu)
/* sockaddr_in in guest memory: +0 u16 family, +2 port (network order), +4 address (network order) */
static uint32_t sa_ip(uint32_t sa){ const uint8_t *p = g_mem + sa; return (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7]; }
static uint16_t sa_port(uint32_t sa){ const uint8_t *p = g_mem + sa; return (uint16_t)(p[2] << 8 | p[3]); }
static void sa_put(uint32_t sa, uint32_t lenp, uint32_t ip, uint16_t port){
    if (!sa) return;
    uint8_t *p = g_mem + sa; memset(p, 0, 16);
    p[0] = 2; p[2] = (uint8_t)(port >> 8); p[3] = (uint8_t)port;
    p[4] = (uint8_t)(ip >> 24); p[5] = (uint8_t)(ip >> 16); p[6] = (uint8_t)(ip >> 8); p[7] = (uint8_t)ip;
    if (lenp) MR32(lenp) = 16;
}
static uint32_t guest_string(const char *t){ size_t n = strlen(t) + 1; uint32_t p = gheap_alloc(1, (uint32_t)n, 1); memcpy(g_mem + p, t, n); return p; }
static const char HOSTNAME[] = "nfs5web";

/* fd_set of the guest: u32 count + 64 u32 sockets */
static int fds_ready(uint32_t g, int which){
    if (!g) return 0;
    uint32_t n = MR32(g); if (n > 64u) n = 64u;
    int k = 0;
    for (uint32_t i = 0; i < n; i++){
        GS *s = sk(MR32(g + 4u + 4u * i));
        int ok = !s ? 0 : which == 0 ? readable(s) : which == 1 ? (s->type != T_STREAM || s->refused) : 0;
        if (ok) k++;
    }
    return k;
}
static void fds_keep(uint32_t g, int which){
    if (!g) return;
    uint32_t n = MR32(g); if (n > 64u) n = 64u;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++){
        uint32_t h = MR32(g + 4u + 4u * i); GS *s = sk(h);
        int ok = !s ? 0 : which == 0 ? readable(s) : which == 1 ? (s->type != T_STREAM || s->refused) : 0;
        if (ok) MR32(g + 4u + 4u * k++) = h;
    }
    MR32(g) = k;
}

static int icmp_call(CPU *c, const char *n, uint32_t *r);
int winsock_call(CPU *c, const char *dll, const char *n, uint32_t *r){
    if (!strcasecmp(dll, "ICMP.DLL")) return icmp_call(c, n, r);
    if (strcmp(dll, "WSOCK32.dll")) return 0;
    if (g_log < 0){ const char *e = getenv("NETLOG"); g_log = e ? atoi(e) : 0; }
    g_calls++;
    if (g_log >= 2 || (g_log == 1 && strcmp(n, "select") && strcmp(n, "recvfrom") && strcmp(n, "sendto") && strcmp(n, "htons") && strcmp(n, "ntohs")
                       && strcmp(n, "htonl") && strcmp(n, "ntohl") && strcmp(n, "__WSAFDIsSet") && strcmp(n, "WSAGetLastError") && strcmp(n, "recv") && strcmp(n, "ioctlsocket")))
        fprintf(stderr, "[net] %s(0x%x, 0x%x, 0x%x)\n", n, A(0), A(1), A(2));

    if (!strcmp(n, "htons") || !strcmp(n, "ntohs")){ uint32_t v = A(0) & 0xFFFFu; *r = ((v >> 8) | (v << 8)) & 0xFFFFu; return 1; }
    if (!strcmp(n, "htonl") || !strcmp(n, "ntohl")){ *r = __builtin_bswap32(A(0)); return 1; }
    if (!strcmp(n, "WSAGetLastError")){ *r = (uint32_t)tl_err; return 1; }
    if (!strcmp(n, "WSAStartup")){
        mbox_init();
        uint32_t p = A(1);
        if (p){ mem_commit(p, 400); memset(g_mem + p, 0, 400); MR16(p) = (uint16_t)A(0); MR16(p + 2) = 0x0101u;
                strcpy((char*)g_mem + p + 4, "WinSock 1.1"); strcpy((char*)g_mem + p + 261, "Running");
                MR16(p + 390) = 256; MR16(p + 392) = 65467; }
        if (__atomic_add_fetch(&g_started, 1, __ATOMIC_SEQ_CST) == 1){
            st(&g_mb->want, 1); __atomic_add_fetch(&g_mb->tx_bell, 1, __ATOMIC_RELEASE); __builtin_wasm_memory_atomic_notify((int *)&g_mb->tx_bell, 1);
        }
        *r = 0; return 1; }
    if (!strcmp(n, "WSACleanup")){ if (g_started > 0) __atomic_sub_fetch(&g_started, 1, __ATOMIC_SEQ_CST); *r = 0; return 1; }
    mbox_init();
    drain();
    if (!strcmp(n, "socket")){
        int af = (int)A(0), type = (int)A(1);
        if (af != 2) ERR(W_EAFNOSUPPORT, 0xFFFFFFFFu);
        if (type != T_STREAM && type != T_DGRAM && type != T_RAW) ERR(W_EINVAL, 0xFFFFFFFFu);
        uint32_t h = sk_new(type);
        if (h == 0xFFFFFFFFu) ERR(10024, h);
        if (g_log) fprintf(stderr, "[net] socket type %d proto %d -> 0x%x\n", type, (int)A(2), h);
        *r = h; return 1; }
    if (!strcmp(n, "closesocket")){ SOCK_OR_FAIL(s, A(0)); sk_free(s); *r = 0; return 1; }
    if (!strcmp(n, "bind")){ SOCK_OR_FAIL(s, A(0));
        uint16_t port = A(1) ? sa_port(A(1)) : 0;
        if (port && port_used(port, s->type)) ERR(W_EADDRINUSE, 0xFFFFFFFFu);
        if (port){ s->port = port; s->bound = 1; } else autobind(s);
        if (g_log) fprintf(stderr, "[net] bind 0x%x -> port %u\n", A(0), s->port);
        *r = 0; return 1; }
    if (!strcmp(n, "listen")){ SOCK_OR_FAIL(s, A(0)); autobind(s); *r = 0; return 1; }
    if (!strcmp(n, "accept")){ SOCK_OR_FAIL(s, A(0)); ERR(W_EWOULDBLOCK, 0xFFFFFFFFu); }
    if (!strcmp(n, "connect")){ SOCK_OR_FAIL(s, A(0));
        if (s->type == T_STREAM){ s->refused = 1; if (g_log) fprintf(stderr, "[net] TCP connect refused (UDP only)\n"); ERR(W_ECONNREFUSED, 0xFFFFFFFFu); }
        *r = 0; return 1; }   /* a connected UDP socket is not used by the game */
    if (!strcmp(n, "send")){ SOCK_OR_FAIL(s, A(0)); ERR(W_ENOTCONN, 0xFFFFFFFFu); }
    if (!strcmp(n, "sendto")){ SOCK_OR_FAIL(s, A(0));
        uint32_t len = A(2), to = A(4);
        if (!to) ERR(W_ENOTCONN, 0xFFFFFFFFu);
        autobind(s);
        uint32_t ip = sa_ip(to); uint16_t port = sa_port(to);
        uint32_t kind = s->type == T_RAW ? NET_KIND_LAN_ICMP : NET_KIND_UDP;
        if (s->type == T_STREAM) ERR(W_ENOTCONN, 0xFFFFFFFFu);
        if (!tx_frame(kind, ip, s->port, port, P(A(1)), len)) ERR(W_EMSGSIZE, 0xFFFFFFFFu);
        if (g_log >= 2) fprintf(stderr, "[net] tx :%u -> %u.%u.%u.%u:%u %u bytes%s\n", s->port, ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port, len, kind == NET_KIND_LAN_ICMP ? " (icmp)" : "");
        s->tx += len; s->txn++; g_tx += len; *r = len; return 1; }
    if (!strcmp(n, "recv") || !strcmp(n, "recvfrom")){ SOCK_OR_FAIL(s, A(0));
        const int from = n[4] != 0;
        if (s->type == T_STREAM) ERR(W_ENOTCONN, 0xFFFFFFFFu);
        while (s->qh == s->qt){
            if (s->nonblock) ERR(W_EWOULDBLOCK, 0xFFFFFFFFu);
            wait_rx(50); drain();
        }
        Dgram *d = &s->q[s->qt % QLEN];
        uint32_t k = d->len < A(2) ? d->len : A(2);
        memcpy(P(A(1)), d->data, k);
        if (from) sa_put(A(4), A(5), d->ip, d->port);
        if (!(A(3) & 2)){ free(d->data); s->qt++; }        /* MSG_PEEK keeps it */
        if (k < d->len && !(A(3) & 2)) ERR(W_EMSGSIZE, 0xFFFFFFFFu);
        s->rx += k; s->rxn++; g_rx += k; *r = k; return 1; }
    if (!strcmp(n, "select")){
        uint32_t pt = A(4);
        double timeout = pt ? MR32(pt) * 1000.0 + MR32(pt + 4) / 1000.0 : 1e12, end = now_ms() + timeout;
        for (;;){
            int k = fds_ready(A(1), 0) + fds_ready(A(2), 1);
            if (k || now_ms() >= end){
                fds_keep(A(1), 0); fds_keep(A(2), 1); if (A(3)) MR32(A(3)) = 0;
                *r = (uint32_t)k; return 1;
            }
            wait_rx(end - now_ms()); drain();
        }
    }
    if (!strcmp(n, "__WSAFDIsSet")){ uint32_t g = A(1), k = g ? MR32(g) : 0; *r = 0; if (k > 64u) k = 64u;
        for (uint32_t i = 0; i < k; i++) if (MR32(g + 4u + 4u * i) == A(0)){ *r = 1; break; } return 1; }
    if (!strcmp(n, "ioctlsocket")){ SOCK_OR_FAIL(s, A(0));
        uint32_t cmd = A(1), p = A(2);
        if (cmd == FIONBIO_){ s->nonblock = p && MR32(p) != 0; *r = 0; return 1; }
        if (cmd == FIONREAD_){ if (p) MR32(p) = s->qh != s->qt ? s->q[s->qt % QLEN].len : 0; *r = 0; return 1; }
        ERR(W_EINVAL, 0xFFFFFFFFu); }
    if (!strcmp(n, "setsockopt")){ SOCK_OR_FAIL(s, A(0)); *r = 0; return 1; }
    if (!strcmp(n, "getsockopt")){ SOCK_OR_FAIL(s, A(0));
        uint32_t p = A(3), lp = A(4);
        if (p && lp && MR32(lp) >= 4) { MR32(p) = (A(2) == 0x1007u /* SO_ERROR */ && s->refused) ? W_ECONNREFUSED : (A(2) == 0x1001u || A(2) == 0x1002u) ? 65536u : 0; MR32(lp) = 4; }
        *r = 0; return 1; }
    if (!strcmp(n, "getsockname")){ SOCK_OR_FAIL(s, A(0)); autobind(s); sa_put(A(1), A(2), vip(), s->port); *r = 0; return 1; }
    if (!strcmp(n, "shutdown")){ SOCK_OR_FAIL(s, A(0)); *r = 0; return 1; }
    if (!strcmp(n, "inet_addr")){
        unsigned a, b, cc, d; const char *t = (const char *)P(A(0));
        if (t && sscanf(t, "%u.%u.%u.%u", &a, &b, &cc, &d) == 4 && a < 256 && b < 256 && cc < 256 && d < 256)
            *r = a | b << 8 | cc << 16 | d << 24;
        else *r = 0xFFFFFFFFu;
        return 1; }
    if (!strcmp(n, "inet_ntoa")){ static uint32_t buf; if (!buf) buf = gheap_alloc(1, 32, 1);
        uint32_t v = A(0); snprintf((char*)g_mem + buf, 32, "%u.%u.%u.%u", v & 255u, (v >> 8) & 255u, (v >> 16) & 255u, v >> 24); *r = buf; return 1; }
    if (!strcmp(n, "gethostname")){ if (A(1) < sizeof HOSTNAME) ERR(10014, 0xFFFFFFFFu); memcpy(P(A(0)), HOSTNAME, sizeof HOSTNAME); *r = 0; return 1; }
    if (!strcmp(n, "gethostbyname")){
        char name[256]; strncpy(name, A(0) ? (const char *)P(A(0)) : "", sizeof name - 1); name[sizeof name - 1] = 0;
        uint32_t ip;
        unsigned a, b, cc, d;
        if (!strcasecmp(name, HOSTNAME) || !name[0]) ip = vip();
        else if (sscanf(name, "%u.%u.%u.%u", &a, &b, &cc, &d) == 4) ip = a << 24 | b << 16 | cc << 8 | d;
        else ip = NET_SERVER_VIP;
        if (g_log) fprintf(stderr, "[net] gethostbyname(\"%s\") -> %u.%u.%u.%u\n", name, ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
        /* struct hostent: name, aliases (empty), family 2, length 4, address list (one) */
        uint32_t g = gheap_alloc(1, 40, 1);
        MR32(g) = guest_string(name); MR32(g + 4) = g + 16u; MR16(g + 8) = 2; MR16(g + 10) = 4; MR32(g + 12) = g + 20u;
        MR32(g + 16) = 0; MR32(g + 20) = g + 28u; MR32(g + 24) = 0;
        uint8_t *p = g_mem + g + 28; p[0] = (uint8_t)(ip >> 24); p[1] = (uint8_t)(ip >> 16); p[2] = (uint8_t)(ip >> 8); p[3] = (uint8_t)ip;
        *r = g; return 1; }
    fprintf(stderr, "[net] %s: not in the browser's Winsock\n", n);
    ERR(W_EOPNOTSUPP, 0xFFFFFFFFu);
}
int winsock_session_active(void){ return g_started > 0; }
void winsock_report(void){
    if (!g_calls) return;
    fprintf(stderr, "\n=== WINSOCK (virtual): %llu calls, sent %llu bytes, received %llu bytes, page state %u, vip %08x ===\n",
            (unsigned long long)g_calls, (unsigned long long)g_tx, (unsigned long long)g_rx, g_mb->page_state, g_mb->vip);
    for (int i = 0; i < NSOCK; i++) if (g_s[i].used && (g_s[i].txn || g_s[i].rxn))
        fprintf(stderr, "    socket 0x%x port %u: %llu packets sent (%llu B), %llu received (%llu B), %llu dropped\n", SOCK_BASE + (uint32_t)i, g_s[i].port,
                (unsigned long long)g_s[i].txn, (unsigned long long)g_s[i].tx, (unsigned long long)g_s[i].rxn, (unsigned long long)g_s[i].rx, (unsigned long long)g_s[i].drop);
}

/* ICMP.DLL: IcmpSendEcho(handle, dest, data, size, options, reply, replySize, timeout ms) - an echo through the switch
 * (vnet.mjs answers it), the reply as ICMP_ECHO_REPLY {Address, Status, RoundTripTime, u16 DataSize, u16 0, Data,
 * Options 8 bytes} + the data.  The handle is a raw socket of the table, so drain() queues the replies for it. */
static uint16_t g_icmp_seq;
static int icmp_call(CPU *c, const char *n, uint32_t *r){
    if (g_log < 0){ const char *e = getenv("NETLOG"); g_log = e ? atoi(e) : 0; }
    mbox_init();
    if (!strcmp(n, "IcmpCreateFile")){ *r = sk_new(T_RAW); return 1; }
    if (!strcmp(n, "IcmpCloseHandle")){ GS *s = sk(A(0)); if (s) sk_free(s); *r = s != 0; return 1; }
    if (strcmp(n, "IcmpSendEcho")) return 0;
    GS *s = sk(A(0));
    uint32_t dest = __builtin_bswap32(A(1)), data = A(2), size = A(3) & 0xFFFFu, reply = A(5), rsize = A(6), timeout = A(7);
    if (!s || size > 1024 || rsize < 28u + size){ *r = 0; return 1; }
    uint8_t m[8 + 1024];
    const uint16_t id = (uint16_t)(A(0) & 0xFFFF), seq = ++g_icmp_seq;
    m[0] = 8; m[1] = 0; m[2] = m[3] = 0; m[4] = (uint8_t)(id >> 8); m[5] = (uint8_t)id; m[6] = (uint8_t)(seq >> 8); m[7] = (uint8_t)seq;
    if (size) memcpy(m + 8, g_mem + data, size);
    const double t0 = now_ms(), end = t0 + (timeout ? timeout : 1000);
    tx_frame(NET_KIND_LAN_ICMP, dest, 0, 0, m, 8 + size);
    for (;;){
        drain();
        while (s->qh != s->qt){
            Dgram *d = &s->q[s->qt % QLEN];
            int match = d->len >= 28 && d->data[20] == 0 && d->data[24] == (uint8_t)(id >> 8) && d->data[25] == (uint8_t)id
                        && d->data[26] == (uint8_t)(seq >> 8) && d->data[27] == (uint8_t)seq;
            if (match){
                uint32_t dn = d->len - 28u; if (dn > size) dn = size;
                uint32_t rtt = (uint32_t)(now_ms() - t0);
                memset(g_mem + reply, 0, 28);
                MR32(reply) = __builtin_bswap32(d->ip); MR32(reply + 4) = 0; MR32(reply + 8) = rtt;
                MR16(reply + 12) = (uint16_t)dn; MR32(reply + 16) = reply + 28u; MR8(reply + 20) = 64;
                memcpy(g_mem + reply + 28, d->data + 28, dn);
                free(d->data); s->qt++;
                if (g_log) fprintf(stderr, "[net] ping %u.%u.%u.%u: %u ms\n", dest >> 24, (dest >> 16) & 255, (dest >> 8) & 255, dest & 255, rtt);
                *r = 1; return 1;
            }
            free(d->data); s->qt++;
        }
        if (now_ms() >= end) break;
        wait_rx(end - now_ms());
    }
    MR32(reply) = 0; MR32(reply + 4) = 11010;    /* IP_REQ_TIMED_OUT */
    *r = 0; return 1;
}
