/* nfs5-web WEB2n: unattended network test.  NETTEST=host|join takes the instance
 * through the IP-mode frontend (game arguments hostgame=1 / joingame=<ip>.<port>)
 * into a race, the way a button click does: the command dispatcher on the screen's
 * callback object.  Runs from the hook at the frontend frame 0x4D2B80.
 * Addresses: _mod/docs/findings/NET0-mp-frontend-automation.md.
 * Without NETTEST every hook returns at once. */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
uint32_t gheap_alloc(int heap, uint32_t n, int zero);

#define COMMS    0x00628C70u
#define DISPATCH 0x0050D6D0u
#define CANSTART 0x005091B0u
#define CARINFO  0x0065808Cu
#define VT_CREATE 0x005B8DFCu
#define VT_JOIN   0x005B90A0u
#define SIMTICK  0x00606AC4u
#define CARS     0x005E8B34u
#define CI_AIDRIVE 0x144u
#define CI_READY   0x578u

static int g_role = -1;              /* 0 off, 1 host, 2 join */
static int g_online;                 /* ON1: through the menus of the Internet mode (NETTEST=online-host|online-join) */
static int g_players = 2;
static volatile int g_inrace;
static int g_busy;

static double now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }
#define LOG(...) do { fprintf(stderr, "[nettest %.1f] ", now_s() - g_t0); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)
static double g_t0;

static uint32_t comms(void){ return MR32(COMMS); }
static int cint(uint32_t off){ uint32_t k = comms(); return k ? (int)MR32(k + off) : -999; }
static int cbyte(uint32_t off){ uint32_t k = comms(); return k ? MR8(k + off) : 0; }
static int members(void){
    uint32_t k = comms(); if (!k) return 0;
    uint32_t head = MR32(k + 0x260); if (!head) return 0;
    int n = 0;
    for (uint32_t p = MR32(head); p && p != head && n < 16; p = MR32(p)) n++;
    return n;
}
static void command(CPU *c, uint32_t fe, const char *cmd){
    static uint32_t buf; if (!buf) buf = gheap_alloc(1, 64, 1);
    uint32_t obj = MR32(fe + 0xE0); if (!obj) return;
    strncpy((char*)g_mem + buf, cmd, 63);
    LOG("command %s", cmd);
    uint32_t ecx = c->ecx; c->ecx = obj;
    uint32_t a = buf; call_guest(c, DISPATCH, 1, &a);
    c->ecx = ecx;
}

/* read-only look at the race from outside the guest threads (and AI-drive for the
 * local car: its flag is read when the race is built) */
static void *monitor(void *u){
    (void)u;
    double last = 0;
    for (;;){
        usleep(20000);
        int menus = cbyte(0xBC);
        int slot = cint(0xDC);
        if (comms() && !menus && g_inrace == 0 && slot >= 0 && slot < 8){
            g_inrace = 1; LOG("race start received: slot %d, %d members", slot, members());
        }
        if (g_inrace && slot >= 0 && slot < 8 && getenv("NETTEST_AI"))
            MR32(CARINFO + (uint32_t)slot * 0x580u + CI_AIDRIVE) = 1;
        if (now_s() - last < 3.0) continue;
        last = now_s();
        char cars[200]; int o = 0; cars[0] = 0;
        for (uint32_t i = 0; i < 8; i++){ uint32_t car = MR32(CARS + 4u * i);
            if (car) o += snprintf(cars + o, sizeof cars - (size_t)o, " %u:%d", i, (int)(int16_t)MR16(car + 8u)); }
        LOG("link %x status %d slot %d logged %d menus %d failed %d members %d | sim tick %u | car slices%s", cint(0x08), cint(0x24), slot, cbyte(0xBE), menus, cbyte(0xBF), members(), MR32(SIMTICK), cars);
    }
    return NULL;
}

static void init_role(void){
    const char *e = getenv("NETTEST");
    g_role = !e ? 0 : !strcmp(e, "host") || !strcmp(e, "online-host") ? 1 : !strcmp(e, "join") || !strcmp(e, "online-join") ? 2 : 0;
    g_online = e && !strncmp(e, "online-", 7);
    if (!g_role) return;
    g_t0 = now_s();
    if (getenv("NETTEST_PLAYERS")) g_players = atoi(getenv("NETTEST_PLAYERS"));
    pthread_t t; pthread_create(&t, NULL, monitor, NULL);
    LOG("role %s", e);
}

