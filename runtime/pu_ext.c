/* nfs5-web: the converted High Stakes tracks (feature port of the PC wrapper's extended_content.cpp and hs_corridor.cpp).
 * The roster and the sound archives need no code here: the server's data layer gives them the stock names
 * (tools/make_mod_layer.py). Overrides of lifted functions: gen.sh OVERRIDE=... */
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cpu.h"
#include "pu_cars.h"
#include "decls.h"
#include "ext/hs_walls.h"
#include "ext/opp_similar.h"
#include "ext/hs_ai.h"

int fileio_resolve_game(const char *win, char *out, size_t outsz);
uint32_t gheap_alloc(int hi, uint32_t n, int zero);

/* ---- names: FEStrings ids from 0x2000 (NAME_ID_BASE of the converter); display, CAPS, short, SHORT CAPS at +0/+14h/+28h/+3Ch ---- */
#define NAME_BASE 0x2000u
#define MAX_NAMES 32
static uint32_t g_names[MAX_NAMES][4];
static int g_nnames = -1;
static void names_load(void){
    g_nnames = 0;
    char path[1024];
    if (!fileio_resolve_game("FEData\\Data\\nfs5ext.names", path, sizeof path)) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[512];
    while (g_nnames < MAX_NAMES && fgets(line, sizeof line, f)){
        if (line[0] == '#' || !isdigit((unsigned char)line[0])) continue;
        line[strcspn(line, "\r\n")] = 0;
        char *p = strchr(line, '\t');
        int v = 0;
        while (p && v < 4){
            char *s = p + 1;
            p = strchr(s, '\t');
            if (p) *p = 0;
            const uint32_t a = gheap_alloc(1, (uint32_t)strlen(s) + 1, 1);
            strcpy((char *)(g_mem + a), s);
            g_names[g_nnames][v++] = a;
        }
        if (v == 4) g_nnames++;
    }
    fclose(f);
    fprintf(stderr, "[hs] %d track names\n", g_nnames);
}
/* 004E06D0, GetString(id), cdecl */
/* ON1 race modes: the create-game screen's MODE combo (patches/exe_v35.txt points its row table here); the value is
 * setup record 0x14, web/online/netgather.mjs RACE_MODES */
#define RACE_MODE_TABLE 0x006C1F40u
#define RACE_MODE_ID    900001u
static const char *const k_race_modes[3] = { "Quick race", "Quick race + bots", "Career tournament" };
static uint32_t g_race_mode_text[3];
void sub_004e06d0(CPU *restrict c){
    if (g_nnames < 0) names_load();
    if (!g_race_mode_text[0]){
        for (uint32_t i = 0; i < 3; i++){
            MR32(RACE_MODE_TABLE + 4u * i) = RACE_MODE_ID + i;
            g_race_mode_text[i] = gheap_alloc(1, (uint32_t)strlen(k_race_modes[i]) + 1, 1);
            strcpy((char *)(g_mem + g_race_mode_text[i]), k_race_modes[i]);
        }
        MR32(RACE_MODE_TABLE + 12u) = 0;
    }
    const uint32_t id = MR32(c->esp + 4);
    if (id >= RACE_MODE_ID && id < RACE_MODE_ID + 3){ c->eax = g_race_mode_text[id - RACE_MODE_ID]; POP32(c->eip); return; }
    /* ON1: MULTIPLAYER is the Online button now (nettest.c online_auto); its title on the lobby and the help line */
    { static uint32_t t_online, t_help;
      if (!t_online){ static const char a[] = "online", b[] = "Race other players online: quick races, with or without bots, and career tournaments";
          t_online = gheap_alloc(1, sizeof a, 1); memcpy(g_mem + t_online, a, sizeof a);
          t_help = gheap_alloc(1, sizeof b, 1); memcpy(g_mem + t_help, b, sizeof b); }
      if (id == 12403u || id == 16305u || id == 15404u){ c->eax = t_online; POP32(c->eip); return; }
      if (id == 1006u){ c->eax = t_help; POP32(c->eip); return; } }
    static const uint32_t off[4] = { 0, 0x14, 0x28, 0x3C };
    for (int v = 0; v < 4; v++){
        if (id >= NAME_BASE + off[v] && id - (NAME_BASE + off[v]) < (uint32_t)g_nnames){
            c->eax = g_names[id - (NAME_BASE + off[v])][v];
            static int shown;
            if (shown++ < 3) fprintf(stderr, "[hs] name %#x -> %s\n", id, (const char *)(g_mem + c->eax));
            POP32(c->eip);
            return;
        }
    }
    sub_004e06d0_orig(c);
}

/* ---- respawn: 0040F540 asks 0040F480 for a usable slice in a loop without a limit; a converted track may refuse them all ---- */
void sub_0040f480(CPU *restrict c){
    static int refused;
    sub_0040f480_orig(c);
    if (c->eax){ refused = 0; return; }
    if (++refused >= 64){ refused = 0; c->eax = 1; }
}

/* ---- 004677D0, the texture manager's callback (uploading, evicted), cdecl: the only place that knows a texture's DTEX name ---- */
void r_tex_name(const char *name) __attribute__((weak));
void sub_004677d0(CPU *restrict c){
    const uint32_t up = MR32(c->esp + 4);
    if (r_tex_name){
        char name[12] = { 0 };
        const uint32_t np = up ? MR32(up + 8) : 0;
        if (np) for (int i = 0; i < 8 && g_mem[np + i]; i++) name[i] = (char)g_mem[np + i];
        r_tex_name(name);
    }
    sub_004677d0_orig(c);
}

/* ---- walls ---- */
static const char *const k_hs_tracks[] = { "aquatica", "atlantic", "dolphin", "country", "empire", "france", "germany", "gt1", "gt2", "gt3",
    "hills", "hometown", "lostcany", "park", "redrock", "rockypas", "snowy", "summit", "uk" };
static char g_track[32];
static int g_hs;
static void track_check(void){
    const char *name = (const char *)(g_mem + 0x00628940u);    /* Init_BWorld copies the selected name here */
    if (!strncmp(g_track, name, sizeof g_track - 1)) return;
    strncpy(g_track, name, sizeof g_track - 1);
    g_hs = 0;
    for (size_t i = 0; i < sizeof k_hs_tracks / sizeof *k_hs_tracks; i++) if (!strcasecmp(g_track, k_hs_tracks[i])) g_hs = 1;
    hs_walls_clear();
    /* the CRP's path, at 00628964; its .hsw sidecar */
    char win[300], path[1024];
    snprintf(win, sizeof win, "%s", (const char *)(g_mem + 0x00628964u));
    size_t n = strlen(win);
    if (n > 4 && !strcasecmp(win + n - 4, ".crp")) win[n - 4] = 0;
    strncat(win, ".hsw", sizeof win - strlen(win) - 1);
    const int found = fileio_resolve_game(win, path, sizeof path);
    /* a converted track is one with a .hsw beside its CRP: only nfs5-track-tools writes that file, for the 19 High
       Stakes tracks and for any other NFS4 track (community tracks made in T3ED, under names of their own) */
    if (found) g_hs = 1;
    if (!g_hs) return;
    const int segs = found ? hs_walls_load(path) : -1;
    fprintf(stderr, "[hs] track '%s' walls '%s': %d\n", g_track, win, segs);
}
typedef struct { CPU *c; uint32_t car; } WallCtx;
static uint32_t put_vec(CPU *c, const float v[3]){
    c->esp -= 12;
    memcpy(g_mem + c->esp, v, 12);
    return c->esp;
}
#define PUSHA(v) do { c->esp -= 4; MR32(c->esp) = (uint32_t)(v); } while (0)
/* The stock response of 00495020 (corner against an .edg edge): 00494000 applies the impulse, the yaw kick and the impact class
 * and returns the magnitude in st0; 0040A630 runs the hit timer; above 20 (005B2470) 00494DF0 damages the panel and sparks. */
static void wall_hit(void *ctx, const float normal[3], const float point[3], const float push[3]){
    CPU *c = ((WallCtx *)ctx)->c;
    const uint32_t car = ((WallCtx *)ctx)->car, sp = c->esp;
    const uint32_t an = put_vec(c, normal), ap = put_vec(c, point), au = put_vec(c, push);
    MR32(0x00628C88u) = 0x60;       /* the edge material; monaco's edges */
    const uint32_t base = c->esp;
    PUSHA(1); PUSHA(au); PUSHA(ap); PUSHA(an); PUSHA(car); PUSHA(0);
    sub_00494000(c);
    const float impact = (float)ST(0);
    c->top = (c->top + 1) & 7;
    c->esp = base;
    memcpy(g_mem + car + 0x440, point, 12);
    PUSHA(car); PUSHA(0);
    sub_0040a630(c);
    c->esp = base;
    if (impact > 20.0f){
        memcpy(g_mem + car + 0x430, &impact, 4);
        PUSHA(1); PUSHA(an); PUSHA(car); PUSHA(0);
        sub_00494df0(c);
        c->esp = base;
    }
    float mag;
    memcpy(&mag, g_mem + car + 0x430, 4);
    if (mag < impact) memcpy(g_mem + car + 0x430, &impact, 4);
    static unsigned hits;
    if (++hits <= 5 || hits % 256 == 0) fprintf(stderr, "[hs] wall hit %u: car %08x impact %.1f\n", hits, car, impact);
    c->esp = sp;
}
static void wall_rebuild(void *ctx){
    CPU *c = ((WallCtx *)ctx)->c;
    const uint32_t sp = c->esp;
    PUSHA(((WallCtx *)ctx)->car); PUSHA(0);
    sub_0049ae30(c);
    c->esp = sp;
}
/* DAMAGETEST=<impact> (debug): from tick 300, eight stock panel hits (00494DF0) per car from all sides, so visual damage can be
 * checked in a scripted quick race */
