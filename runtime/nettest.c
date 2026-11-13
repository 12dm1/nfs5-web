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
static int g_online_req;            /* ON1: the Online button was pressed (online_auto) */
static int g_humans = -1;           /* ON2: players in the game, from the server's "humans N" line (bots are members too) */
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
        command(c, fe, "SETTYPEMULTIPLAYER"); navigate(c, fe, "protocol", 1);
        if (getenv("NETTEST_UI")){ g_online_req = 1; on_go(7, "online button"); } else on_go(3, "multiplayer");
        break;
    case 7:   /* NETTEST_UI: online_auto takes it to the lobby */
        if (vt == VT_GAMES && cbyte(0xBE)) on_go(5, "in the lobby");
        else if (in > 60){ LOG("FAIL: the online button did not reach the lobby (status %d, vt %08x)", cint(0x24), vt); on_go(6, "fail"); }
        break;
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

/* ---- ON1: the Online button.  The main menu's MULTIPLAYER (renamed "online" by the 004E06D0 override) sets
 * g_online_req (00510B10 SETTYPEMULTIPLAYER override below); the frontend frame then goes the Internet route by itself:
 * Internet -> sign on with the profile's account -> our one server -> its lobby (udpip).  Back from the lobby lands on
 * the server list, which goes back to the main menu. ---- */
static int g_auto, g_auto_n; static double g_auto_t;
void sub_00510b10_orig(CPU *);
void sub_00510b10(CPU *restrict c){ sub_00510b10_orig(c); if (g_role <= 0) g_online_req = 1; }
static void auto_go(int s){ g_auto = s; g_auto_n = 0; g_auto_t = now_s(); fprintf(stderr, "[online] step %d, status %d\n", s, cint(0x24)); }
static const char *online_account(void){
    static char a[24];
    if (!a[0]){ const char *e = getenv("NFS5_ACCOUNT");
        if (e && *e) snprintf(a, sizeof a, "%s", e);
        else { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); snprintf(a, sizeof a, "web%06ld", (long)((t.tv_sec * 7919L + t.tv_nsec / 1000) % 1000000L)); } }
    return a;
}

/* ---- ON2: career tournament races (web/online/tournament.mjs).  The server sends the game's creator a line
 * "race k/N laps=L reverse=R track=NAME" over the control channel; on its create screen it sets the track and the laps.
 * After each race it reports the finishing order of every car: "result slot:place:time ..." (005E77A0 + car index * 258h:
 * place, +4 time; car index -> slot from the member list). ---- */
int online_ctl_recv(char *out, int max);
int online_ctl_send(const char *text);
#define RESULTS 0x005E77A0u
static char g_dir[256]; static int g_dir_new; static uint32_t g_reported_tick;

/* ---- ON2 stage 2: an online tournament race counts in the local career (ON2 section 4.1 route B: the stock pieces of
 * 004EDF00 type 4).  The server sends "career id=<event> race=<r> of=<n> fee=<fee> results p:t p:t ..." with this
 * player as entry 0.  Applied only to an event of a club or of an era the profile has reached, and only when this
 * client had the event from its first race (else the table would never be complete). ---- */