/* ---- ON1: the Internet route through the menus (after the PC mod's net_autotest.cpp, NET0 doc section 8) ---- */
#define FIND_ACC   0x004B7AB0u
#define DYNCAST    0x005A134Bu
#define RTTI_ACC   0x005CD6A8u
#define RTTI_STR   0x005D7588u
#define RTTI_USERS 0x005D71E0u
#define RTTI_SERV  0x005D7268u
#define RTTI_GAMES 0x005D7310u
#define RTTI_LOCAL 0x005D74B8u
#define NAVIGATE   0x004D2100u
#define CREATEGAME 0x00509A00u
#define VT_MAIN    0x005B9D64u
#define VT_SERVERS 0x005B9424u
#define VT_GAMES   0x005B9504u
static uint32_t gstr(const char *t){ static uint32_t b[4]; static int k; uint32_t p = b[k & 3]; if (!p) p = b[k & 3] = gheap_alloc(1, 128, 1);
    k++; strncpy((char*)g_mem + p, t, 127); return p; }
static uint32_t thiscall(CPU *c, uint32_t obj, uint32_t fn, int n, const uint32_t *a){
    uint32_t ecx = c->ecx; c->ecx = obj; uint32_t r = call_guest(c, fn, n, a); c->ecx = ecx; return r; }
static uint32_t vfn(uint32_t obj, uint32_t off){ return MR32(MR32(obj) + off); }
static uint32_t accessor(CPU *c, const char *name, uint32_t rtti){
    uint32_t a = gstr(name), acc = call_guest(c, FIND_ACC, 1, &a);
    if (!acc) return 0;
    uint32_t args[5] = { acc, 0, RTTI_ACC, rtti, 0 };
    return call_guest(c, DYNCAST, 5, args);
}
static void set_string(CPU *c, const char *acc, const char *text){
    uint32_t a = accessor(c, acc, RTTI_STR);
    if (!a){ LOG("no %s accessor", acc); return; }
    thiscall(c, a, vfn(a, 0x44), 0, NULL);
    uint32_t t = gstr(text); thiscall(c, a, vfn(a, 0x24), 1, &t);
}
static int find_row(CPU *c, uint32_t acc, const char *want, const char *what){
    int n = (int)thiscall(c, acc, vfn(acc, 0x24), 0, NULL);
    for (int i = 0; i < n && i < 64; i++){
        uint32_t ia = (uint32_t)i, sp = thiscall(c, acc, vfn(acc, 0x30), 1, &ia);
        const char *s = sp ? (const char*)g_mem + sp : NULL;
        if (!s) continue;
        if (!want || !*want || strstr(s, want)){ LOG("%s row %d/%d '%.40s'", what, i, n, s); return i; }
    }
    return -1;
}
static void set_index(CPU *c, uint32_t acc, int i){ uint32_t a = (uint32_t)i; thiscall(c, acc, vfn(acc, 0x2C), 1, &a); }
static void navigate(CPU *c, uint32_t fe, const char *name, int off){
    uint32_t a[3] = { gstr(name), (uint32_t)off, 1 }; LOG("navigate %s (%d)", name, off); thiscall(c, fe, NAVIGATE, 3, a); }