static void damage_test(CPU *c, uint32_t car){
    static int imp = -1;
    if (imp < 0){ const char *e = getenv("DAMAGETEST"); imp = e ? atoi(e) : 0; }
    if (imp <= 0 || !car) return;
    const uint32_t tick = MR32(0x00606AC4u);
    if (tick < 300 || tick >= 380 || (tick - 300) % 10) return;
    static const float dirs[8][3] = { {1,0,0}, {-1,0,0}, {0,0,1}, {0,0,-1}, {0.7f,0,0.7f}, {-0.7f,0,0.7f}, {0.7f,0,-0.7f}, {-0.7f,0,-0.7f} };
    static uint32_t done[64][2]; static int nd;
    const uint32_t k = (tick - 300) / 10;
    for (int i = 0; i < nd; i++) if (done[i][0] == car && done[i][1] == k + 1) return;
    if (nd < 64){ done[nd][0] = car; done[nd][1] = k + 1; nd++; } else return;
    const uint32_t sp = c->esp;
    float point[3]; memcpy(point, g_mem + car + 0x440, 12);
    const float fi = (float)imp;
    memcpy(g_mem + car + 0x430, &fi, 4);
    const uint32_t an = put_vec(c, dirs[k]);
    const uint32_t base = c->esp;
    PUSHA(1); PUSHA(an); PUSHA(car); PUSHA(0);
    sub_00494df0(c);
    c->esp = sp; (void)base; (void)point;
    if (k == 0) fprintf(stderr, "[damage] test hits on car %08x, impact %d\n", car, imp);
}
/* 00498FF0, the per-car collision step (car+E2C), cdecl */
void sub_00498ff0(CPU *restrict c){
    const uint32_t car = MR32(c->esp + 4);
    damage_test(c, car);
    track_check();
    { static int init; if (!init){ init = 1; hs_ai_init(g_mem); } }
    hs_ai_set_converted(g_hs);
    if (!g_hs || !car){ sub_00498ff0_orig(c); return; }
    WallCtx w = { c, car };
    const HsWallGame g = { &w, wall_hit, wall_rebuild };
    hs_walls_step(g_mem + car, 1, &g);
    sub_00498ff0_orig(c);
    const uint32_t eax = c->eax;
    hs_walls_step(g_mem + car, 0, &g);
    c->eax = eax;
}

/* ---- car LOD (PC wrapper car_lod.cpp, L1 + L3). A car's .crp has LOD 1..7 per article; the closest race tier is LOD 2 (index 1) at
 * the highest detail, with simplified wheels, mirrors, interior and driver. LOD 1 is the garage model and its instance is in the
 * race anyway, so the closest tier draws it. Size reference 120 -> 60: patches/exe_v35.txt. ---- */
static int part_row(uint32_t article, uint32_t ident){
    const uint32_t flags = MR32(article + 4), count = MR32(article + 8), off = MR32(article + 0xC);
    uint32_t row = ((flags & 2) ? article + off * 16 : off) + (flags >> 5) * 16;
    for (uint32_t i = 0; i < count; i++, row += 16) if (MR32(row) == ident) return (int)row;
    return 0;
}
/* 0043A3B0, mov [esi+2C0h], ebx: esi = R3DCar, ebx = the clamped index */
/* CARLOD=0 (web/src/quality.ts, phones): the game's own tiers; the garage model costs a third of a race frame's work */
void pu_car_lod(CPU *c){
    static int on = -1;
    if (on < 0){ const char *e = getenv("CARLOD"); on = !(e && *e == '0'); }
    if (!on) return;
    const int index = (int)c->ebx, detail = (int)MR32(0x006197C4u), state = (int)MR32(0x00657424u);
    if (detail < 0 || detail > 2) return;
    const uint32_t info = MR32(c->esi + 0xF4);
    int base = (int)MR32(0x005CD5B8u + 4u * (uint32_t)detail);
    if (state >= 2 && detail != 0) base -= 1;                  /* SelectLOD's own step at race state >= 2 */
    int idx = index == base ? 0 : index;
    if (info){
        const uint32_t flags = MR32(info + 0x52C);
        if (flags & 0x10) idx = index;                         /* its base is 0 already */
        else if ((flags & 8) && detail == 0 && index == (int)MR32(0x005CD5B8u) + 1) idx = 0;    /* AI: base + 1 */
    }
    c->ebx = (uint32_t)idx;
}
/* L3: vertex-animated parts (the 911 rear spoiler grille) have only frame 0 right in LOD 1: the garage never moves them, the raise
 * frames lack vertices that LOD 2 has. Such an article draws its LOD 2 data inside the race's LOD 1 instance.
 * 0043C431, mov [esi+48h], eax: eax = the Base LOD record, esi = the instance, ebx = the R3DLOD */
void pu_car_lod_record(CPU *c){
    if (MR32(0x0065B928u) != 0 || MR32(c->ebx + 8) != 1 || !c->eax) return;     /* frontend, other LODs */
    const uint32_t article = MR32(c->esi), base = MR32(c->esi + 0x4C);
    if (!article || !base || g_mem[base + 0x4D] == 0) return;
    const int f1 = part_row(article, 0x76740011u), f2 = part_row(article, 0x76740012u);     /* vt, frame 1, LOD 1 / LOD 2 */
    if (!f1 || !f2 || (int)MR32(f1 + 8) >= (int)MR32(f2 + 8)) return;
    const uint32_t first = MR32(base + 8), last = MR32(base + 0xC);
    for (uint32_t k = first; k < last && k < 16; k++){
        const uint32_t r = base + 0x64 + k * 12;
        if (*(uint16_t *)(g_mem + r + 8) == 2){ c->eax = r; return; }
    }
}
/* 0043E120, the morph of an R3DLOD (ecx): it forms the frame info from the R3DLOD's LOD number, so the redirected instances run
 * through it again in a copy of the R3DLOD that carries their LOD number. */
#define MORPH_INSTS 256
static uint32_t g_morph_own, g_morph_other;     /* guest buffers of MORPH_INSTS * 0x54 */
static void morph_run(CPU *c, uint32_t lodobj, uint32_t lod, uint32_t insts, uint32_t count){
    const uint32_t ecx = c->ecx, sp = c->esp;
    c->esp -= 24;
    const uint32_t fake = c->esp;
    MR32(fake) = MR32(lodobj); MR32(fake + 4) = MR32(lodobj + 4);
    MR32(fake + 8) = lod; MR32(fake + 12) = insts; MR32(fake + 16) = count;
    c->ecx = fake;
    c->esp -= 4; MR32(c->esp) = 0;
    sub_0043e120_orig(c);
    c->esp = sp; c->ecx = ecx;
}
void sub_0043e120(CPU *restrict c){
    const uint32_t lodobj = c->ecx, lod = MR32(lodobj + 8), insts = MR32(lodobj + 0xC), count = MR32(lodobj + 0x10);
    int redirected = 0;
    for (uint32_t i = 0; i < count && !redirected; i++){
        const uint32_t rec = MR32(insts + i * 0x54 + 0x48);
        redirected = rec && *(uint16_t *)(g_mem + rec + 8) != (uint16_t)lod;
    }
    if (!redirected || count > MORPH_INSTS){ sub_0043e120_orig(c); return; }
    if (!g_morph_own){ g_morph_own = gheap_alloc(1, MORPH_INSTS * 0x54, 1); g_morph_other = gheap_alloc(1, MORPH_INSTS * 0x54, 1); }
    uint32_t own = 0, other = 0, other_lod = lod;
    for (uint32_t i = 0; i < count; i++){      /* only animated instances (Base+4D) are morphed */
        const uint32_t inst = insts + i * 0x54, b44 = MR32(inst + 0x44), rec = MR32(inst + 0x48);
        if (!b44 || !rec || g_mem[b44 + 9] == 0) continue;
        const uint32_t rl = *(uint16_t *)(g_mem + rec + 8);
        if (rl != lod) other_lod = rl;
        memcpy(g_mem + (rl == lod ? g_morph_own + own++ * 0x54 : g_morph_other + other++ * 0x54), g_mem + inst, 0x54);
    }
    if (own) morph_run(c, lodobj, lod, g_morph_own, own);
    if (other) morph_run(c, lodobj, other_lod, g_morph_other, other);
    POP32(c->eip);
}

/* ---- PHYS1 (PC wrapper collision_probe.cpp): in a network race a remote car's own physics velocity is not what it is doing; the
 * car-car solve (004980D0, cdecl a, b) takes the velocity of its last packet (cm/s), and the car keeps its own afterwards. ---- */
static int car_remote(uint32_t car){ return MR32(0x00657404u) != 0 && MR32(car + 0xDAC) != 0; }
static void car_packet_vel(uint32_t car){
    for (uint32_t i = 0; i < 3; i++){
        const float v = (float)*(int16_t *)(g_mem + car + 0x4A + i * 2) * 0.01f;
        memcpy(g_mem + car + 0x33C + i * 4, &v, 4);
    }
}
void sub_004980d0(CPU *restrict c){
    const uint32_t a = MR32(c->esp + 4), b = MR32(c->esp + 8);
    const int ra = car_remote(a), rb = car_remote(b);
    uint8_t own_a[12], own_b[12];
    if (ra){ memcpy(own_a, g_mem + a + 0x33C, 12); car_packet_vel(a); }
    if (rb){ memcpy(own_b, g_mem + b + 0x33C, 12); car_packet_vel(b); }
    sub_004980d0_orig(c);
    if (ra) memcpy(g_mem + a + 0x33C, own_a, 12);
    if (rb) memcpy(g_mem + b + 0x33C, own_b, 12);
}

/* ---- GP1 (PC wrapper enhanced/subsystems/gamepad): the page reads the Gamepad API and works the menus itself (web/src/input.ts); it
 * only needs to know which screen it is on. In a race the pad goes on top of what the bindings gave, just before the input frame's
 * packet is handed over. The native build has no pad: host_pad_read stays NULL. ---- */
int host_pad_read(uint32_t *buttons, float *lx, float *rt, float *lt) __attribute__((weak));
void host_pad_rumble(float low, float high, int ms) __attribute__((weak));
enum { PAD_A = 0, PAD_B = 1, PAD_X = 2, PAD_Y = 3, PAD_LB = 4, PAD_RB = 5 };    /* the standard mapping of the Gamepad API */
static volatile int g_race_depth;
void sub_004152e0(CPU *restrict c){      /* the race loop: its screens are the race's, what runs outside it is the frontend */
    g_race_depth++;
    sub_004152e0_orig(c);
    g_race_depth--;
    { void pu_race_camera_end(CPU *c); pu_race_camera_end(c); }
}
/* 0 frontend, 1 race menu (pause, results, options, the replay bar after the race), 2 driving. [00657424] == 2: the race is over
 * (00414850), the replay bar (HReplay [0060CA58]) takes the mouse under and after the result pages (KB2). */
int pu_pad_mode(void){
    if (g_race_depth <= 0) return 0;
    if (MR32(0x00606AA4u) != 0 || MR32(0x00606A90u) != 0 || MR32(0x00657424u) == 2u) return 1;
    const uint32_t pages = MR32(0x0060C9C4u);
    if (pages){
        const int32_t n = ((int32_t)MR32(pages + 4) - (int32_t)MR32(pages)) >> 3;
        if (n > 0 && n < 64) return 1;
    }
    return 2;
}
static uint32_t player_car(void){
    const int player = (int)MR32(0x0065741Cu);
    return player >= 0 && player < 16 ? MR32(CARS_L_RACER + 4u * (uint32_t)player) : 0;
}
static int guest_readable(uint32_t a){
#ifdef __wasm__
    return a >= 0x10000u && a < (uint32_t)__builtin_wasm_memory_size(0) * 65536u - 64u;     /* a race that loads holds garbage there */
#else
    return a >= 0x10000u;
#endif
}
/* The stick is direct, while keyboard steering eases toward full lock: car+558 is the driver record, +14 "digital steering". Not in
 * network races, the other side keeps its own copy of the flag. */
