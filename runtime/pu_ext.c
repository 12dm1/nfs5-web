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
void sub_004e06d0(CPU *restrict c){
    if (g_nnames < 0) names_load();
    const uint32_t id = MR32(c->esp + 4);
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
    if (!g_hs) return;
    /* the CRP's path, at 00628964 */
    char win[300], path[1024];
    snprintf(win, sizeof win, "%s", (const char *)(g_mem + 0x00628964u));
    size_t n = strlen(win);
    if (n > 4 && !strcasecmp(win + n - 4, ".crp")) win[n - 4] = 0;
    strncat(win, ".hsw", sizeof win - strlen(win) - 1);
    const int segs = fileio_resolve_game(win, path, sizeof path) ? hs_walls_load(path) : -1;
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
/* 00498FF0, the per-car collision step (car+E2C), cdecl */
void sub_00498ff0(CPU *restrict c){
    const uint32_t car = MR32(c->esp + 4);
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
void pu_car_lod(CPU *c){
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
}
/* 0 frontend, 1 race menu (pause, results, options), 2 driving */
int pu_pad_mode(void){
    if (g_race_depth <= 0) return 0;
    if (MR32(0x00606AA4u) != 0 || MR32(0x00606A90u) != 0) return 1;
    const uint32_t pages = MR32(0x0060C9C4u);
    if (pages){
        const int32_t n = ((int32_t)MR32(pages + 4) - (int32_t)MR32(pages)) >> 3;
        if (n > 0 && n < 64) return 1;
    }
    return 2;
}
static uint32_t player_car(void){
    const int player = (int)MR32(0x0065741Cu);
    return player >= 0 && player < 16 ? MR32(0x005E8E2Cu + 4u * (uint32_t)player) : 0;
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
static uint32_t guest_vcall(CPU *c, uint32_t obj, uint32_t off){ return guest_call(c, MR32(MR32(obj) + off), obj, NULL, 0); }
static uint32_t guest_string(const char *text){
    const size_t n = strlen(text) + 1;
    const uint32_t a = gheap_alloc(1, (uint32_t)n, 1);
    memcpy(g_mem + a, text, n);
    return a;
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
void sub_004f4fa0(CPU *restrict c){      /* Generate, ecx = the accessor */
    const uint32_t self = c->ecx;
    MR32(self + 0x14) = OPP_MODE_COUNT;
    const int menu = (int)MR32(self + 0x18);
    int game = menu - OPP_MODES_OURS;
    if (menu >= 0 && menu < OPP_MODES_OURS){
        if (opp_run(c, self, menu)){ POP32(c->eip); return; }
        game = 4;                            /* its random car */
    }
    if (game >= 0 && game <= 4) MR32(self + 0x18) = (uint32_t)game;
    sub_004f4fa0_orig(c);
    MR32(self + 0x18) = (uint32_t)menu;
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
static float g_hud_fx = 1.0f, g_hud_fy = 1.0f;
static void hud_virtualize(int w, int h, int *vw, int *vh){
    float scale = (float)h / HUD_REF_HEIGHT;
    if (scale < 1.0f) scale = 1.0f;
    *vw = (int)lroundf((float)w / scale); *vh = (int)lroundf((float)h / scale);
}
#define DRAW_LIMIT1 0x006C1F08u
#define DRAW_LIMIT2 0x006C1F0Cu
#define DRAW_SCALE 2.0f
#define DRAW_MIN 20000.0f
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
void pu_hud_mode(void){
    const int w = (int)MR32(0x00619784u), h = (int)MR32(0x00619788u);
    int vw = w, vh = h;
    if (w > 0 && h > 0) hud_virtualize(w, h, &vw, &vh);
    MR32(HUD_VW) = (uint32_t)vw; MR32(HUD_VH) = (uint32_t)vh;
    if (!MR32(DRAW_LIMIT1)) draw_limits(0.0f);
    g_hud_fx = vw > 0 ? (float)w / (float)vw : 1.0f;
    g_hud_fy = vh > 0 ? (float)h / (float)vh : 1.0f;
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
    if (r->changed && logs > 0){ logs--; fprintf(stderr, "[widescreen] camera 0x%08x %.2f deg aspect %.4f -> %.2f deg aspect %.4f\n", camera, deg, asp, eff_deg, eff_asp); }
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
    CamRec *r = camera ? cam_find(camera) : NULL;
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
    race_idle(MR32(0x00606AA0u));
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
        g_idle_sleep_ms = g_idle_lock_ms = 0; g_idle_sleeps = 0;
        t0 = t; f0 = r_frames(); tick0 = tick; passes = idle = maxticks = 0;
    }
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