/* 0 not started, 1 profile, 2 multiplayer, 3 login, 4 servers, 5 lobby, 6 done */
static int g_on, g_on_step; static double g_on_t;
static void on_go(int s, const char *why){ g_on = s; g_on_step = 0; g_on_t = now_s(); LOG("online step %d (%s), status %d", s, why, cint(0x24)); }
static int online_tick(CPU *c, uint32_t fe, uint32_t vt){
    double in = now_s() - g_on_t;
    const char *acct = getenv("NETTEST_ACCOUNT") ? getenv("NETTEST_ACCOUNT") : (g_role == 1 ? "host" : "joiner");
    const char *game = getenv("NETTEST_GAME") ? getenv("NETTEST_GAME") : "Online test";
    if (!g_on) on_go(1, "start");
    switch (g_on){
    case 1:
        if (vt != VT_MAIN || in < 4) break;
        { uint32_t u = accessor(c, "UserList.UserList", RTTI_USERS);
          int r = u ? find_row(c, u, getenv("NETTEST_PROFILE") ? getenv("NETTEST_PROFILE") : "", "profile") : -1;
          if (r >= 0) set_index(c, u, r); }
        command(c, fe, "SETNAMETHENBACK"); on_go(2, "profile"); break;
    case 2:
        if (vt != VT_MAIN || in < 3) break;
        command(c, fe, "SETTYPEMULTIPLAYER"); navigate(c, fe, "protocol", 1); on_go(3, "multiplayer"); break;
    case 3:
        if (g_on_step == 0 && in > 2){ g_on_step = 1; command(c, fe, "SETMODENET"); navigate(c, fe, "accountlogin", 0); }
        if (g_on_step == 1 && in > 5){ g_on_step = 2; set_string(c, "COMMS.ACCOUNTID", acct); set_string(c, "COMMS.PASSWORD", "x"); command(c, fe, "SIGNON"); }
        if (g_on_step == 2 && cint(0x24) == 0x0E && vt == VT_SERVERS) on_go(4, "logged in");
        if (in > 60){ LOG("FAIL: no login (status %d, screen vt %08x)", cint(0x24), vt); on_go(6, "fail"); }
        break;
    case 4:
        if (in < 2) break;
        if (!g_on_step){
            uint32_t sv = accessor(c, "Comms.ServerList", RTTI_SERV);
            int r = sv ? find_row(c, sv, "", "server") : -1;
            if (r < 0){ if (in > 30){ LOG("FAIL: no server row"); on_go(6, "fail"); } break; }   /* the row shows once the ping answered */
            set_index(c, sv, r); command(c, fe, "ENTER"); navigate(c, fe, "udpip", 0); g_on_step = 1;
        }
        if (vt == VT_GAMES && cbyte(0xBE)) on_go(5, "in the lobby");
        break;
    case 5:
        if (in < 3) break;
        if (g_role == 1){
            set_string(c, "Comms.GameName", game);
            uint32_t z = 0; call_guest(c, CREATEGAME, 1, &z);
            navigate(c, fe, "creategame", 0); on_go(6, "game created");
        } else {
            uint32_t g = accessor(c, "Comms.GamesList", RTTI_GAMES);
            int r = g ? find_row(c, g, game, "game") : -1;
            if (r < 0 && g && in > 13) r = find_row(c, g, "", "game (any)");
            if (r < 0) break;
            set_index(c, g, r); command(c, fe, "JOIN"); on_go(6, "joining");
        }
        break;
    }
    return g_on < 6;
}
static void host_mode(CPU *c){
    const char *m = getenv("NETTEST_MODE");             /* the MODE combo: 0 PvP, 1 PvE, 2 career */
    if (!m) return;
    uint32_t pm = accessor(c, "Comms.Positional", RTTI_LOCAL);
    if (pm){ set_index(c, pm, atoi(m)); LOG("race mode %s", m); }
}

static void fe_tick(CPU *c){
    static double since, last_ready, all_since;
    static int phase;
    if (g_busy) return;
    g_busy = 1;
    uint32_t fe = c->ecx, obj = fe ? MR32(fe + 0xE0) : 0, vt = obj ? MR32(obj) : 0;
    double t = now_s();
    int slot = cint(0xDC);
    if (g_online && online_tick(c, fe, vt)){ g_busy = 0; return; }
    if (g_role == 1){
        if (phase == 0 && vt == VT_CREATE && slot >= 0){ phase = 1; since = t; LOG("create screen, slot %d", slot); if (g_online) host_mode(c); }
        if (phase == 1 && t - since > 2.0){
            int all = (int)call_guest(c, CANSTART, 0, NULL), m = members();
            if (!(all && m >= g_players)) all_since = 0;
            else if (!all_since){ all_since = t; LOG("all %d ready", m); }
            if (all_since && t - all_since > 5.0){ phase = 2; command(c, fe, "PLAYGAME"); LOG("back from PLAYGAME"); }
        }
    } else {
        if (phase == 0 && vt == VT_JOIN && slot >= 0 && cbyte(0xBC)){ phase = 1; since = t; LOG("join screen, slot %d", slot); }
        if (phase == 1 && t - since > 3.0 && slot >= 0 && slot < 8 && cbyte(0xBC)){
            if (!MR32(CARINFO + (uint32_t)slot * 0x580u + CI_READY) && t - last_ready > 3.0){ last_ready = t; command(c, fe, "READY"); }
        }
    }
    g_busy = 0;
}

void nettest_hook(CPU *c, uint32_t ip){
    if (g_role < 0) init_role();
    if (!g_role) return;
    if (ip == 0x4D2B80u) fe_tick(c);
    /* the joiner connects at game start and again when the frontend starts (the second
     * hung it): cmp ecx,ebx / je at 004D3D00 taken = no second join */
    else if (ip == 0x4D3D00u && g_role == 2){ c->ecx = c->ebx; LOG("frontend's second join skipped"); }
}