static uint32_t g_steer_driver; static int g_steer_digital, g_steer_direct;
static void pad_direct_steer(int direct){
    const uint32_t car = player_car();
    const uint32_t driver = car && guest_readable(car + 0x558) ? MR32(car + 0x558) : 0;
    if (driver != g_steer_driver){
        g_steer_driver = 0; g_steer_direct = 0;
        if (!direct || !guest_readable(driver)) return;
        g_steer_driver = driver;
        g_steer_digital = (int)MR32(driver + 0x14);
    }
    if (!driver || direct == g_steer_direct || !g_steer_digital) return;
    MR32(driver + 0x14) = direct ? 0 : (uint32_t)g_steer_digital;
    g_steer_direct = direct;
}
static int clampi(int v, int lo, int hi){ return v < lo ? lo : v > hi ? hi : v; }
/* 00413800 (entry): packet of player 0, byte 0: bit 0 hand brake, bits 3..7 event (2 up, 3 down, 5 reset); 1 steer -63..63; 2, 3 throttle, brake 0..126 */
void pu_pad_input(void){
    static uint32_t prev; static int pending; static int logged;
    uint32_t b = 0; float lx = 0, rt = 0, lt = 0;
    const int pad = host_pad_read && host_pad_read(&b, &lx, &rt, &lt);
    if (!pad){ if (host_pad_read && g_steer_direct) pad_direct_steer(0); return; }
    if (pu_pad_mode() != 2){ pad_direct_steer(0); prev = b; pending = 0; return; }
    uint8_t *packet = g_mem + 0x005E99DCu;
    const uint32_t rising = b & ~prev;
    const int key_steer = (int8_t)packet[1], pad_steer = clampi((int)lroundf(lx * 63.0f), -63, 63);
    const int pad_steers = abs(pad_steer) > abs(key_steer);
    if (pad_steers) packet[1] = (uint8_t)(int8_t)pad_steer;
    const int th = clampi((int)lroundf(rt * 126.0f), 0, 126), br = clampi((int)lroundf(lt * 126.0f), 0, 126);
    if (th > packet[2]) packet[2] = (uint8_t)th;
    if (br > packet[3]) packet[3] = (uint8_t)br;
    if (b & 1u << PAD_A) packet[0] |= 1;
    /* one event per input frame, a pad event waits while the keyboard has one */
    if (rising & 1u << PAD_RB) pending = 2;
    else if (rising & 1u << PAD_LB) pending = 3;
    else if (rising & 1u << PAD_Y) pending = 5;
    if (pending && !(packet[0] >> 3)){ packet[0] = (uint8_t)((packet[0] & 7) | pending << 3); pending = 0; }
    /* look behind is held, the camera changes on the rising edge the game finds itself */
    const uint32_t bits = 0x005E954Cu + 4u * (MR32(0x005E99B8u) & 31u);
    if (b & 1u << PAD_X) MR32(bits) |= 1u << 19;
    if (b & 1u << PAD_B) MR32(bits) |= 1u << 23;
    prev = b;
    if (pad_steers) pad_direct_steer(MR32(0x005E99F4u) != 2);
    else if (key_steer) pad_direct_steer(0);
    if (!logged && (pad_steers || th || br)){ logged = 1; fprintf(stderr, "[pad] the pad drives: packet %02X %02X %02X %02X\n", packet[0], packet[1], packet[2], packet[3]); }
    { static int pl = -1; static time_t last; if (pl < 0) pl = getenv("PADLOG") != 0;     /* PADLOG=1: the packet once a second */
      if (pl && time(0) != last){ last = time(0);
          fprintf(stderr, "[pad] packet %02X %02X %02X %02X (pad buttons %08X lx %.2f rt %.2f lt %.2f)\n", packet[0], packet[1], packet[2], packet[3], b, lx, rt, lt); } }
}
/* 0040F740 (entry), the car's tick: it turns car+430 (magnitude) and car+438 (high word: 3 ground, 4 wall, 5 car) into a sound over 1.5
 * and clears them. Magnitudes of full strength from the impacts of 129 traced races. */
void pu_pad_impact(CPU *c){
    if (!host_pad_rumble) return;
    const uint32_t car = MR32(c->esp + 4);
    if (!car || (MR32(car + 0x52C) & 0x400u) || car != player_car()) return;
    float mag; memcpy(&mag, g_mem + car + 0x430, 4);
    if (!(mag > 1.5f)) return;
    const uint32_t group = MR32(car + 0x438) >> 16;
    const float full = group == 3 ? 18.0f : group == 4 ? 50.0f : group == 5 ? 25.0f : 30.0f;
    const float low = group == 5 ? 0.8f : 1.0f, high = group == 3 ? 0.3f : group == 4 ? 0.6f : group == 5 ? 0.8f : 0.5f;
    float range = (mag - 1.5f) / (full - 1.5f);
    range = range < 0 ? 0 : range > 1 ? 1 : range;
    const float strength = 0.12f + 0.88f * powf(range, 0.6f);
    host_pad_rumble(strength * low, strength * high, 90 + (int)(210.0f * strength));
}

/* ---- OS1 (PC wrapper opponent_similar.cpp): the opponent list of Quick Race gets "similar", "similar stock" and "similar tuned" on
 * top; the game's five modes shift down to 3..7. FEOpponentCarAccessor (vtable 005B77D8) has 7 opponent objects of 0x760 bytes at
 * this+24h, its value is at +18h. The counts: patches/exe_v35.txt. ---- */
void dispatch_indirect(CPU *c, uint32_t target);
static uint32_t guest_call(CPU *c, uint32_t fn, uint32_t ecx, const uint32_t *args, int n){
    const uint32_t sp = c->esp, keep = c->ecx;
    for (int i = n - 1; i >= 0; i--) PUSH32(args[i]);
    PUSH32(0);
    c->ecx = ecx;
    dispatch_indirect(c, fn);
    const uint32_t r = c->eax;
    c->esp = sp; c->ecx = keep;
    return r;
}
uint32_t pu_guest_call(CPU *c, uint32_t fn, uint32_t ecx, const uint32_t *args, int n){ return guest_call(c, fn, ecx, args, n); }
static uint32_t guest_vcall(CPU *c, uint32_t obj, uint32_t off){ return guest_call(c, MR32(MR32(obj) + off), obj, NULL, 0); }
static uint32_t guest_string(const char *text){
    const size_t n = strlen(text) + 1;
    const uint32_t a = gheap_alloc(1, (uint32_t)n, 1);
    memcpy(g_mem + a, text, n);
    return a;
}
/* ---- CAM1: the race camera the player picks with the camera key becomes the profile's camera, so the next race and the
 * next session start with it.  The options' cameras page (cameras.lay, FECameraSelect 004BF2A0) holds the cycle the camera
 * key steps through: "Camera 1".."Camera 4" (accessors Camera.1..4, a camera position each: 0 bumper, 1 bumper 2, 2 chase
 * near, 3 chase far; the defaults 0..3), Camera.Current is only the page's selected slot.  The race setup sends the four to
 * the player's car (004EEDA0, setup field 148h) and the race starts with slot 1; view 0 starts with carInfo[0].CAMERA
 * (00462F30 -> 00465790) and the key changes view 0's camera (00413960 -> 00465790(0, camera)).
 * So a pick turns the cycle round until the picked position is Camera 1, the order kept (pick 2 of 0,1,2,3 -> 2,3,0,1), the
 * same values the page writes.  The profile's accessors are gone during the race (004B7AB0 finds none), so the pick waits for
 * the frontend's first tick after the race (004D2B80), is written there through the accessors' own set (vfn 2Ch) and the
 * profile saved as the game saves it (00510AF0 -> 004DEDA0 "Saving for %s"); the cloud sync sends that save like any other.
 * Only view 0 inside the race loop (004152E0) counts, and only a change after the race's first camera (a key press, not
 * the start): the intro (14), the finish camera (10) and the network spectator camera (10) are not 0..3, and a replay (its
 * own pass of the race loop, mode 2) is skipped.  carInfo[0].CAMERA follows the pick (a restart keeps it), not in a network
 * race.  CAMERA_KEEP=0 turns it off, CAMLOG=1 logs. ---- */
#define CAM_VIEW0      0x00627F3Cu     /* view 0's camera, 16 bit (0x10C per view) */
#define CAM_SLOT1      0x005D7AD8u     /* "Camera.1"; "Camera.2" .. "Camera.4" at -0Ch steps */
#define CAM_VTABLE     0x005B52E4u     /* their class: the list accessor of 004BC1F0 (rows +14h, row +18h, set vfn 2Ch) */
#define FIND_ACCESSOR  0x004B7AB0u
#define SAVE_PROFILE   0x00510AF0u     /* the current player's profile (.sav) */
#define CAR_CAMERA     (CARS_INFO + 0x208u)  /* carInfo .CAMERA (16 bit) */
#define LOOP_MODE      0x005E99F4u     /* the race loop's mode, copied from 00657424 by 00413CF0: 2 = a replay */
static int g_cam_prev = -1, g_cam_pick = -1;
static int cam_on(void){ static int on = -1; if (on < 0){ const char *e = getenv("CAMERA_KEEP"); on = !(e && *e == '0'); } return on; }
static int cam_log(void){ static int on = -1; if (on < 0){ const char *e = getenv("CAMLOG"); on = e && *e && *e != '0'; } return on; }
void pu_race_camera(CPU *c){
    (void)c;
    { static int lastv = -99;
      const int v0 = (int16_t)MR16(CAM_VIEW0);
      if (cam_log() && v0 != lastv){ lastv = v0; fprintf(stderr, "[camera] view 0 camera %d, the car's cycle %d,%d,%d,%d\n", v0,
          (int)MR32(CAR_CAMERA), (int)MR32(CAR_CAMERA + 4u), (int)MR32(CAR_CAMERA + 8u), (int)MR32(CAR_CAMERA + 12u)); } }
    if (!cam_on() || g_race_depth <= 0 || MR32(LOOP_MODE) == 2u) return;      /* 2: a replay (00413CF0 loads replay.rpl) */
    const int cur = (int16_t)MR16(CAM_VIEW0);
    if (cur == g_cam_prev) return;
    /* only a camera of the car's cycle (the intro 14, the finish and spectator 10 are not in it); nothing is written in the
     * race: the cycle is carInfo +208h..+214h (4 x int32, the race starts with the first) and the key steps through it */
    int in = 0;
    for (int i = 0; i < 4; i++) in |= (int)MR32(CAR_CAMERA + 4u * (uint32_t)i) == cur;
    if (!in) return;
    if (g_cam_prev >= 0){
        g_cam_pick = cur;
        if (cam_log()) fprintf(stderr, "[camera] race camera %d picked (was %d)\n", cur, g_cam_prev);
    } else if (cam_log()) fprintf(stderr, "[camera] race starts with camera %d\n", cur);
    g_cam_prev = cur;
}
/* the race loop is left (the end, a restart): a restart starts with the car's cycle as it is, so in a local race the cycle turns
 * round to the pick there, the order kept */
