/* nfs5-web WEB2n: unattended network test.  NETTEST=host|join takes the instance
 * through the IP-mode frontend (game arguments hostgame=1 / joingame=<ip>.<port>)
 * into a race, the way a button click does: the command dispatcher on the screen's
 * callback object.  Runs from the hook at the frontend frame 0x4D2B80.
 * Addresses: _mod/docs/findings/NET0-mp-frontend-automation.md.
 * Without NETTEST every hook returns at once. */
#include "cpu.h"
#include "pu_cars.h"
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
#define CARINFO  CARS_INFO     /* CARS1: moved */
#define VT_CREATE 0x005B8DFCu
#define VT_JOIN   0x005B90A0u
#define SIMTICK  0x00606AC4u
#define CARS     CARS_L_CARS
#define CI_AIDRIVE 0x144u
#define CI_READY   0x578u

static int g_role = -1;              /* 0 off, 1 host, 2 join */
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
    g_role = !e ? 0 : !strcmp(e, "host") ? 1 : !strcmp(e, "join") ? 2 : 0;
    if (!g_role) return;
    g_t0 = now_s();
    if (getenv("NETTEST_PLAYERS")) g_players = atoi(getenv("NETTEST_PLAYERS"));
    pthread_t t; pthread_create(&t, NULL, monitor, NULL);
    LOG("role %s", e);
}

static void fe_tick(CPU *c){
    static double since, last_ready, all_since;
    static int phase;
    if (g_busy) return;
    g_busy = 1;
    uint32_t fe = c->ecx, obj = fe ? MR32(fe + 0xE0) : 0, vt = obj ? MR32(obj) : 0;
    double t = now_s();
    int slot = cint(0xDC);
    if (g_role == 1){
        if (phase == 0 && vt == VT_CREATE && slot >= 0){ phase = 1; since = t; LOG("create screen, slot %d", slot); }
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
