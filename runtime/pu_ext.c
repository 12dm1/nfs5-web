/* nfs5-web: the converted High Stakes tracks (feature port of the PC wrapper's extended_content.cpp and hs_corridor.cpp).
 * The roster and the sound archives need no code here: the server's data layer gives them the stock names
 * (tools/make_mod_layer.py). Overrides of lifted functions: gen.sh OVERRIDE=... */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cpu.h"
#include "decls.h"
#include "ext/hs_walls.h"
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