void pu_race_camera_end(CPU *c){
    (void)c;
    if (cam_log()) fprintf(stderr, "[camera] race loop left, pick %d" "\n", g_cam_pick);
    if (g_cam_pick >= 0 && MR32(0x006573E8u) != 3u && MR32(LOOP_MODE) != 2u){
        int cyc[4], k = -1;
        for (int i = 0; i < 4; i++){ cyc[i] = (int)MR32(CAR_CAMERA + 4u * (uint32_t)i); if (k < 0 && cyc[i] == g_cam_pick) k = i; }
        if (k > 0){
            for (int i = 0; i < 4; i++) MR32(CAR_CAMERA + 4u * (uint32_t)i) = (uint32_t)cyc[(i + k) & 3];
            if (cam_log()) fprintf(stderr, "[camera] the car's cycle %d,%d,%d,%d turned to start with %d\n", cyc[0], cyc[1], cyc[2], cyc[3], g_cam_pick);
        }
    }
    g_cam_prev = -1;
}
/* the options' rows (Camera.1..4: 0 bumper, 1 cockpit, 2 chase near, 3 chase far) and the race's cameras: row 3 is camera 7 */
static int cam_row(int camera){ return camera == 7 ? 3 : camera >= 0 && camera <= 2 ? camera : -1; }
/* the frontend's tick (004D2B80, a thiscall entry: eax and edx are free) */
void pu_race_camera_fe(CPU *c){
    if (g_cam_pick < 0 || g_race_depth > 0) return;
    uint32_t acc[4];
    for (int i = 0; i < 4; i++){
        const uint32_t name = CAM_SLOT1 - 0x0Cu * (uint32_t)i;
        acc[i] = guest_call(c, FIND_ACCESSOR, 0, &name, 1);
        if (!acc[i]) return;                                  /* not yet: the profile's accessors come with the frontend */
    }
    const int pick = cam_row(g_cam_pick);
    g_cam_pick = -1;
    if (pick < 0){ fprintf(stderr, "[camera] the picked camera has no options row: not saved\n"); return; }
    int had[4], k = -1;
    for (int i = 0; i < 4; i++){
        if (MR32(acc[i]) != CAM_VTABLE || MR32(acc[i] + 0x14) != 4u){ fprintf(stderr, "[camera] Camera.%d is not the 4-row list (vtable %08x): not written\n", i + 1, MR32(acc[i])); return; }
        had[i] = (int)MR32(acc[i] + 0x18);
        if (k < 0 && had[i] == pick) k = i;
    }
    /* the whole cycle turns round to the pick, the order kept; a camera already earlier in the cycle (a duplicate the
     * profile had, or the pick when it was in no slot) gives its place to the first camera the cycle lacks, so the four
     * slots always hold four different cameras */
    int rot[4], want[4], n = 0;
    for (int i = 0; i < 4; i++) rot[i] = k > 0 ? had[(i + k) & 3] : i == 0 ? pick : had[i];
    for (int i = 0; i < 4; i++){
        int dup = rot[i] < 0 || rot[i] > 3;
        for (int j = 0; j < n && !dup; j++) dup = want[j] == rot[i];
        want[n++] = dup ? -1 : rot[i];
    }
    for (int i = 0; i < 4; i++) if (want[i] < 0)
        for (int m = 0; m < 4; m++){ int used = 0; for (int j = 0; j < 4; j++) used |= want[j] == m; if (!used){ want[i] = m; break; } }
    if (!memcmp(want, had, sizeof want)){ if (cam_log()) fprintf(stderr, "[camera] the profile's Camera 1 is already %d\n", pick); return; }
    for (int i = 0; i < 4; i++){
        const uint32_t v = (uint32_t)want[i];
        if (had[i] != want[i]) guest_call(c, MR32(CAM_VTABLE + 0x2C), acc[i], &v, 1);
        if ((int)MR32(acc[i] + 0x18) != want[i]){ fprintf(stderr, "[camera] Camera.%d did not take %d: not saved\n", i + 1, want[i]); return; }
    }
    guest_call(c, SAVE_PROFILE, 0, NULL, 0);
    fprintf(stderr, "[camera] picked in the race: camera %d; the profile's cameras %d,%d,%d,%d -> %d,%d,%d,%d, profile saved\n",
            pick, had[0], had[1], had[2], had[3], want[0], want[1], want[2], want[3]);
}
typedef struct { CPU *c; uint32_t self; } OppCtx;
static void opp_set_slot(void *ctx, int slot, const uint8_t *rec, uint32_t ext, uint32_t stripe, uint32_t trim, uint32_t extra, int package, int number){
    OppCtx *o = ctx;
    const uint32_t args[7] = { (uint32_t)(rec - g_mem), ext, stripe, trim, extra, (uint32_t)package, (uint32_t)number };
    guest_call(o->c, 0x004EEBA0u, o->self + 0x24 + (uint32_t)slot * 0x760, args, 7);
}
static void opp_install(void *ctx, int slot, const int16_t *ids, int count){
    static uint32_t buf;
    if (!buf) buf = gheap_alloc(1, 64 * 2, 1);
    if (count > 64) count = 64;
    OppCtx *o = ctx;
    memcpy(g_mem + buf, ids, (size_t)count * 2);
    const uint32_t args[3] = { o->self + 0x24 + (uint32_t)slot * 0x760, (uint32_t)count, buf };
    guest_call(o->c, 0x004ED880u, 0, args, 3);
}
static int opp_run(CPU *c, uint32_t self, int mode){
    const uint32_t name = 0x005D71A8u;      /* "zMergedList.Car" */
    const uint32_t acc = guest_call(c, 0x004B7AB0u, 0, &name, 1);
    const uint32_t cast[5] = { acc, 0, 0x005CD6A8u, 0x005D9468u, 0 };
    const uint32_t merged = acc ? guest_call(c, 0x005A134Bu, 0, cast, 5) : 0;
    if (!merged) return 0;
    const uint32_t player = guest_vcall(c, merged, 0x90);
    if (!player) return 0;
    const int owned = (guest_call(c, 0x004F4DF0u, merged, NULL, 0) & 0xFF) != 0;
    const uint32_t obj = owned ? guest_vcall(c, merged, 0x8C) : 0;
    const uint32_t list = guest_call(c, MR32(self + 0xCF2 * 4), 0, NULL, 0);     /* the racer records of nfs5.car */
    if (!list || !MR32(list) || MR32(list + 4) <= MR32(list)) return 0;
    OppCtx ctx = { c, self };
    OppGame g;
    memset(&g, 0, sizeof g);
    g.mem = g_mem;
    g.player_rec = g_mem + player;
    g.owned_obj = obj ? g_mem + obj : NULL;
    g.list = (const uint32_t *)(g_mem + MR32(list));
    g.list_size = (int)((MR32(list + 4) - MR32(list)) / 4);
    g.part_table = g_mem + MR32(0x00628F84u);
    g.part_count = (int)MR32(0x00628F80u);
    g.sim_dir = (const char *)(g_mem + MR32(0x0065B2ACu));
    g.packages = (int)MR32(0x006115F8u);
    g.ctx = &ctx; g.set_slot = opp_set_slot; g.install = opp_install;
    return opp_similar_run(&g, mode);
}
/* ---- CARS2: opponents 8..15 in menu races.  FEOpponentCarAccessor holds 7 opponent cars inline (this+24h, 760h each, then
 * its fields at +33C4h/+33C8h), so the cars past them live in two shadow accessors: a block of the accessor's size with a
 * copy of its header (vtable, mode) and fields, and 7 opponent cars built by their constructor 004EE5C0.  Generate runs
 * on the real accessor, then on each shadow (every mode, OS1's included, fills them its own way, and the cars register
 * in the game's car list 0069188C under their own addresses); the slot getter 004F5850 hands out slot 7+ from them. ---- */
#define OPP_SLOTS 7u
#define OPP_SIZE 0x33CCu
#define OPP_SHADOWS 2
static uint32_t g_opp_shadow[OPP_SHADOWS], g_opp_owner;
static int g_opp_nested;
static void opp_shadows(CPU *c, uint32_t real){
    if (g_opp_nested) return;
    g_opp_nested = 1; g_opp_owner = real;
    for (int k = 0; k < OPP_SHADOWS; k++){
        if (!g_opp_shadow[k]){
            const uint32_t s = gheap_alloc(1, OPP_SIZE, 1);
            if (!s) break;
            for (uint32_t i = 0; i < OPP_SLOTS; i++) guest_call(c, 0x004EE5C0u, s + 0x24 + i * 0x760, NULL, 0);
            g_opp_shadow[k] = s;
        }
        const uint32_t s = g_opp_shadow[k];
        memcpy(g_mem + s, g_mem + real, 0x24);
        MR32(s + 0x33C4) = MR32(real + 0x33C4); MR32(s + 0x33C8) = MR32(real + 0x33C8);
        guest_call(c, 0x004F4FA0u, s, NULL, 0);
    }
    g_opp_nested = 0;
}
void sub_004f5850(CPU *restrict c){      /* the opponent car of a slot, thiscall(slot) */
    const uint32_t self = c->ecx, i = MR32(c->esp + 4);
    uint32_t r = self + 0x24 + i * 0x760;
    if (self == g_opp_owner && i >= OPP_SLOTS && i < OPP_SLOTS * (1 + OPP_SHADOWS) && g_opp_shadow[(i - OPP_SLOTS) / OPP_SLOTS])
        r = g_opp_shadow[(i - OPP_SLOTS) / OPP_SLOTS] + 0x24 + ((i - OPP_SLOTS) % OPP_SLOTS) * 0x760;
    c->eax = r;
    POP32(c->eip); c->esp += 4;
}
/* The AI drivers of a race (cdecl(count, difficulty), 004E3160): the game draws `count` distinct drivers of that difficulty
 * and divides by the pool left, so a pool smaller than the count (7 easy, 7 medium, 21 hard of 35) is a division by
 * zero.  Then: the stock call for any difficulty gives a vector of the right size, filled here with every driver of the
 * difficulty first and the rest from the nearest one (a tie: the harder), in random order within each. */