#define TRN_TABLE  0x00691E58u     /* the profile's tournament table, 0xD40 per event */
#define TRN_COUNT  0x00691E64u
#define TRN_CUR    0x00691E6Cu     /* current tournament (index) */
#define TRN_NEXT   0x00691E7Cu     /* next race in it */
#define TRN_ERA    0x00691E50u
#define TRN_RACE   0x005DB768u     /* race / tournament being played */
#define TRN_PLAYED 0x005DB76Cu
#define CASH       0x00691D90u
static int g_car_id = -1, g_car_next = -1;
static uint32_t cg0(CPU *c, uint32_t fn){ return call_guest(c, fn, 0, NULL); }
static uint32_t cg1(CPU *c, uint32_t fn, uint32_t a){ return call_guest(c, fn, 1, &a); }
static void career_apply(CPU *c, const char *m){
    int id = -1, race = -1, of = 0, fee = 0, place[8], tm[8], n = 0;
    if (sscanf(m, "career id=%d race=%d of=%d fee=%d", &id, &race, &of, &fee) != 4) return;
    const char *r = strstr(m, "results ");
    for (char *e; r && n < 8; n++){
        r = n ? r : r + 8;
        place[n] = (int)strtol(r, &e, 10); if (e == r || *e != ':') break;
        tm[n] = (int)strtol(e + 1, &e, 10); r = e;
    }
    const uint32_t table = MR32(TRN_TABLE); const int count = (int)MR32(TRN_COUNT);
    int idx = -1;
    for (int i = 0; table && i < count && i < 64; i++) if ((int)(int16_t)MR16(table + (uint32_t)i * 0xD40u) == id){ idx = i; break; }
    if (idx < 0 || n < 1){ LOG("career: event %d not in this profile (%d events), not applied", id, count); return; }
    const uint32_t ev = table + (uint32_t)idx * 0xD40u;
    const int group = (int)MR32(ev + 4), era = (int)MR32(TRN_ERA);
    if (group >= 0x209 && group <= 0x20B && group > era){ LOG("career: event %d is of era %X, the profile is in %X: not applied", id, group, era); return; }
    if (race == 0){
        if ((int)MR32(CASH) < fee){ LOG("career: entry fee %d DM > cash %d: not applied", fee, (int)MR32(CASH)); g_car_id = -1; return; }
        cg1(c, 0x00503E40u, (uint32_t)id);                         /* this event's old results */
        cg1(c, 0x005012E0u, (uint32_t)-fee);
        g_car_id = id; g_car_next = 0;
    }
    if (g_car_id != id || g_car_next != race){ LOG("career: race %d of event %d without the earlier ones: not applied", race + 1, id); return; }
    const uint32_t cur = MR32(TRN_CUR), next = MR32(TRN_NEXT), cash0 = MR32(CASH);
    MR32(TRN_CUR) = (uint32_t)idx; MR32(TRN_NEXT) = (uint32_t)race;
    MR32(TRN_RACE) = (uint32_t)race; MR32(TRN_PLAYED) = (uint32_t)idx;
    for (int k = 0; k < n; k++){ const uint32_t a[4] = { (uint32_t)k, (uint32_t)tm[k], (uint32_t)place[k], 1 }; call_guest(c, 0x00503960u, 4, a); }
    cg1(c, 0x005012E0u, cg0(c, 0x00505740u));                     /* race prize */
    cg1(c, 0x005012E0u, cg0(c, 0x005057A0u));                     /* bonus */
    const int last = cg0(c, 0x005056D0u) == 0;
    int overall = 0;
    if (last){ overall = (int)cg0(c, 0x00505710u); cg1(c, 0x005012E0u, cg0(c, 0x00505770u) ); }
    cg0(c, 0x00503A50u);
    cg1(c, 0x00503AD0u, (uint32_t)id);                            /* progress: next race, or the event done, year, era */
    const int era_moved = (int)MR32(TRN_ERA) != era || MR8(0x00691E81u);
    if (!era_moved){ MR32(TRN_CUR) = cur; MR32(TRN_NEXT) = next; } /* the offline career stays where it was */
    g_car_next = race + 1;
    LOG("career: event %d race %d/%d place %d -> cash %d -> %d%s%s", id, race + 1, of, place[0], (int)cash0, (int)MR32(CASH),
        last ? ", event done" : "", era_moved ? ", NEW ERA" : "");
    (void)overall;
    const uint32_t name = cg0(c, 0x00501270u);                     /* save the profile (004DEDA0, "Saving for %s") */
    if (name && MR8(name)) cg1(c, 0x004DEDA0u, name);
}
static void career_tick(CPU *c, uint32_t fe, uint32_t vt){
    char m[512];
    while (online_ctl_recv(m, sizeof m)){
        fprintf(stderr, "[online] server: %s\n", m);
        if (!strncmp(m, "race ", 5)){ snprintf(g_dir, sizeof g_dir, "%s", m); g_dir_new = 1; }
        else if (!strcmp(m, "over")) g_dir[0] = 0;
        else if (!strncmp(m, "humans ", 7)) g_humans = atoi(m + 7);
        else if (!strncmp(m, "career ", 7)) career_apply(c, m);
    }
    if (vt != VT_CREATE || cint(0xDC) != 0 || !g_dir[0]) return;          /* the creator, on its create screen */
    const uint32_t tick = MR32(SIMTICK);
    if (tick > 64 && tick != g_reported_tick){                             /* back from a race */
        char r[400]; int n = snprintf(r, sizeof r, "result");
        const uint32_t head = comms() ? MR32(comms() + 0x260) : 0;
        int k = 0;
        for (uint32_t p = head ? MR32(head) : 0; p && p != head && k < 16; p = MR32(p), k++){
            const uint32_t pl = MR32(p + 8);
            if (!pl) continue;
            const int slot = (int)MR32(pl + 0x40), idx = (int)MR32(pl + 0x4C);
            if (idx < 0 || idx > 8 || slot < 0 || slot > 7) continue;
            n += snprintf(r + n, sizeof r - (size_t)n, " %d:%d:%d", slot, (int)MR32(RESULTS + (uint32_t)idx * 0x258u), (int)MR32(RESULTS + (uint32_t)idx * 0x258u + 4));
        }
        fprintf(stderr, "[online] %s (sim tick %u)\n", r, tick);
        online_ctl_send(r);
        g_reported_tick = tick;
        return;
    }
    if (g_dir_new){
        g_dir_new = 0;
        const char *tr = strstr(g_dir, "track="), *lp = strstr(g_dir, "laps=");
        uint32_t t = accessor(c, "Track1.Track", 0x005DB2A0u);
        int row = (t && tr) ? find_row(c, t, tr + 6, "tournament track") : -1;
        if (row >= 0) set_index(c, t, row); else fprintf(stderr, "[online] track '%s' not in the list\n", tr ? tr + 6 : "?");
        if (lp){
            char want[8]; snprintf(want, sizeof want, "%d", atoi(lp + 5));
            uint32_t l = accessor(c, "Track1.Laps", RTTI_LOCAL);
            int lr = l ? find_row(c, l, want, "laps") : -1;
            if (l) set_index(c, l, lr >= 0 ? lr : atoi(lp + 5) - 1);
        }
    }
}
static void online_auto(CPU *c){
    uint32_t fe = c->ecx, obj = fe ? MR32(fe + 0xE0) : 0, vt = obj ? MR32(obj) : 0;
    const double in = now_s() - g_auto_t;
    career_tick(c, fe, vt);
    if (g_online_req){ g_online_req = 0; auto_go(1); return; }
    switch (g_auto){
    case 1:   /* on the protocol screen (the button navigated there): Internet */
        if (in < 0.3) break;
        command(c, fe, "SETMODENET"); navigate(c, fe, "accountlogin", 0); auto_go(2); break;
    case 2:
        if (in < 0.5) break;
        if (!g_auto_n){ g_auto_n = 1; set_string(c, "COMMS.ACCOUNTID", online_account()); set_string(c, "COMMS.PASSWORD", "x"); command(c, fe, "SIGNON"); }
        if (cint(0x24) == 0x0E && vt == VT_SERVERS) auto_go(3);
        else if (in > 30){ fprintf(stderr, "[online] no answer from the server (status %d)\n", cint(0x24)); auto_go(0); }
        break;
    case 3: {
        uint32_t sv = accessor(c, "Comms.ServerList", RTTI_SERV);
        int r = sv ? find_row(c, sv, "", "server") : -1;                    /* the row shows once the ping answered */
        if (r < 0){ if (in > 15){ fprintf(stderr, "[online] no server row\n"); auto_go(0); } break; }
        set_index(c, sv, r); command(c, fe, "ENTER"); navigate(c, fe, "udpip", 0); auto_go(4); break; }
    case 4:
        if (vt == VT_GAMES) auto_go(5);
        else if (in > 15) auto_go(0);
        break;
    case 5:   /* in the lobby or a game; back on the server list = leave online */
        if (vt == VT_SERVERS){ command(c, fe, "GOBACK"); auto_go(6); }
        break;
    case 6:   /* accountlogin -> protocol -> main menu */
        if (in < 0.4) break;
        if (vt == VT_MAIN && g_auto_n >= 2){ auto_go(0); break; }
        if (g_auto_n < 4){ command(c, fe, "GOBACK"); g_auto_n++; g_auto_t = now_s(); }
        else auto_go(0);
        break;
    }
}
static void host_mode(CPU *c){
    {   /* the track (PC net_autotest.cpp PickTrack): NETTEST_TRACK = part of its name, else the list's current row */
        uint32_t t = accessor(c, "Track1.Track", 0x005DB2A0u);
        if (!t) LOG("no Track1.Track accessor");
        else {
            int n = (int)thiscall(c, t, vfn(t, 0x24), 0, NULL);
            LOG("tracks: %d", n);
            int r = find_row(c, t, getenv("NETTEST_TRACK") ? getenv("NETTEST_TRACK") : "", "track");
            if (r >= 0) set_index(c, t, r);
        }
    }
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
            if (!(all && m >= g_players && (g_humans < 0 || g_humans >= g_players))) all_since = 0;
            else if (!all_since){ all_since = t; LOG("all %d ready", m); }
            if (all_since && t - all_since > 5.0){ phase = 2; command(c, fe, "PLAYGAME"); LOG("back from PLAYGAME"); }
        }
        /* ON2: the next race of a tournament once the create screen is back */
        static uint32_t raced;
        if (phase == 2 && vt == VT_CREATE && MR32(SIMTICK) > 64 && MR32(SIMTICK) != raced && g_dir[0]){
            raced = MR32(SIMTICK); phase = 1; since = t + 5.0; all_since = 0; LOG("back on the create screen: next race (%s)", g_dir); }
    } else {
        if (phase == 0 && vt == VT_JOIN && slot >= 0 && cbyte(0xBC)){ phase = 1; since = t; LOG("join screen, slot %d", slot); }
        if (phase == 1 && t - since > 3.0 && slot >= 0 && slot < 8 && cbyte(0xBC)){
            if (!MR32(CARINFO + (uint32_t)slot * 0x580u + CI_READY) && t - last_ready > 3.0){ last_ready = t; command(c, fe, "READY"); }
        }
    }
    g_busy = 0;
}