void sub_004e3160(CPU *restrict c){
    const int n = (int)MR32(c->esp + 4), diff = (int)MR32(c->esp + 8);
    const int count = (int)MR32(0x006907D0u);
    const uint32_t tab = MR32(0x006907D4u);
    int pool = 0;
    if (diff != -1 && tab && count > 0 && count <= 256)
        for (int i = 0; i < count; i++) if ((int16_t)MR16(tab + 8 + (uint32_t)i * 0x2C) == diff) pool++;
    if (diff == -1 || n <= pool || n > count || count > 256){ sub_004e3160_orig(c); return; }
    MR32(c->esp + 8) = 0xFFFFFFFFu;
    sub_004e3160_orig(c);
    const uint32_t v = c->eax, b = v ? MR32(v) : 0, e = v ? MR32(v + 4) : 0;
    if (!b || (int)((e - b) / 2) != n) return;
    int order[256], keys[256];
    for (int i = 0; i < count; i++){
        const int d = (int16_t)MR16(tab + 8 + (uint32_t)i * 0x2C);
        keys[i] = abs(d - diff) * 2 + (d < diff) ;
        order[i] = i;
    }
    for (int i = count - 1; i > 0; i--){ const int j = rand() % (i + 1), t = order[i]; order[i] = order[j]; order[j] = t; }
    for (int i = 1; i < count; i++){      /* stable by key: the shuffle stays within a key */
        const int t = order[i]; int j = i - 1;
        while (j >= 0 && keys[order[j]] > keys[t]){ order[j + 1] = order[j]; j--; }
        order[j + 1] = t;
    }
    for (int k = 0; k < n; k++) MR16(b + 2u * (uint32_t)k) = MR16(tab + (uint32_t)order[k] * 0x2C);
    fprintf(stderr, "[cars2] %d drivers for difficulty %d (%d of it)\n", n, diff, pool);
}
void sub_004f4fa0(CPU *restrict c){      /* Generate, ecx = the accessor */
    const uint32_t self = c->ecx;
    MR32(self + 0x14) = OPP_MODE_COUNT;
    const int menu = (int)MR32(self + 0x18);
    int game = menu - OPP_MODES_OURS;
    if (menu >= 0 && menu < OPP_MODES_OURS){
        if (opp_run(c, self, menu)){ opp_shadows(c, self); POP32(c->eip); return; }
        game = 4;                            /* its random car */
    }
    if (game >= 0 && game <= 4) MR32(self + 0x18) = (uint32_t)game;
    sub_004f4fa0_orig(c);
    MR32(self + 0x18) = (uint32_t)menu;
    opp_shadows(c, self);
}
void sub_004f5820(CPU *restrict c){      /* SetRange: 4 modes on low memory, 5 otherwise */
    const uint32_t count = OPP_MODE_COUNT;
    guest_call(c, 0x004BC310u, c->ecx, &count, 1);
    POP32(c->eip);
}
static uint32_t opp_name(CPU *c, int mode, int short_text){
    static uint32_t ours[2][OPP_MODES_OURS];
    static const char *const text[2][OPP_MODES_OURS] = {
        { "similar cars", "similar stock cars", "similar tuned cars" }, { "similar", "similar stock", "similar tuned" } };
    static const uint32_t ids[2][5] = { { 640, 642, 643, 644, 641 }, { 640, 642, 643, 645, 641 } };
    if (mode >= 0 && mode < OPP_MODES_OURS){
        if (!ours[short_text][mode]) ours[short_text][mode] = guest_string(text[short_text][mode]);
        return ours[short_text][mode];
    }
    const int game = mode - OPP_MODES_OURS;
    if (game < 0 || game > 4) return 0x005DACACu;   /* "bug in car list" */
    return guest_call(c, 0x004E06D0u, 0, &ids[short_text][game], 1);
}
void sub_004f5720(CPU *restrict c){      /* the long and the short text of a mode, thiscall(mode) */
    const uint32_t r = opp_name(c, (int)MR32(c->esp + 4), 0);
    c->eax = r;
    POP32(c->eip); c->esp += 4;
}
void sub_004f57a0(CPU *restrict c){
    const uint32_t r = opp_name(c, (int)MR32(c->esp + 4), 1);
    c->eax = r;
    POP32(c->eip); c->esp += 4;
}

/* ---- NE1 HUD scaling (PC wrapper enhanced/subsystems/hud_scaling): the HUD code reads a virtual size of one scale for both axes, so the
 * layout records stay the 640x480 ones and the HUD's orthographic camera stretches them. The text keeps its own pixel size, so the
 * fonts go up by the same factor while the HUD's text loop runs; fonts are shared with the frontend and get their scale back. ---- */
#define HUD_VW 0x006C1F00u
#define HUD_VH 0x006C1F04u
#define HUD_REF_HEIGHT 480
#define HUD_SCALE 0.67f     /* PC dx11z.ini [hud] scale: 1 would keep the 640x480 proportions */
static float g_hud_fx = 1.0f, g_hud_fy = 1.0f;
static int g_hud_w0;        /* the screen width the race's HUD was laid out for; a live resize keeps it (race_reshape) */
/* a piece of the HUD laid out on a screen g_hud_w0 wide, from x0 to x1, on the screen as it is now: it keeps its side */
static float hud_anchor_x(float x0, float x1){
    const int sw = (int)MR32(0x00619784u);
    if (g_hud_w0 <= 0 || sw == g_hud_w0) return x0;
    const float c = (x0 + x1) * 0.5f / (float)g_hud_w0;
    const float side = c < 1.0f / 3.0f ? 0.0f : c > 2.0f / 3.0f ? 1.0f : 0.5f;
    return x0 + (float)(sw - g_hud_w0) * side;
}
static void hud_virtualize(int w, int h, int *vw, int *vh){
    float scale = (float)h / HUD_REF_HEIGHT * HUD_SCALE;
    if (scale < 1.0f) scale = 1.0f;
    *vw = (int)lroundf((float)w / scale); *vh = (int)lroundf((float)h / scale);
}
#define DRAW_LIMIT1 0x006C1F08u
#define DRAW_LIMIT2 0x006C1F0Cu
/* DRAW_X=<scale>: 2 as on the PC; 1 = the game's own distance (web/src/quality.ts, phones: half a race frame's work goes on the far half) */
static float draw_x(void){
    static float x = -1.0f;
    if (x < 0.0f){ const char *e = getenv("DRAW_X"); x = e && *e ? (float)atof(e) : 2.0f; if (x < 0.25f || x > 4.0f) x = 2.0f; }
    return x;
}
#define DRAW_SCALE draw_x()
#define DRAW_MIN (draw_x() >= 2.0f ? 20000.0f : 0.0f)
/* The two object classes with fixed limits (600 and 700, squared, in 0047F890) scale along and never end before the rest, otherwise
 * ground and walls go missing under buildings that are still drawn. */
static void draw_limits(float drawn){
    const float a = 600.0f * DRAW_SCALE, b = 700.0f * DRAW_SCALE;
    const float l1 = a > drawn ? a : drawn, l2 = b > drawn ? b : drawn;
    const float s1 = l1 * l1, s2 = l2 * l2;
    memcpy(g_mem + DRAW_LIMIT1, &s1, 4); memcpy(g_mem + DRAW_LIMIT2, &s2, 4);
}
/* 0044F1A0, cdecl(distance): the race's draw distance, stored at 006197CC with the view distance option on top */
void sub_0044f1a0(CPU *restrict c){
    float d;
    memcpy(&d, g_mem + c->esp + 4, 4);
    d *= DRAW_SCALE;
    if (d < DRAW_MIN) d = DRAW_MIN;
    memcpy(g_mem + c->esp + 4, &d, 4);
    sub_0044f1a0_orig(c);
    float drawn;
    memcpy(&drawn, g_mem + 0x006197CCu, 4);
    draw_limits(drawn);
}
/* 0044E324, 0044EC58: the mode setters, just after the screen size was written */
static void rects_forget(void);
void pu_hud_mode(void){
    rects_forget();
    const int w = (int)MR32(0x00619784u), h = (int)MR32(0x00619788u);
    int vw = w, vh = h;
    if (w > 0 && h > 0) hud_virtualize(w, h, &vw, &vh);
    MR32(HUD_VW) = (uint32_t)vw; MR32(HUD_VH) = (uint32_t)vh;
    g_hud_w0 = w;
    if (!MR32(DRAW_LIMIT1)) draw_limits(0.0f);
    g_hud_fx = vw > 0 ? (float)w / (float)vw : 1.0f;
    g_hud_fy = vh > 0 ? (float)h / (float)vh : 1.0f;
}
/* The race's menus (pause, results: the pages of [0060C9C4]; the replay bar after the race) are laid out on the HUD's virtual
 * screen and hit-tested there, while the mouse comes in the race mode's pixels: a click on a button as drawn missed it (it hit
 * only up and left of it, by the HUD scale). input_web.c puts the mouse into the layout's space while such a page is up: the inverse of the HUD's stretch and of
 * hud_anchor_x (the point's third of the screen picks the side). */
void pu_race_menu_point(int *x, int *y){
    if (pu_pad_mode() != 1 || g_hud_w0 <= 0 || (g_hud_fx == 1.0f && g_hud_fy == 1.0f)) return;
    const uint32_t pages = MR32(0x0060C9C4u);
    if ((!pages || MR32(pages + 4) == MR32(pages)) && MR32(0x00657424u) != 2u) return;
    const int sw = (int)MR32(0x00619784u), vw = (int)MR32(HUD_VW), vh = (int)MR32(HUD_VH);
    if (sw <= 0 || vw <= 0 || vh <= 0) return;
    const float c = (float)*x / (float)sw, side = c < 1.0f / 3.0f ? 0.0f : c > 2.0f / 3.0f ? 1.0f : 0.5f;
    int vx = (int)lroundf(((float)*x - (float)(sw - g_hud_w0) * side) / g_hud_fx), vy = (int)lroundf((float)*y / g_hud_fy);
    *x = vx < 0 ? 0 : vx >= vw ? vw - 1 : vx;
    *y = vy < 0 ? 0 : vy >= vh ? vh - 1 : vy;
}
/* 0041E170, the layout loader, cdecl (name, set, width, height): the race and the Display page ask for the same record */
void sub_0041e170(CPU *restrict c){
    int vw, vh;
    hud_virtualize((int)MR32(c->esp + 12), (int)MR32(c->esp + 16), &vw, &vh);
    MR32(c->esp + 12) = (uint32_t)vw; MR32(c->esp + 16) = (uint32_t)vh;
    sub_0041e170_orig(c);
}
/* 004CBBDE, 004CBBF9: the Display page's preview, eax = the selected race size just read; the race modes are 4:3 */
void pu_hud_preview(CPU *c, int height){
    const uint32_t ref = height ? 480u : 640u;
    if (c->eax > ref) c->eax = ref;
}
static void font_set_scale(CPU *c, uint32_t font, float x, float y){
    const uint32_t eax = c->eax, edx = c->edx;
    uint32_t args[2];
    memcpy(&args[0], &x, 4); memcpy(&args[1], &y, 4);
    guest_call(c, 0x00536CE0u, font, args, 2);
    c->eax = eax; c->edx = edx;
}
#define HUD_FONTS 16
static struct { uint32_t font; float x, y; } g_hud_font[HUD_FONTS];
static uint32_t g_hud_scaled; static float g_hud_keep_x, g_hud_keep_y;
/* 0041A495 in the HUD's text loop (0041A330): esi = the text entry, edi = its font holder. The loop calls set scale only when the
 * entry's scale is not 1, a font otherwise stays as it was: the last scale the game gave it counts. */
void pu_hud_text_begin(CPU *c){
    if (g_hud_fx == 1.0f && g_hud_fy == 1.0f) return;
    const uint32_t font = MR32(c->edi + 4);
    if (!font) return;
    float ex, ey;
    memcpy(&ex, g_mem + c->esi + 0x18, 4); memcpy(&ey, g_mem + c->esi + 0x1C, 4);
    int i = 0;
    while (i < HUD_FONTS && g_hud_font[i].font && g_hud_font[i].font != font) i++;
    if (i == HUD_FONTS) i = 0;
    if (g_hud_font[i].font != font){ g_hud_font[i].font = font; g_hud_font[i].x = g_hud_font[i].y = 1.0f; }
    if (ex != 1.0f || ey != 1.0f){ g_hud_font[i].x = ex; g_hud_font[i].y = ey; }
    g_hud_scaled = font; g_hud_keep_x = g_hud_font[i].x; g_hud_keep_y = g_hud_font[i].y;
    font_set_scale(c, font, g_hud_keep_x * g_hud_fx, g_hud_keep_y * g_hud_fy);
}
/* 0041A4AE, after the draw */
void pu_hud_text_end(CPU *c){
    if (!g_hud_scaled) return;
    font_set_scale(c, g_hud_scaled, g_hud_keep_x, g_hud_keep_y);
    g_hud_scaled = 0;
}

/* ---- Widescreen (PC wrapper camera_hooks.cpp, P0-P7): Hor+ for a race mode wider than 4:3. The game's two perspective calls
 * (0046700C, 00467162 -> 005391B0, thiscall(fullDeg, aspect, near, far)) get the widened angle and aspect, the camera's
 * sphere test planes (C+F8..104) follow after the call; the activation replay (0053912E) only redoes the planes. The tree
 * traversal's footprint (0047F550, 0047F210) and the sky dome's edge directions (0045AAF0) read the view's half angle
 * (V+130) themselves: it is the widened one for the length of the call. A 4:3 mode keeps the game's own values. ---- */
#define CAM_PI 3.14159265358979323846
typedef struct {
    uint32_t camera, eff_deg, eff_aspect;      /* the camera, the bits of the angle and aspect it was given */
    float authored_deg, authored_aspect, tan_x, tan_y;
    int changed;
} CamRec;
#define CAM_RECS 16
static CamRec g_cam[CAM_RECS];
static int g_cam_next;
int g_cam_capture;                  /* pu_cube.c: a cube face is being drawn: exactly 90 degrees on both axes */
static uint32_t g_cam_call;                /* the camera of the perspective call in progress */
static float g_cam_view_keep; static uint32_t g_cam_view;      /* V+130 put back after the tree / sky call */
static int g_tree_on; static float g_tree_half, g_tree_ratio;
static uint32_t fbits(float f){ uint32_t b; memcpy(&b, &f, 4); return b; }
static float rdf(uint32_t a){ float f; memcpy(&f, g_mem + a, 4); return f; }
static void wrf(uint32_t a, float f){ memcpy(g_mem + a, &f, 4); }
static float cam_target_aspect(void){
    const uint32_t w = MR32(0x00619784u), h = MR32(0x00619788u);
    return w && h ? (float)w / (float)h : 640.0f / 480.0f;
}
static CamRec *cam_find(uint32_t camera){
    for (int i = 0; i < CAM_RECS; i++) if (g_cam[i].camera == camera) return &g_cam[i];
    return NULL;
}
static void cam_sides(uint32_t camera, float full_deg, float aspect){
    const double tx = tan((double)full_deg * CAM_PI / 360.0), ty = tx / (double)aspect;
    wrf(camera + 0xF8, (float)tx); wrf(camera + 0xFC, (float)(1.0 / sqrt(1.0 + tx * tx)));
    wrf(camera + 0x100, (float)ty); wrf(camera + 0x104, (float)(1.0 / sqrt(1.0 + ty * ty)));
}
/* before 0046700C / 00467162: ecx = the camera, [esp] = the full angle, [esp+4] = the aspect */
void pu_cam_before(CPU *c){
    g_cam_call = 0;
    const uint32_t camera = c->ecx;
    const float deg = rdf(c->esp), asp = rdf(c->esp + 4), target = cam_target_aspect();
    if (!camera || !(deg > 0.0f && deg < 179.0f) || !(asp > 0.0f) || !isfinite(deg) || !isfinite(asp)) return;
    if (g_cam_capture){
        CamRec *q = cam_find(camera);
        if (q) memset(q, 0, sizeof *q);
        wrf(c->esp, 90.0f); wrf(c->esp + 4, 1.0f);
        g_cam_call = camera;
        return;
    }
    const double t0 = tan((double)deg * CAM_PI / 360.0);
    const double a = (double)asp * (double)target / (640.0 / 480.0);
    const double ty = t0 / (double)asp, tx = ty * a;
    CamRec *r = cam_find(camera);
    if (!r){ r = &g_cam[g_cam_next]; g_cam_next = (g_cam_next + 1) % CAM_RECS; }
    memset(r, 0, sizeof *r);
    r->camera = camera; r->authored_deg = deg; r->authored_aspect = asp; r->tan_x = (float)tx; r->tan_y = (float)ty;
    r->changed = fabs(a - (double)asp) > 1e-6 * (double)asp;
    float eff_deg = deg, eff_asp = asp;
    if (r->changed){ eff_deg = (float)(2.0 * atan(tx) * 180.0 / CAM_PI); eff_asp = (float)a; }
    r->eff_deg = fbits(eff_deg); r->eff_aspect = fbits(eff_asp);
    wrf(c->esp, eff_deg); wrf(c->esp + 4, eff_asp);
    static int logs = 6;
    static float logged_asp;   /* and the main camera's (authored 4:3) whenever the screen's shape changes */
    const int main_cam = fabsf(asp - 4.0f / 3.0f) < 1e-3f;
    if (r->changed && (logs > 0 || (main_cam && fabsf(eff_asp - logged_asp) > 1e-3f))){ if (logs > 0) logs--; if (main_cam) logged_asp = eff_asp; fprintf(stderr, "[widescreen] camera 0x%08x %.2f deg aspect %.4f -> %.2f deg aspect %.4f\n", camera, deg, asp, eff_deg, eff_asp); }
    g_cam_call = camera;
}
/* after them (00467011 / 00467167) */
void pu_cam_after(CPU *c){
    (void)c;
    const uint32_t camera = g_cam_call;
    g_cam_call = 0;
    if (camera) cam_sides(camera, rdf(camera + 0xD4), rdf(camera + 0xD8));
}
/* after the activation replay 0053912E (esi = the camera): the stored values are already the effective ones */
void pu_cam_replay(CPU *c){
    const uint32_t camera = c->esi;
    if (!camera) return;
    const float deg = rdf(camera + 0xD4), asp = rdf(camera + 0xD8);
    if (deg > 0.0f && deg < 179.0f && asp > 0.0f && cam_find(camera)) cam_sides(camera, deg, asp);
}
/* the half angle to draw with for this view, 0 when the game's own stays */
static float cam_view_half(uint32_t view, CamRec **out){
    const uint32_t camera = MR32(view + 0x0C);
    CamRec *r = camera && !g_cam_capture ? cam_find(camera) : NULL;
    if (!r || !r->changed || MR32(camera + 0x220) != 0) return 0.0f;
    if (MR32(camera + 0xD4) != r->eff_deg || MR32(camera + 0xD8) != r->eff_aspect) return 0.0f;
    if (fbits(rdf(view + 0x130) * 2.0f) != fbits(r->authored_deg)) return 0.0f;
    float half = (float)(atan((double)r->tan_x) * 180.0 / CAM_PI);
    if (half > 89.0f) half = 89.0f;
    if (out) *out = r;
    return half;
}
static void cam_view_swap(uint32_t view, float half){
    g_cam_view = view; g_cam_view_keep = rdf(view + 0x130);
    wrf(view + 0x130, half);
}
static void cam_view_restore(void){
    if (g_cam_view) wrf(g_cam_view + 0x130, g_cam_view_keep);
    g_cam_view = 0;
}
/* before 0047FB6F: call 0047F550(rotation = V+B8, translation = V+DC, distance) */
void pu_cam_tree_before(CPU *c){
    g_tree_on = 0;
    const uint32_t rot = MR32(c->esp), trans = MR32(c->esp + 4);
    if (!rot || trans != rot - 0xB8 + 0xDC) return;
    const uint32_t view = rot - 0xB8;
    CamRec *r = NULL;
    const float half = cam_view_half(view, &r);
    if (!(half > 0.0f)) return;
    const double t0 = tan((double)r->authored_deg * CAM_PI / 360.0);
    double vert = t0 * fmax(0.75, 1.0 / (double)r->authored_aspect);
    if ((double)r->tan_y > vert) vert = r->tan_y;
    g_tree_on = 1; g_tree_half = half; g_tree_ratio = (float)(vert / (double)r->tan_x);
    cam_view_swap(view, half);
}
void pu_cam_tree_after(CPU *c){ (void)c; cam_view_restore(); g_tree_on = 0; }
/* before 0047F642: call 0047F210(half angle, distance, 0.75, out): the vertical ratio of the widened footprint */
void pu_cam_footprint(CPU *c){
    if (g_tree_on && MR32(c->esp) == fbits(g_tree_half)) wrf(c->esp + 8, g_tree_ratio);
}
/* before 0045B3BE / 0045BD7E: call 0045AAF0(layer) of the active view (renderer 00628130 -> +10) */
void pu_cam_sky_before(CPU *c){
    (void)c;
    const uint32_t rend = MR32(0x00628130u), view = rend ? MR32(rend + 0x10) : 0;
    if (!view) return;
    const float half = cam_view_half(view, NULL);
    if (half > 0.0f) cam_view_swap(view, half);
}
void pu_cam_sky_after(CPU *c){ (void)c; cam_view_restore(); }

/* PERFSTAT=1: the race loop (004152E0) at 00415C6E, after its simulation ticks: passes, ticks and the frames drawn per 5 s */
long r_frames(void);
static void race_wait_tick(void);
void guest_wait_begin(void); void guest_wait_end(void);
/* The race loop spins between the simulation's 60 ticks a second (3 million passes a second, the timer's tick 00606A9C comes
 * from the clock, read by the loop itself): a pass that simulated nothing lets the guest lock go for a short sleep, so the
 * timer and the other threads get the lock at once and the core is free. ?env=RACE_SPIN=1 keeps the game's own spin. */