/* A network race never ends by itself (00417010 skips the "everyone finished" end for game type 3): every player
 * leaves through the race menu, and the creator's results (tournaments) hold only the cars that finished before it
 * left.  So in a network race (006573E8 == 3) the runtime leaves for the player once every car has finished, or 30 s
 * after its own finish (5 s at least), as the menu's "quit" entry does (004152E0 case 5; PC mod NetAutotest_RaceTick).
 * From the race's input hook 0x413800. */
void nettest_race(CPU *c){
    static uint32_t finished_at, last_tick; static int left;
    if (MR32(0x006573E8u) != 3u || !comms()) return;
    const uint32_t tick = MR32(SIMTICK);
    if (tick < last_tick){ finished_at = 0; left = 0; }
    last_tick = tick;
    const int pi = (int)MR32(0x0065741Cu);
    if (left || pi < 0 || pi > 15) return;
    const uint32_t car = MR32(0x005E8E2Cu + 4u * (uint32_t)pi);
    if (!car) return;
    const int b50 = (int)MR32(car + 0xB50u);
    if (!b50){ finished_at = 0; return; }
    if (!finished_at){ finished_at = tick ? tick : 1; LOG("local car finished (state %d) at tick %u", b50, tick); return; }
    if (tick - finished_at < 5 * 64) return;
    int all = 1; const int n = (int)MR32(0x005E8AE8u);
    for (int i = 0; i < n && i < 9; i++){ const uint32_t k = MR32(0x005E8E2Cu + 4u * (uint32_t)i); if (k && !MR32(k + 0xB50u)) all = 0; }
    if (!all && tick - finished_at < 30 * 64) return;
    left = 1;
    LOG("leaving the race (tick %u, %s)", tick, all ? "every car finished" : "30 s after the finish");
    call_guest(c, 0x004AB270u, 0, NULL);
    call_guest(c, 0x004120F0u, 0, NULL);
    MR32(0x00606A90u) = 0; MR32(0x00606A8Cu) = 1; MR32(0x00606A88u) = 0;
    if ((int)MR32(0x005E99F4u) < 2){ uint32_t one = 1; MR32(0x00657424u) = 3; call_guest(c, 0x00416AC0u, 1, &one); }
}

void nettest_hook(CPU *c, uint32_t ip){
    if (g_role < 0) init_role();
    if (!g_role || (g_online && getenv("NETTEST_UI"))){ if (ip == 0x4D2B80u && !g_busy){ g_busy = 1; online_auto(c); g_busy = 0; } if (!g_role) return; }
    if (ip == 0x4D2B80u) fe_tick(c);
    /* the joiner connects at game start and again when the frontend starts (the second
     * hung it): cmp ecx,ebx / je at 004D3D00 taken = no second join */
    else if (ip == 0x4D3D00u && g_role == 2){ c->ecx = c->ebx; LOG("frontend's second join skipped"); }
}