static double g_idle_sleep_ms, g_idle_lock_ms; static int g_idle_sleeps;
static double g_sim_ms; static int g_sim_n;
static void race_idle(uint32_t tick){
    static int on = -1; static uint32_t last;
    if (on < 0){ const char *e = getenv("RACE_SPIN"); on = !(e && *e && *e != '0'); }
    const int idle = tick == last;
    last = tick;
    if (!on || !idle) return;
    struct timespec a0, a1, a2;
    clock_gettime(CLOCK_MONOTONIC, &a0);
    guest_wait_begin();
    race_wait_tick(); g_idle_sleeps++;
    clock_gettime(CLOCK_MONOTONIC, &a1);
    guest_wait_end();
    clock_gettime(CLOCK_MONOTONIC, &a2);
    g_idle_sleep_ms += (a1.tv_sec - a0.tv_sec) * 1e3 + (a1.tv_nsec - a0.tv_nsec) / 1e6;
    g_idle_lock_ms += (a2.tv_sec - a1.tv_sec) * 1e3 + (a2.tv_nsec - a1.tv_nsec) / 1e6;
}
void pu_loop_stat(CPU *c){
    (void)c;
    {
        int pu_interp_check(void);
        const int drawing = pu_interp_check();
        /* a pass that draws is paced by the display in r_pageflip */
        if (!drawing) race_idle(MR32(0x00606AA0u));
    }
    static int on = -1; static double t0; static long f0; static uint32_t tick0; static int passes, idle, maxticks; static uint32_t last_tick;
    if (on < 0){ const char *e = getenv("PERFSTAT"); on = e && *e && *e != '0'; }
    if (!on) return;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    const double t = ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
    const uint32_t tick = MR32(0x00606AA0u);
    passes++;
    if (tick == last_tick) idle++;
    else if ((int)(tick - last_tick) > maxticks && last_tick) maxticks = (int)(tick - last_tick);
    last_tick = tick;
    if (!t0){ t0 = t; f0 = r_frames(); tick0 = tick; passes = idle = maxticks = 0; return; }
    if (t - t0 >= 5000.0){
        fprintf(stderr, "[perf loop] per s: %.0f passes (%.0f without a tick), %.1f ticks, %.1f frames; most ticks in one pass %d; idle: %d sleeps, %.2f ms each, %.0f ms slept, %.0f ms getting the lock back, in 1 s\n",
                passes * 1000.0 / (t - t0), idle * 1000.0 / (t - t0), (tick - tick0) * 1000.0 / (t - t0), (r_frames() - f0) * 1000.0 / (t - t0), maxticks, g_idle_sleeps, g_idle_sleeps ? g_idle_sleep_ms / g_idle_sleeps : 0.0, g_idle_sleep_ms * 1000.0 / (t - t0), g_idle_lock_ms * 1000.0 / (t - t0));
        fprintf(stderr, "[perf sim] %d steps, %.2f ms each, %.0f ms a second\n", g_sim_n, g_sim_n ? g_sim_ms / g_sim_n : 0.0, g_sim_ms * 1000.0 / (t - t0));
        g_sim_ms = 0; g_sim_n = 0;
        g_idle_sleep_ms = g_idle_lock_ms = 0; g_idle_sleeps = 0;
        t0 = t; f0 = r_frames(); tick0 = tick; passes = idle = maxticks = 0;
    }
}

/* 00414FF0, one simulation step of the race loop: render interpolation records the state it left */
void pu_interp_step(void);
/* ---- the race's screen takes the window's shape mid-race (r_web.c r_reshape_wanted). Every view gave its camera a pixel rectangle
 * from its fractions of the screen through 00538DA0 when it was made; those calls are kept and made again with the new width. ---- */
int r_reshape_wanted(int *pw) __attribute__((weak));
void r_reshape(int nw) __attribute__((weak));
#define CAM_RECTS 16
static struct { uint32_t camera, a[6]; int screen_w, per_frame; } g_rects[CAM_RECTS];
/* 00538DA0, the camera's pixel rectangle: thiscall(w, h, x, y, ?, ?) ret 18h */
void sub_00538da0(CPU *restrict c){
    const int per_frame = MR32(c->esp) == 0x0046744Cu;      /* the view constructor (004672F0), which the race calls every frame */
    if (!g_cam_capture && c->ecx){
        int k = -1;
        for (int i = 0; i < CAM_RECTS && k < 0; i++) if (g_rects[i].camera == c->ecx) k = i;
        for (int i = 0; i < CAM_RECTS && k < 0; i++) if (!g_rects[i].camera) k = i;
        if (k >= 0){
            g_rects[k].camera = c->ecx; g_rects[k].screen_w = (int)MR32(0x00619784u); g_rects[k].per_frame = per_frame;
            for (int i = 0; i < 6; i++) g_rects[k].a[i] = MR32(c->esp + 4 + 4u * (uint32_t)i);
        }
    }
    /* a view placed from the HUD layout (the mirror, the map): its fractions are of the layout's screen, put back at that size by its side */
    const int sw = (int)MR32(0x00619784u);
    if (per_frame && g_hud_w0 > 0 && sw != g_hud_w0){
        float w, x;
        memcpy(&w, g_mem + c->esp + 4, 4); memcpy(&x, g_mem + c->esp + 12, 4);
        if (w < (float)sw - 1.0f){
            const float k = (float)g_hud_w0 / (float)sw;
            w *= k; x *= k;
            x = hud_anchor_x(x, x + w);
            memcpy(g_mem + c->esp + 4, &w, 4); memcpy(g_mem + c->esp + 12, &x, 4);
        }
    }
    sub_00538da0_orig(c);
}
/* 0044E324, 0044EC58 (pu_hud_mode): a new mode, the race's views come anew */
static void rects_forget(void){ memset(g_rects, 0, sizeof g_rects); }
/* 00538D80, the camera's destructor (thiscall): a restarted race frees its views, a later resize must not touch their cameras */
void sub_00538d80(CPU *restrict c){
    for (int i = 0; i < CAM_RECTS; i++) if (g_rects[i].camera == c->ecx) memset(&g_rects[i], 0, sizeof g_rects[i]);
    sub_00538d80_orig(c);
}
static void race_reshape(CPU *c){
    int nw;
    if (!r_reshape_wanted || !r_reshape_wanted(&nw)) return;
    const uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx;
    MR32(0x00619784u) = (uint32_t)nw;
    MR32(0x005DEAC8u) = (uint32_t)nw;     /* the 3D driver's copy (00467A10), which 00538DA0 clamps the rectangles to */
    r_reshape(nw);
    for (int i = 0; i < CAM_RECTS; i++){
        if (!g_rects[i].camera || g_rects[i].screen_w <= 0 || g_rects[i].per_frame) continue;
        uint32_t a[6];
        memcpy(a, g_rects[i].a, sizeof a);
        const float k = (float)nw / (float)g_rects[i].screen_w;
        float w, x;
        memcpy(&w, &a[0], 4); memcpy(&x, &a[2], 4);
        w *= k; x *= k;
        memcpy(&a[0], &w, 4); memcpy(&a[2], &x, 4);
        pu_guest_call(c, 0x00538DA0u, g_rects[i].camera, a, 6);
    }
    /* the HUD keeps the layout it was made with (its virtual size, the layout's size at 0060C900): the page puts each of its pieces
     * back at that size by the side it sits on (r_hud_draw) */
    c->eax = eax; c->ecx = ecx; c->edx = edx;
}
void sub_00414ff0(CPU *restrict c){
    struct timespec s0, s1; clock_gettime(CLOCK_MONOTONIC, &s0);
    sub_00414ff0_orig(c);
    clock_gettime(CLOCK_MONOTONIC, &s1);
    g_sim_ms += (s1.tv_sec - s0.tv_sec) * 1e3 + (s1.tv_nsec - s0.tv_nsec) / 1e6; g_sim_n++;
    pu_interp_step();
    race_reshape(c);
}
/* 00425950, 00425810: the race HUD's two passes in the frame (00468A97, 00468AAA) */
void r_hud_draw(int w0) __attribute__((weak));
void sub_00425950(CPU *restrict c){
    if (r_hud_draw) r_hud_draw(g_hud_w0);
    sub_00425950_orig(c);
    if (r_hud_draw) r_hud_draw(0);
}
void sub_00425810(CPU *restrict c){
    if (r_hud_draw) r_hud_draw(g_hud_w0);
    sub_00425810_orig(c);
    if (r_hud_draw) r_hud_draw(0);
}

/* ---- The game's timer on an exact schedule (PC wrapper enhanced qpc_timer.cpp, SilentPatch). The game re-arms a one-shot
 * timeSetEvent (00564EB0) that signals an event; its timer thread (00565270) runs one tick per wake-up. Two signals during
 * one wait for the guest lock make one tick: 60 instead of 64 simulation steps a second, unevenly. The thread proc here
 * counts the ticks due by the clock and runs them all; the one-shot chain is off. ---- */
#define TIMER_TICKS 0x006B7C7Cu
#define TIMER_TICKS2 0x006B7C40u
#define TIMER_PIT 0x006B7C44u
#define TIMER_CALLBACKS 0x006B7C20u
#define TIMER_PERIOD 0x006A5BF8u
#define TIMER_RUNNING 0x006A5C0Cu
#define TIMER_CATCH_UP 128
static volatile int32_t g_tick_seq;
static volatile uint32_t g_timer_gen;
static uint64_t mono_us(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u; }
void sub_00564eb0(CPU *restrict c){ POP32(c->eip); c->esp += 0x14; }
/* the guest lock is free while it waits; the last 2.5 ms on the clock, a sleep's wake-up is that coarse in a page's worker */
static void timer_wait_until(uint64_t due, uint32_t mine){
    guest_wait_begin();
    for (;;){
        const uint64_t now = mono_us();
        if (now >= due || g_timer_gen != mine) break;
        if (due - now > 2500u) usleep((useconds_t)(due - now - 2500u));
    }
    guest_wait_end();
}
void sub_00565270(CPU *restrict c){
    const uint32_t mine = __atomic_add_fetch(&g_timer_gen, 1, __ATOMIC_SEQ_CST);
    /* TIMER_init publishes "running" after it created this thread */
    for (const uint64_t t0 = mono_us(); !MR32(TIMER_RUNNING) && g_timer_gen == mine && mono_us() - t0 < 2000000u;){ guest_wait_begin(); usleep(1000); guest_wait_end(); }
    const uint32_t period = MR32(TIMER_PERIOD);
    const uint64_t hz = period ? (65536000u + period / 2u) / period : 100u;
    fprintf(stderr, "[timer] exact schedule, %u Hz\n", (unsigned)hz);
    uint64_t start = mono_us(), processed = 0, pit_reported = 0;
    const uint64_t pit_start = start;
    while (g_timer_gen == mine && MR32(TIMER_RUNNING)){
        timer_wait_until(start + (processed + 1) * 1000000u / hz, mine);
        if (g_timer_gen != mine || !MR32(TIMER_RUNNING)) break;
        const uint64_t now = mono_us(), due = (now - start) * hz / 1000000u;
        if (due <= processed) continue;
        uint64_t pending = due - processed;
        if (pending > TIMER_CATCH_UP){           /* a long hold of the lock: game time does not race ahead */
            pending = TIMER_CATCH_UP;
            start = now - (processed + pending) * 1000000u / hz;
        }
        for (uint64_t i = 0; i < pending; i++){
            MR32(TIMER_TICKS) = MR32(TIMER_TICKS) + 1;
            MR32(TIMER_TICKS2) = MR32(TIMER_TICKS2) + 1;
            const uint64_t pit = ((now - pit_start) * 1193u / 1000u) >> 16;
            if (pit != pit_reported){ MR32(TIMER_PIT) = MR32(TIMER_PIT) + (uint32_t)(pit - pit_reported); pit_reported = pit; }
            for (uint32_t slot = 0; slot < 8; slot++){
                const uint32_t fn = MR32(TIMER_CALLBACKS + slot * 4);
                if (fn) guest_call(c, fn, 0, NULL, 0);
            }
            guest_call(c, 0x0053C270u, 0, NULL, 0);     /* the frame event the one-shot callback pulsed */
        }
        processed += pending;
        __atomic_add_fetch(&g_tick_seq, 1, __ATOMIC_SEQ_CST);
#ifdef __wasm__
        __builtin_wasm_memory_atomic_notify((int32_t *)&g_tick_seq, 0xFFFFFFFFu);
#endif
    }
    if (g_timer_gen == mine) MR32(0x005DEB48u) = 0;
    c->eax = 0;
    POP32(c->eip);
}
/* a pass of the race loop without a simulation step sleeps until the next tick */
static void race_wait_tick(void){
#ifdef __wasm__
    __builtin_wasm_memory_atomic_wait32((int32_t *)&g_tick_seq, __atomic_load_n(&g_tick_seq, __ATOMIC_SEQ_CST), 4000000LL);
#else
    usleep(1000);
#endif
}

#ifdef __wasm__
/* ---- In the browser the page owns the controls (web/src/input.ts: keyboard, the Gamepad API, touch.ts on a touch screen) and
 * puts the pad straight into the race's input packet (pu_pad_input), so the game's own controller setup has nothing to set up
 * there: the Options screen loses its Controllers tab. A read of FEData/Layouts/optiontabs.lay gets "state=hidden" (same length)
 * in the section of the tab whose panel is ID_controllers_panel; the game knows that state (carsetup.lay uses it).
 * ?env=CONTROLS_TAB=1 keeps the tab. ---- */
static void patch_option_tabs(char *p, long len){
    static int keep = -1; if (keep < 0){ const char *e = getenv("CONTROLS_TAB"); keep = e && *e && *e != '0'; }
    if (keep) return;
    static const char panel[] = "panel=ID_controllers_panel", state[] = "state=normal", sect[] = "[section.";
    for (long i = 0; i + (long)sizeof panel - 1 <= len; i++){
        if (strncasecmp(p + i, panel, sizeof panel - 1)) continue;
        for (long k = i; k >= 0; k--){
            if (!strncmp(p + k, sect, sizeof sect - 1)) break;                  /* the tab's section has no plain state */
            if (k + (long)sizeof state - 1 <= len && !strncmp(p + k, state, sizeof state - 1)){
                memcpy(p + k, "state=hidden", sizeof state - 1);
                fprintf(stderr, "[controls] the Options screen's Controllers tab is hidden (the page has the controls)\n");
                return;
            }
        }
        return;
    }
}
/* ---- RES1: the browser picks the race's resolution (the window's size, web/src/adaptive-res.ts; the race mode is forced in
 * pu_hooks.c race_mode), so the graphics options lose their Resolution row: in FEData/Layouts/graphicsettings.lay the
 * row of the Graphics.Resolutions combo box (the box, its label and the row's two rectangles: the sections at its height)
 * gets "state=hidden", and the rows under it move up a row, so the list has no hole (the panel's last line stays empty). Same-length edits
 * of the file as it is read. The hidden box is not drawn, not clicked and not in the page's keyboard / pad focus
 * (web/src/focus-page.ts skips hidden controls). ?env=RES_OPTION=1 keeps the row. ---- */
typedef struct { long at, end, pos; int x, y; char acc[40]; } LaySection;
static int lay_sections(char *p, long len, LaySection *out, int max){
    int n = 0;
    for (long i = 0; i + 9 <= len && n < max; i++){
        if (strncmp(p + i, "[section.", 9) || (i > 0 && p[i - 1] != '\n')) continue;
        if (n) out[n - 1].end = i;
        LaySection *s = &out[n++];
        memset(s, 0, sizeof *s); s->at = i; s->end = len; s->pos = -1; s->y = -1;
    }
    for (int k = 0; k < n; k++){
        LaySection *s = &out[k];
        for (long i = s->at; i < s->end; i++){
            if (p[i] != '\n' || i + 1 >= s->end) continue;
            const char *l = p + i + 1;
            if (!strncmp(l, "pos=", 4)){ s->pos = i + 1 + 4; s->x = atoi(l + 4); const char *cm = memchr(l, ',', (size_t)(s->end - (i + 1))); if (cm) s->y = atoi(cm + 1); }
            else if (!strncmp(l, "accessor=", 9)){
                int m = 0; while (m < 39 && i + 1 + 9 + m < s->end && l[9 + m] != '\r' && l[9 + m] != '\n'){ s->acc[m] = l[9 + m]; m++; }
                s->acc[m] = 0;
            }
        }
    }
    return n;
}
static int lay_state(char *p, const LaySection *s, const char *to){
    for (long i = s->at; i + 12 <= s->end; i++) if (!strncmp(p + i, "state=normal", 12)){ memcpy(p + i + 6, to, 6); return 1; }
    return 0;
}
static int lay_move_y(char *p, const LaySection *s, int from, int to){
    if (s->pos < 0) return 0;
    char *cm = memchr(p + s->pos, ',', 8);
    if (!cm) return 0;
    char a[8], b[8];
    snprintf(a, sizeof a, "%d", from); snprintf(b, sizeof b, "%d", to);
    if (strlen(a) != strlen(b) || strncmp(cm + 1, a, strlen(a))) return 0;
    memcpy(cm + 1, b, strlen(b));
    return 1;
}
static void patch_graphics_settings(char *p, long len){
    static int keep = -1; if (keep < 0){ const char *e = getenv("RES_OPTION"); keep = e && *e && *e != '0'; }
    if (keep) return;
    LaySection sec[64];
    const int n = lay_sections(p, len, sec, 64);
    int res = -1, last = -1, next = -1;
    for (int k = 0; k < n; k++){
        if (!strcmp(sec[k].acc, "Graphics.Resolutions")) res = k;
        if (!strcmp(sec[k].acc, "Graphics.ViewDistance")) last = k;
    }
    if (res < 0 || last < 0 || sec[res].y < 0 || sec[last].y <= sec[res].y){ fprintf(stderr, "[options] graphicsettings.lay: rows not as expected, left alone\n"); return; }
    const int ry = sec[res].y, ly = sec[last].y;
    for (int k = 0; k < n; k++)       /* the next row's box: the row step */
        if (sec[k].acc[0] && sec[k].y > ry && (next < 0 || sec[k].y < next)) next = sec[k].y;
    const int step = next - ry;
    int hidden = 0, moved = 0;
    /* the rows' column (left of x 300): the combo box at y, its label at y+2, the two rectangles at y */
    for (int k = 0; k < n; k++){
        const int y = sec[k].y;
        if (y < 0 || sec[k].x >= 300) continue;
        if (y == ry || y == ry + 2) hidden += lay_state(p, &sec[k], "hidden");
        else if (y > ry + 2 && y <= ly + 2) moved += lay_move_y(p, &sec[k], y, y - step);
    }
    fprintf(stderr, "[options] graphics options: the Resolution row hidden (%d controls), the rows under it moved up %d (%d controls)\n", hidden, step, moved);
}
#endif
static int path_is(const char *path, size_t n, const char *name){     /* the file's name, any folder */
    const size_t k = strlen(name);
    return n >= k && !strcasecmp(path + n - k, name) && (n == k || path[n - k - 1] == '\\' || path[n - k - 1] == '/');
}
/* Called by ReadFile (fileio.c) after every read: same-length edits of a few menu layouts as they are read. */
void pu_patch_read(const char *path, uint32_t buf, long got){
#ifdef __wasm__
    if (got <= 0) return;
    const size_t n = strlen(path);
    char *p = (char *)g_mem + buf;
    if (path_is(path, n, "optiontabs.lay")) patch_option_tabs(p, got);
    else if (path_is(path, n, "graphicsettings.lay")) patch_graphics_settings(p, got);
#else
    (void)path; (void)buf; (void)got;
#endif
}

/* ---- R3DSpecularTextureLayer (vtable 005B39B4, apply 00445520): the car's sphere-map reflection pass. Its apply runs right
 * before the layer's drawtrimesh, so the web renderer is told the next draw over the car page is the reflection (masked off the
 * paint, which has its own). Other passes over the same vertices (the projected tree shadows and lamps) are drawn as they are. ---- */
void r_reflection_next(void) __attribute__((weak));
void sub_00445520(CPU *restrict c){
    sub_00445520_orig(c);
    if (r_reflection_next) r_reflection_next();
}

/* ---- G11 (PC wrapper car_reflection.cpp, [display] tunnel_lamps=0): R3DSkyMapTextureLayer (vtable 005B398C, apply 00445B10,
 * thiscall(streamA, streamB) ret 8) projects the tunnel lamps' glow onto the car from above; with the R1 cube reflection the car
 * already reflects the lamp panels, the projection lies on top of it as bright patches. The game's own zero-texture path
 * (00445B86..00445BB1) is replayed: streamB+4 = 0 makes the next texture layer skip the pass, the return value is what the
 * game computes for an unused layer. TUNNEL_LAMPS=1 keeps the original. ---- */
void sub_00445b10(CPU *restrict c){
    static int lamps = -1;
    if (lamps < 0){ const char *e = getenv("TUNNEL_LAMPS"); lamps = e && *e == '1'; }
    static int tlog = -1; static unsigned calls, lit;
    if (tlog < 0) tlog = getenv("TUNNELLOG") != NULL;
    const uint32_t layer = c->ecx, stream_b = MR32(c->esp + 8);
    if (tlog && !(++calls & 1023)) fprintf(stderr, "[tunnel] sky-map layer: %u calls, %u with a lamp texture (lamps %s)\n", calls, lit, lamps ? "on" : "off");
    if (lamps || !layer || !stream_b){ sub_00445b10_orig(c); if (stream_b && MR32(stream_b + 4)) lit++; return; }
    MR32(stream_b + 4) = 0;
    uint32_t r;
    if (MR32(layer + 0x14) == 0x20 || MR32(layer + 0x10) == 0x0E){
        const uint32_t args[2] = { 0, 0 };
        const uint32_t p = pu_guest_call(c, 0x0053F460u, layer, args, 2);
        r = p ? MR32(p + 8) : 0;
    } else r = MR32(layer + 8);
    c->eax = r;
    POP32(c->eip); c->esp += 8;
}
