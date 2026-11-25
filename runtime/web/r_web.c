/* nfs5-web WEB3: the renderer behind the THRASH driver (thrash_hle.c r_*) in the browser.
 *
 * The game thread records into a shared ring of segments; the page (web/src/gpu/renderer.ts) replays them with
 * WebGPU. This file holds the same behaviour as the native renderer's base path (renderer/dx11z: dx11_state.cpp,
 * dx11_texture.cpp, dx11_draw.cpp, thrash_dx11.cpp), without the PC mod's features (materials, cars, reflections).
 *
 * A segment: a word stream of commands + its vertices + its texture pixels. Everything of one segment's uploads
 * reaches the GPU before its draws, so updating a texture this segment already drew with closes the segment first.
 * The protocol is web/src/gpu/proto.ts. */
#include "../cpu.h"
#include "host_abi.h"
#include "../../renderer/dx11z/thrash_api.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdatomic.h>

void guest_wait_begin(void);
void guest_wait_end(void);

/* ---- protocol ---- */
enum { C_VIDEO = 1, C_TEX_CREATE, C_TEX_DESTROY, C_TEX_WRITE, C_TEX_GENMIPS, C_SCISSOR, C_PIPE, C_BIND, C_UNIFORM, C_DRAW, C_PRESENT, C_READRECT, C_TARGET, C_CUBE_MIPS, C_PAINT };
#define SEGS 3
#define CMD_WORDS (1u << 18)
#define VTX_BYTES (24u << 20)
#define UP_BYTES  (24u << 20)
#define VSIZE 48
#define UNI 48                 /* floats of a draw's uniform block */
typedef struct {
    volatile int32_t ready;         /* 0 free, 1 published (game -> page -> 0) */
    uint32_t cmd_words, vtx_bytes, up_bytes, has_present;
    uint32_t cmd, vtx, up;          /* addresses in linear memory */
} Seg;
typedef struct {
    uint32_t magic, seg_count;
    volatile int32_t published, consumed;   /* segment counters; notified on change */
    volatile int32_t presents_pub, presents_done;
    Seg seg[SEGS];
    volatile int32_t win_w, win_h;          /* the page: the mode a race started now would use */
} Ring;
static Ring g_ring;
static Seg *g_cur;
static uint32_t *g_cmd, g_ncmd;
static uint8_t *g_vtx, *g_up;
static uint32_t g_nvtx, g_nup;

typedef struct { float x, y, z, w, u, v; uint32_t color, specular; float rect[4]; } DV;
typedef struct { float x, y, z, rhw; uint32_t color, specular; float u, v; } TV;

/* PERFSTAT=1: every 5 s the game thread's frame: its own work, the waits for a free segment and for the page's present */
static double g_pf_wait_seg, g_pf_wait_present, g_pf_last, g_pf_t0, g_pf_frame_max, g_pf_work_max; static int g_pf_n, g_pf_slow, g_pf_on = -1;
static double pf_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void seg_acquire(void){
    static int next;
    Seg *s = &g_ring.seg[next];
    if (__atomic_load_n(&s->ready, __ATOMIC_ACQUIRE)){
        const double w0 = g_pf_on > 0 ? pf_now() : 0;
        guest_wait_begin();
        while (__atomic_load_n(&s->ready, __ATOMIC_ACQUIRE))
            __builtin_wasm_memory_atomic_wait32((int32_t *)&s->ready, 1, 100000000LL);
        guest_wait_end();
        if (g_pf_on > 0) g_pf_wait_seg += pf_now() - w0;
    }
    next = (next + 1) % SEGS;
    g_cur = s;
    g_cmd = (uint32_t *)(uintptr_t)s->cmd; g_vtx = (uint8_t *)(uintptr_t)s->vtx; g_up = (uint8_t *)(uintptr_t)s->up;
    g_ncmd = g_nvtx = g_nup = 0;
}

/* the state a segment starts with is unknown to the page: re-sent on demand */
static int g_seg_serial;
static void dirty_all(void);
static void close_draw(void);
static void seg_publish(int present){
    close_draw();
    Seg *s = g_cur;
    s->cmd_words = g_ncmd; s->vtx_bytes = g_nvtx; s->up_bytes = g_nup; s->has_present = (uint32_t)present;
    __atomic_store_n(&s->ready, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_ring.published, 1, __ATOMIC_ACQ_REL);
    if (present) __atomic_add_fetch(&g_ring.presents_pub, 1, __ATOMIC_ACQ_REL);
    __builtin_wasm_memory_atomic_notify((int32_t *)&g_ring.published, 0x7FFFFFFFu);
    g_seg_serial++;
    seg_acquire();
    dirty_all();
}
static inline void cmd_room(uint32_t n){ if (g_ncmd + n + 32 > CMD_WORDS) seg_publish(0); }

/* ---- states (dx11_state.cpp) ---- */
#define NST 0x10100
static int g_st[NST];
static int g_vstride = 32, g_istride = 4, g_race;
static int g_lw = 640, g_lh = 480;
static int g_zcmp = 3, g_zwrite, g_blend, g_cull;
static float g_alpharef, g_depthbias, g_fogdensity, g_fogscale = 1.0f;
static int g_fogon; static uint32_t g_fogcolor, g_clearcolor = 0xFF000000u;
static int g_filter = 1, g_clamp_s = 1, g_clamp_t = 1; static float g_lodbias;
static int g_clip[4], g_clipvalid;
/* textures */
#define MAX_TEX 8192
typedef struct { int used, w, h, fmt, levels, cutout, keyed, seg, paint; } Tex;     /* paint: 1 a car's paint page, 2 its glass page (pu_paint.c) */
static Tex g_tex[MAX_TEX];
static int g_bound;             /* slot + 1, 0 none */
#define LFB_SLOT (MAX_TEX - 1)
static uint32_t g_tex_mean[LFB_SLOT];   /* GLASSLOG debug */
static uint16_t g_tex_bright[LFB_SLOT]; /* per mille of opaque texels brighter than the cube's emissive threshold (0.85) */
#define SHADE_N 32
static uint8_t *g_tex_shade[LFB_SLOT];  /* dark textures: SHADE_N x SHADE_N cells of alpha, luma (the car shadow layer) */
static long g_frames;

static void update_fogscale(void){
    const float w63 = 262144.0f / 5.0f;
    const float d = 1.0f - expf(-g_fogdensity * w63);
    g_fogscale = d > 1e-6f ? 1.0f / d : 1.0f;
}
static int bound_keyed(void){ return g_bound && g_tex[g_bound - 1].keyed; }
/* HardCutoutWanted without the per-draw suppress, which comes from the vertices */
static int hard_wanted(void){
    if (!g_race || g_blend != 0 || g_zcmp == 7) return 0;
    if (!g_zwrite) return g_alpharef >= 10.0f / 255.0f && bound_keyed();
    if (g_alpharef >= 128.0f / 255.0f && g_alpharef <= 160.0f / 255.0f) return bound_keyed();
    return g_alpharef >= 10.0f / 255.0f && bound_keyed();
}

/* what the next draw needs; sent when it differs from what this segment last sent */
static uint32_t g_s_pipe = ~0u, g_s_sampler = ~0u, g_s_tex = ~0u, g_s_scissor[4]; static int g_s_scissor_ok, g_s_video;
static float g_s_uni[UNI]; static int g_s_uni_ok;
static uint32_t g_s_paint = ~0u;
static uint32_t g_draw_first, g_draw_count; static int g_draw_open;
static void dirty_all(void){ g_s_pipe = g_s_sampler = g_s_tex = g_s_paint = ~0u; g_s_scissor_ok = g_s_uni_ok = g_s_video = 0; }
static void close_draw(void){
    if (g_draw_open && g_draw_count){       /* in cmd_room's margin: seg_publish comes here too */
        g_cmd[g_ncmd++] = C_DRAW; g_cmd[g_ncmd++] = g_draw_first; g_cmd[g_ncmd++] = g_draw_count;
    }
    g_draw_open = 0; g_draw_count = 0;
}
/* the states of one draw, in full */
typedef struct { int blend, zcmp, zwrite, cull, topo, colorwrite; int tex; int filter, cs, ct, mips; float alpharef, bias, fogon; int hard; float depthbias; int paint, paint_tex; } DrawState;

/* a cube face is the target (pu_cube.c) */
static int g_cap, g_cap_clear, g_cap_opaque, g_cap_sun, g_cube_faces;
extern float g_paint_sun[3];
/* the paint shader's camera (PC car_reflection.cpp BuildConstants, materials.cpp): basis rows, projection, the pixels' centre and half size,
 * the game's car light in camera space */
static void paint_uniform(float *u){
    const uint32_t eagl = MR32(0x0069E0D4u), engine = MR32(0x00628130u);
    const uint32_t view = engine ? MR32(engine + 0x10) : 0, cam = view ? MR32(view + 0x0C) : 0;
    if (!eagl || !cam) return;
    float R[3], U[3], F[3], f;
    memcpy(R, g_mem + view + 0xE8, 12); memcpy(U, g_mem + view + 0xF4, 12); memcpy(F, g_mem + view + 0x100, 12);
    for (int k = 0; k < 3; k++){ u[16 + k] = R[k]; u[20 + k] = U[k]; u[24 + k] = F[k]; }
    u[19] = g_cube_faces >= 6 ? 1.0f : 0.0f;    /* the cube holds a whole capture */
    const int menu = MR32(0x0065B928u) != 0;
    u[23] = menu ? 0.0f : u[19];               /* its alpha is the sun mask; a room has no sun sprite */
    u[40] = menu ? 0.5f : 9.0f;                /* [car_paint] env_hdr_menu (the showroom's lamps are close and bright), env_hdr */
    memcpy(&f, g_mem + cam + 0x148, 4); u[28] = f;
    memcpy(&f, g_mem + cam + 0x15C, 4); u[29] = -f;
    memcpy(&f, g_mem + cam + 0x168, 4); u[30] = -f;
    memcpy(&f, g_mem + cam + 0x16C, 4); u[31] = -f;
    memcpy(&u[32], g_mem + eagl + 0x38, 4); memcpy(&u[33], g_mem + eagl + 0x3C, 4);
    memcpy(&u[34], g_mem + eagl + 0x2C, 4); memcpy(&u[35], g_mem + eagl + 0x30, 4);
    static const float k_menu_sun[3] = { 0.0f, 1.0f, 0.0f };   /* the showroom's light comes from above: [car_paint] menu_sun_dir */
    const float *sd = menu ? k_menu_sun : g_paint_sun;
    float sc[3] = { R[0] * sd[0] + R[1] * sd[1] + R[2] * sd[2], U[0] * sd[0] + U[1] * sd[1] + U[2] * sd[2], -(F[0] * sd[0] + F[1] * sd[1] + F[2] * sd[2]) };
    const float l = sqrtf(sc[0] * sc[0] + sc[1] * sc[1] + sc[2] * sc[2]);
    if (l > 1e-6f) for (int k = 0; k < 3; k++) u[36 + k] = sc[k] / l;
}
static const float *g_pu_frozen;   /* GLASS1: a deferred glass pass keeps the camera it was drawn with */
static void emit_state(const DrawState *d){
    /* a change of anything closes the running draw */
    uint32_t pipe = (uint32_t)d->blend | (uint32_t)d->zcmp << 3 | (uint32_t)d->zwrite << 6 | (uint32_t)d->cull << 7 |
                    (uint32_t)d->topo << 9 | (uint32_t)d->colorwrite << 11;
    pipe |= (uint32_t)(d->paint == 1) << 15 | (uint32_t)(d->paint == 2) << 16 | (uint32_t)(d->paint == 3) << 17;
    if (g_cap) pipe |= 1u << 12 | (uint32_t)(g_cap_clear && d->blend == 0) << 13 | (uint32_t)(g_cap_sun && d->blend == 1) << 14;
    uint32_t sam = (uint32_t)d->filter | (uint32_t)d->cs << 1 | (uint32_t)d->ct << 3 | (uint32_t)d->mips << 5;
    float u[UNI];
    memset(u, 0, sizeof u);
    u[0] = (float)g_lw; u[1] = (float)g_lh; u[2] = d->alpharef; u[3] = d->depthbias;
    u[4] = (float)((g_fogcolor >> 16) & 255) / 255.0f; u[5] = (float)((g_fogcolor >> 8) & 255) / 255.0f;
    u[6] = (float)(g_fogcolor & 255) / 255.0f; u[7] = d->fogon;
    u[8] = g_fogdensity; u[9] = g_fogscale; u[10] = d->hard ? 1.0f : 0.0f; u[11] = d->bias;
    u[12] = u[13] = u[14] = u[15] = 0.0f;
    /* PC [reflection] emissive: in the HDR cube the additive lights go above 1, and in the dark so do unlit lamp panels */
    if (g_cap){
        if (d->blend == 1){ u[12] = 5.0f; u[13] = 0.6f; u[14] = 2.0f; }
        else if (d->blend == 0 && g_cap_opaque){
            float dark = 1.0f;
            const uint32_t sky = MR32(0x006115CCu);
            if (sky){
                float amb[3]; memcpy(amb, g_mem + sky + 0x1C, 12);
                const float al = 0.299f * amb[0] + 0.587f * amb[1] + 0.114f * amb[2];
                if (al >= 0.0f && al < 16.0f){ dark = (0.5f - al) / 0.2f; dark = dark < 0.0f ? 0.0f : dark > 1.0f ? 1.0f : dark; }
            }
            /* Monaco at night: only small bright spots (lamp bulbs) go HDR there; a texture bright over a large part of it
             * (a lit shop window) keeps its colour, or its reflection outshines the lamps. CAP_OPAQUE = the gain in the dark (4),
             * CAP_AREA = the bright share (per mille) where the gain starts to fade, gone at twice that (60 on the Monaco
             * tracks; given, it counts on every track). */
            static float g_max = -1.0f, g_area = 0.0f;
            if (g_max < 0.0f){ const char *e = getenv("CAP_OPAQUE"); g_max = e && *e ? (float)atof(e) : 4.0f; e = getenv("CAP_AREA"); if (e && *e){ g_area = (float)atof(e); if (g_area < 1.0f) g_area = 1.0f; } }
            const float area = g_area > 0.0f ? g_area : !strncasecmp((const char *)(g_mem + 0x00628940u), "monaco", 6) ? 60.0f : 0.0f;
            float wa = 1.0f;
            if (area > 0.0f && d->tex > 0){ wa = (2.0f * area - (float)g_tex_bright[d->tex - 1]) / area; wa = wa < 0.0f ? 0.0f : wa > 1.0f ? 1.0f : wa; }
            u[12] = 1.0f + (g_max - 1.0f) * dark * wa; u[13] = 0.85f; u[14] = -4.0f;
            { static int lg = -1; if (lg < 0){ const char *e = getenv("CAPLOG"); lg = e && *e && *e != '0'; }
              static long last = -1000;
              if (lg && g_frames - last >= 300){ last = g_frames; fprintf(stderr, "[caplog] frame %ld opaque emissive gain %.2f (dark %.2f)\n", g_frames, u[12], dark); }
              static uint8_t seen[LFB_SLOT];
              if (lg && d->tex > 0 && !seen[d->tex - 1] && g_tex_bright[d->tex - 1] > 0){ seen[d->tex - 1] = 1; const uint32_t m = g_tex_mean[d->tex - 1];
                  fprintf(stderr, "[caplog] opaque tex %d %dx%d bright %u/1000 mean %06x\n", d->tex, g_tex[d->tex - 1].w, g_tex[d->tex - 1].h, g_tex_bright[d->tex - 1], m & 0xFFFFFF); } }
        }
        u[15] = g_cap_sun && d->blend == 1 ? 1.0f : 0.0f;
    }
    if (d->paint){ if (g_pu_frozen) memcpy(u + 16, g_pu_frozen + 16, (UNI - 16) * sizeof(float)); else paint_uniform(u); }
    int sc[4] = { 0, 0, g_lw, g_lh };
    if (g_clipvalid) memcpy(sc, g_clip, sizeof sc);
    int ch = pipe != g_s_pipe || sam != g_s_sampler || (uint32_t)d->tex != g_s_tex || !g_s_uni_ok || memcmp(u, g_s_uni, sizeof u) ||
             !g_s_scissor_ok || memcmp(sc, g_s_scissor, sizeof sc) || !g_s_video;
    if (!ch) return;
    close_draw();
    cmd_room(80);
    if (!g_s_video){ g_cmd[g_ncmd++] = C_VIDEO; g_cmd[g_ncmd++] = (uint32_t)g_lw; g_cmd[g_ncmd++] = (uint32_t)g_lh; g_s_video = 1; }
    if (pipe != g_s_pipe){ g_cmd[g_ncmd++] = C_PIPE; g_cmd[g_ncmd++] = pipe; g_s_pipe = pipe; }
    if (sam != g_s_sampler || (uint32_t)d->tex != g_s_tex || (uint32_t)d->paint_tex != g_s_paint){
        g_cmd[g_ncmd++] = C_BIND; g_cmd[g_ncmd++] = (uint32_t)d->tex; g_cmd[g_ncmd++] = sam; g_cmd[g_ncmd++] = (uint32_t)d->paint_tex;
        g_s_sampler = sam; g_s_tex = (uint32_t)d->tex; g_s_paint = (uint32_t)d->paint_tex;
        if (d->tex > 0 && d->tex <= MAX_TEX) g_tex[d->tex - 1].seg = g_seg_serial;
    }
    if (!g_s_uni_ok || memcmp(u, g_s_uni, sizeof u)){ g_cmd[g_ncmd++] = C_UNIFORM; memcpy(g_cmd + g_ncmd, u, sizeof u); g_ncmd += UNI; memcpy(g_s_uni, u, sizeof u); g_s_uni_ok = 1; }
    if (!g_s_scissor_ok || memcmp(sc, g_s_scissor, sizeof sc)){
        g_cmd[g_ncmd++] = C_SCISSOR; for (int i = 0; i < 4; i++) g_cmd[g_ncmd++] = (uint32_t)sc[i];
        memcpy(g_s_scissor, sc, sizeof sc); g_s_scissor_ok = 1;
    }
}

static void submit(const DV *v, int n, const DrawState *d){
    if (n <= 0) return;
    if (g_nvtx + (uint32_t)n * VSIZE > VTX_BYTES){ seg_publish(0); }
    emit_state(d);
    if ((uint32_t)n * VSIZE > VTX_BYTES) return;
    if (!g_draw_open){ g_draw_first = g_nvtx / VSIZE; g_draw_open = 1; g_draw_count = 0; }
    memcpy(g_vtx + g_nvtx, v, (size_t)n * VSIZE);
    g_nvtx += (uint32_t)n * VSIZE; g_draw_count += (uint32_t)n;
}

/* ---- GLASS1: the intact car glass's reflection pass ----
 * A glass page's draw goes as the game draws it (its tint and alpha test: the race's windows are mostly the clear intglass page,
 * alpha 0, which draws nothing and writes no depth), then once more with the glass shader (fs_glass), premultiplied, depth tested,
 * no depth write: the R1 cube's reflection by Fresnel over whatever ends up behind the window. That pass waits for the end of the
 * view (the next clear, clip, target change, HUD pass, 2D draw or the frame's end): the car draws its interior after some of its
 * windows, and drawn at once the pass would lie under it. */
#define GOV_VERTS 8192
#define GOV_DRAWS 128
typedef struct { DrawState d; int first, n, clip[4], clipvalid; float pu[UNI]; } GlassPass;
static DV g_gov_v[GOV_VERTS]; static int g_gov_nv, g_gov_n;
static GlassPass g_gov[GOV_DRAWS];
static void glass_flush(void){
    if (!g_gov_n) return;
    const int n = g_gov_n;
    g_gov_n = 0;
    int clip[4]; memcpy(clip, g_clip, sizeof clip);
    const int cv = g_clipvalid;
    { static long gd = -2; if (gd == -2){ const char *e = getenv("GDUMP"); gd = e ? atol(e) : -1; }
      if (gd == g_frames) for (int i = 0; i < n; i++){
          const GlassPass *q = &g_gov[i]; const DV *v = g_gov_v + q->first;
          fprintf(stderr, "[gdump] glass pass %d/%d: tex %d n %d zcmp %d cull %d hard %d clip %d %d,%d..%d,%d xy %.0f,%.0f z %.4f w %.4f cube %.0f\n", i, n, q->d.tex, q->n,
                  q->d.zcmp, q->d.cull, q->d.hard, q->clipvalid, q->clip[0], q->clip[1], q->clip[2], q->clip[3], v[0].x, v[0].y, v[0].z, v[0].w, q->pu[19]);
      } }
    for (int i = 0; i < n; i++){
        memcpy(g_clip, g_gov[i].clip, sizeof clip); g_clipvalid = g_gov[i].clipvalid;
        g_pu_frozen = g_gov[i].pu;
        submit(g_gov_v + g_gov[i].first, g_gov[i].n, &g_gov[i].d);
    }
    g_pu_frozen = NULL;
    memcpy(g_clip, clip, sizeof clip); g_clipvalid = cv;
    g_gov_nv = 0;
}
static void glass_defer(const DV *v, int n, const DrawState *d){
    if (n <= 0 || n > GOV_VERTS) return;
    if (g_gov_n >= GOV_DRAWS || g_gov_nv + n > GOV_VERTS) glass_flush();
    GlassPass *q = &g_gov[g_gov_n++];
    q->d = *d;
    q->d.zwrite = 0;
    if (q->d.zcmp == 1) q->d.zcmp = 3;                 /* over the game's own glass at the same depth */
    q->first = g_gov_nv; q->n = n;
    memcpy(q->clip, g_clip, sizeof q->clip); q->clipvalid = g_clipvalid;
    memset(q->pu, 0, sizeof q->pu);
    paint_uniform(q->pu);
    memcpy(g_gov_v + g_gov_nv, v, (size_t)n * sizeof(DV));
    g_gov_nv += n;
}

/* ---- draws (dx11_draw.cpp) ---- */
#define MAX_VERTS 65536
static DV g_scratch[MAX_VERTS];
static inline DV conv(const uint8_t *p){
    TV s; memcpy(&s, p, sizeof s);
    DV d; d.x = s.x; d.y = s.y; d.z = s.z; d.w = s.rhw; d.u = s.u; d.v = s.v; d.color = s.color; d.specular = s.specular;
    d.rect[0] = d.rect[1] = d.rect[2] = d.rect[3] = 0.0f;
    return d;
}
static inline int in_band(const DV *v, float x0, float x1, float y0, float y1){
    if (v->x != v->x || v->y != v->y || v->w != v->w) return 0;
    return !(v->x < x0 || v->x > x1 || v->y < y0 || v->y > y1);
}
static int guard_filter(DV *v, int n){
    const float S = 3.0f, w = (float)g_lw, h = (float)g_lh;
    const float x0 = -(S - 1) * w, x1 = S * w, y0 = -(S - 1) * h, y1 = S * h;
    int out = 0;
    for (int t = 0; t + 2 < n; t += 3){
        if (in_band(v + t, x0, x1, y0, y1) && in_band(v + t + 1, x0, x1, y0, y1) && in_band(v + t + 2, x0, x1, y0, y1) &&
            v[t].w >= 0.0f && v[t + 1].w >= 0.0f && v[t + 2].w >= 0.0f){
            if (out != t) memmove(v + out, v + t, 3 * sizeof(DV));
            out += 3;
        }
    }
    return out;
}
static void ui_rects(DV *verts, int n){
    if (g_race || !g_bound) return;
    for (int t = 0; t + 2 < n; t += 3){
        DV *v = verts + t;
        if ((v[0].w != 1.0f && v[0].w != 0.0f) || (v[1].w != 1.0f && v[1].w != 0.0f) || (v[2].w != 1.0f && v[2].w != 0.0f)) continue;
        float lo0 = v[0].u, hi0 = v[0].u, lo1 = v[0].v, hi1 = v[0].v;
        for (int i = 1; i < 3; i++){
            lo0 = v[i].u < lo0 ? v[i].u : lo0; hi0 = v[i].u > hi0 ? v[i].u : hi0;
            lo1 = v[i].v < lo1 ? v[i].v : lo1; hi1 = v[i].v > hi1 ? v[i].v : hi1;
        }
        if (hi0 <= lo0 || hi1 <= lo1 || lo0 < -1.0f || hi0 > 2.0f || lo1 < -1.0f || hi1 > 2.0f) continue;
        for (int i = 0; i < 3; i++){ v[i].rect[0] = lo0; v[i].rect[1] = lo1; v[i].rect[2] = hi0; v[i].rect[3] = hi1; }
    }
}
static int tan_substitute(int bound);
static float tan_voffset(int bound);
static void glass_log(int bound, const DV *o, int nv, int refl);
static int g_paint_on = -1, g_mask_slot;
static void game_state(DrawState *d, int topo){
    if (g_paint_on < 0){ const char *e = getenv("PAINT"); g_paint_on = !(e && *e == '0'); }
    d->blend = g_blend; d->zcmp = g_zcmp; d->zwrite = g_zwrite; d->cull = g_cull; d->topo = topo; d->colorwrite = 1;
    d->tex = tan_substitute(g_bound); d->filter = g_filter; d->cs = g_clamp_s; d->ct = g_clamp_t; d->mips = g_race ? 1 : 0;
    d->alpharef = g_alpharef; d->fogon = g_fogon ? 1.0f : 0.0f; d->depthbias = g_depthbias;
    if (d->filter == 0 && !g_race) d->filter = 1;            /* point_filter=2: honoured only in the race (G7) */
    d->hard = hard_wanted();
    d->bias = d->mips ? g_lodbias : 0.0f;
    if (!(d->bias > -16.0f && d->bias < 16.0f)) d->bias = 0.0f;
    d->paint = 0; d->paint_tex = 0;
    if ((g_race || MR32(0x0065B928u)) && !g_cap && g_paint_on && topo == 0){
        if (d->tex > 0 && g_tex[d->tex - 1].paint){ d->paint = g_tex[d->tex - 1].paint == 2 ? 3 : 1; d->paint_tex = d->tex; }   /* 3: a glass page */
        else if (g_mask_slot){ d->paint = 2; d->paint_tex = g_mask_slot; }
    }
}
/* The frontend's loading screen comes after the race's video mode (pu_hooks.c) and is laid out in 640x480 around the screen's
 * centre: scaled to the screen until the race's first 3D draw. */
/* the race HUD's passes (pu_ext.c sub_00425950): laid out on a screen g_hud_w0 wide, its pixels stay those of that screen after a
 * live resize; each draw moves by the side it sits on, so no piece stays where the old edges were */
static int g_hud_w0, g_hud_collect, g_hud_xform;
static float g_hud_lo, g_hud_hi, g_hud_dx;
/* one draw's primitives (triangles, lines, points) in the order they come: their x span, then the shift of the group each is in */
#define HUD_PRIMS 8192
static float g_hp_lo[HUD_PRIMS], g_hp_hi[HUD_PRIMS], g_hp_dx[HUD_PRIMS];
static int g_hp_n, g_hp_at;
void r_hud_draw(int w0){ glass_flush(); g_hud_w0 = w0; }
static float hud_side_dx(float lo, float hi){
    const float c = (lo + hi) * 0.5f / (float)g_hud_w0;
    const float side = c < 1.0f / 3.0f ? 0.0f : c > 2.0f / 3.0f ? 1.0f : 0.5f;
    return (float)(g_lw - g_hud_w0) * side;
}
/* pieces close side by side (a word's letters, a dial and its marks) move together, each group by the side it sits on: one draw can
 * hold pieces of several places (the racers' marks next to the names) */
static void hud_groups(void){
    static int order[HUD_PRIMS];
    const int n = g_hp_n;
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 1; i < n; i++){          /* insertion sort by the left edge: draws are short */
        const int k = order[i]; int j = i - 1;
        while (j >= 0 && g_hp_lo[order[j]] > g_hp_lo[k]){ order[j + 1] = order[j]; j--; }
        order[j + 1] = k;
    }
    for (int a = 0; a < n;){
        float lo = g_hp_lo[order[a]], hi = g_hp_hi[order[a]];
        int b = a + 1;
        while (b < n && g_hp_lo[order[b]] <= hi + 6.0f){ if (g_hp_hi[order[b]] > hi) hi = g_hp_hi[order[b]]; b++; }
        /* a piece across the screen (a fade, a flash) keeps its place */
        const float dx = hi - lo < 0.9f * (float)g_hud_w0 ? hud_side_dx(lo, hi) : 0.0f;
        for (int i = a; i < b; i++) g_hp_dx[order[i]] = dx;
        a = b;
    }
}
static void flush_verts(DV *v, int n, int topo){
    if (n <= 0) return;
    const int per = topo == 0 ? 3 : topo == 1 ? 2 : 1;
    if (g_hud_collect){
        for (int t = 0; t + per <= n; t += per){
            float lo = v[t].x, hi = v[t].x;
            for (int i = 1; i < per; i++){ lo = v[t + i].x < lo ? v[t + i].x : lo; hi = v[t + i].x > hi ? v[t + i].x : hi; }
            g_hud_lo = lo < g_hud_lo ? lo : g_hud_lo; g_hud_hi = hi > g_hud_hi ? hi : g_hud_hi;
            if (g_hp_n < HUD_PRIMS){ g_hp_lo[g_hp_n] = lo; g_hp_hi[g_hp_n] = hi; g_hp_n++; }
        }
        return;
    }
    if (g_hud_xform)
        for (int t = 0; t + per <= n; t += per, g_hp_at++){
            const float dx = g_hp_at < g_hp_n ? g_hp_dx[g_hp_at] : g_hud_dx;
            for (int i = 0; i < per; i++) v[t + i].x += dx;
        }
    { const float dv = tan_voffset(g_bound); if (dv != 0.0f) for (int i = 0; i < n; i++) v[i].v += dv; }
    if (topo == 0){
        n = guard_filter(v, n);
        if (n <= 0) return;
        ui_rects(v, n);
    }
    if (g_gov_n && v[0].w == 1.0f && (n < 2 || v[1].w == 1.0f) && (n < 3 || v[2].w == 1.0f)) glass_flush();   /* 2D: the view is over */
    DrawState d; game_state(&d, topo);
    { static int lg = -1; if (lg < 0){ const char *e = getenv("GOVLOG"); lg = e && *e && *e != '0'; }
      if (lg && g_frames % 600 == 300 && (g_gov_n || d.paint == 3)){
          float y0 = v[0].y, y1 = v[0].y; for (int i = 1; i < n; i++){ y0 = v[i].y < y0 ? v[i].y : y0; y1 = v[i].y > y1 ? v[i].y : y1; }
          fprintf(stderr, "[govlog] frame %ld %s tex %d paint %d blend %d zw %d zcmp %d cull %d n %d y %.0f..%.0f w %.3f pending %d va %02x\n", g_frames,
                  d.paint == 3 ? "GLASS" : "after", d.tex, d.paint, g_blend, g_zwrite, d.zcmp, d.cull, n, y0, y1, v[0].w, g_gov_n, v[0].color >> 24); } }
    /* HZ6: a keyed cutout in the sorted queue goes hard only while every vertex is opaque */
    if (d.hard && !g_zwrite)
        for (int i = 0; i < n; i++) if ((v[i].color >> 24) < 0xF8){ d.hard = 0; break; }
    /* the hard path clamps its sampler, unless the triangle's UVs tile (bar5) */
    if (d.hard){
        int tile = 0;
        if (topo == 0) for (int i = 0; i < n && !tile; i++) tile = v[i].u < -0.01f || v[i].u > 1.01f || v[i].v < -0.01f || v[i].v > 1.01f;
        if (!tile){ d.cs = d.ct = 0; }
        else for (int t = 0; t + 2 < n; t += 3){      /* the shader's inset stays inside the UV box */
            DV *q = v + t;
            float lo0 = q[0].u, hi0 = q[0].u, lo1 = q[0].v, hi1 = q[0].v;
            for (int i = 1; i < 3; i++){
                lo0 = q[i].u < lo0 ? q[i].u : lo0; hi0 = q[i].u > hi0 ? q[i].u : hi0;
                lo1 = q[i].v < lo1 ? q[i].v : lo1; hi1 = q[i].v > hi1 ? q[i].v : hi1;
            }
            for (int i = 0; i < 3; i++){ q[i].rect[0] = lo0; q[i].rect[1] = lo1; q[i].rect[2] = hi0; q[i].rect[3] = hi1; }
        }
    }
    if (d.paint == 3){                                  /* a glass page: the game's draw now, the reflection pass later */
        DrawState o = d;
        d.paint = 0; d.paint_tex = 0;
        submit(v, n, &d);
        glass_defer(v, n, &o);
        return;
    }
    submit(v, n, &d);
}
static inline uint32_t idx_at(const uint8_t *ib, int i){
    uint16_t x; memcpy(&x, ib + i * g_istride, 2); return x;
}
#define VI(vb, ib, i) ((vb) + idx_at((ib), (i)) * g_vstride)
#define GP(a) ((a) ? g_mem + (a) : (uint8_t *)0)

static void quad(const uint8_t *a, const uint8_t *b, const uint8_t *c, const uint8_t *e){
    DV v[6]; v[0] = conv(a); v[1] = conv(b); v[2] = conv(c); v[3] = v[0]; v[4] = v[2]; v[5] = conv(e);
    flush_verts(v, 6, 0);
}
static void line(const uint8_t *a, const uint8_t *b){ DV v[2]; v[0] = conv(a); v[1] = conv(b); flush_verts(v, 2, 1); }
static void point(const uint8_t *a){ DV v = conv(a); flush_verts(&v, 1, 2); }
static void sprite(const uint8_t *p0, const uint8_t *p1){
    TV a, b; memcpy(&a, p0, sizeof a); memcpy(&b, p1, sizeof b);
    TV tl = a, tr = a, bl = a, br = b;
    tr.x = b.x; tr.u = b.u; bl.y = b.y; bl.v = b.v;
    quad((uint8_t *)&tl, (uint8_t *)&tr, (uint8_t *)&br, (uint8_t *)&bl);
}
/* CP1 (PC dx11_draw.cpp ResolveClipCodes, FillMissingCarNormals): the game's clipper appends the corners of the polygons it cuts to
 * the EAGL vertex array after the model's own vertices (index >= E+4) with position, colour and UV, never the specular dword that
 * carries the car normal (runtime/pu_paint.c): those slots keep the codes of an earlier, larger batch, the cut polygons near the
 * camera shade wrong. A copy of a model corner takes that corner's code; a corner made on the cut, or any corner without a code,
 * takes the nearest one's on screen. */
static void car_clip_normals(uint32_t vba, const uint8_t *vb, const uint8_t *ib, DV *o, int nv){
    const uint32_t eagl = MR32(0x0069E0D4u);
    if (eagl && vba == eagl + 0x434u){
        const int orig = (int)MR32(eagl + 4);
        if (orig > 0 && orig <= 8192){
            int cut = 0;
            for (int i = 0; i < nv; i++){
                const int idx = (int)idx_at(ib, i);
                if (idx < orig) continue;
                float p[3]; memcpy(p, vb + idx * g_vstride, 12);
                o[i].specular = 0;
                for (int j = 0; j < orig; j++){
                    float q[3]; memcpy(q, vb + j * g_vstride, 12);
                    if (q[0] == p[0] && q[1] == p[1] && q[2] == p[2]){ memcpy(&o[i].specular, vb + j * g_vstride + 20, 4); break; }
                }
                cut++;
            }
            static int logs = 3;
            if (cut && logs > 0){ logs--; fprintf(stderr, "[paint] clipped car batch: %d of %d corners past the %d model vertices\n", cut, nv, orig); }
        }
    }
    int valid = 0, missing = 0;
    for (int i = 0; i < nv; i++){ if (o[i].specular & 0xFFu) valid++; else missing++; }
    if (!missing || !valid) return;
    for (int i = 0; i < nv; i++){
        if (o[i].specular & 0xFFu) continue;
        float best = 3.4e38f; int bi = -1;
        for (int j = 0; j < nv; j++){
            if (!(o[j].specular & 0xFFu) || j == i) continue;
            const float dx = o[j].x - o[i].x, dy = o[j].y - o[i].y, d2 = dx * dx + dy * dy;
            if (d2 < best){ best = d2; bi = j; }
        }
        if (bi >= 0) o[i].specular = o[bi].specular & 0x00FFFFFFu;
    }
}
static uint32_t g_pv_vb, g_pv_ib; static int g_pv_count, g_pv_slot;
static float g_pv_uv[MAX_VERTS * 2], g_pv_xy[MAX_VERTS * 2]; static int g_pv_nv;
/* the game's reflection pass over a car page draws the same vertices again. Same buffers and triangle count are not enough: the
 * game's projected layers (tree shadows, lamps) and car shadows draw the same vertices too, and taken for the reflection they
 * were masked off the paint, so they darkened only the glass and the lamp boxes. The reflection layer's apply (00445520, pu_ext.c)
 * arms the next draw. */
static int g_refl_armed;
void r_reflection_next(void){ g_refl_armed = 1; }
/* The game's projected tree shadow (a dark texture drawn over the car's paint-page meshes only) left the grilles, lamps and wheels
 * lit and laid its pattern on the glass as soot. Instead each car's shadow cover is measured from the layer (its texture's alpha
 * and darkness at the vertices) over all its paint-page vertices, and the next frame darkens the whole car by it: the paint-page
 * draws by their page, the car's other draws by screen box and depth. The light goes, the sky's reflection stays. */
typedef struct { int slot; float x0, y0, x1, y1, z0, z1; double n, dark; } CarShade;
#define CAR_SHADES 48
static CarShade g_cs_cur[CAR_SHADES], g_cs_prev[CAR_SHADES]; static int g_ncs_cur, g_ncs_prev;
static CarShade *cs_get(int slot){
    for (int i = 0; i < g_ncs_cur; i++) if (g_cs_cur[i].slot == slot) return &g_cs_cur[i];
    if (g_ncs_cur >= CAR_SHADES) return NULL;
    CarShade *r = &g_cs_cur[g_ncs_cur++]; memset(r, 0, sizeof *r); r->slot = slot;
    r->x0 = r->y0 = r->z0 = 1e30f; r->x1 = r->y1 = r->z1 = -1e30f;
    return r;
}
static float cs_factor(const CarShade *r){
    if (r->n <= 0.0) return 1.0f;
    const float f = 1.0f - (float)(r->dark / r->n);
    return f < 0.45f ? 0.45f : f > 1.0f ? 1.0f : f;
}
static void cs_scale(DV *o, int nv, float f){
    if (f >= 0.995f) return;
    for (int i = 0; i < nv; i++){
        const uint32_t c = o[i].color;
        o[i].color = (c & 0xFF000000u) | (uint32_t)(((c >> 16) & 255) * f) << 16 | (uint32_t)(((c >> 8) & 255) * f) << 8 | (uint32_t)((c & 255) * f);
    }
}
static unsigned g_cs_other;    /* CSLOG: non-paint draws darkened this second */
static void cs_flip(void){
    static int lg = -1; if (lg < 0) lg = getenv("CSLOG") != NULL;
    if (lg && g_frames % 60 == 0){
        float mn = 1.0f; int k = -1;
        for (int i = 0; i < g_ncs_cur; i++){ const float f = cs_factor(&g_cs_cur[i]); if (f < mn){ mn = f; k = i; } }
        fprintf(stderr, "[carshade] frame %ld: %d cars, darkest %.2f (page %d), %u other draws darkened\n", g_frames, g_ncs_cur, mn, k >= 0 ? g_cs_cur[k].slot : 0, g_cs_other);
        g_cs_other = 0;
    }
    memcpy(g_cs_prev, g_cs_cur, sizeof g_cs_cur); g_ncs_prev = g_ncs_cur; g_ncs_cur = 0;
}
static void cs_box(const DV *o, int nv, float *b){
    for (int i = 0; i < nv; i++){
        if (o[i].x < b[0]) b[0] = o[i].x;
        if (o[i].x > b[1]) b[1] = o[i].x;
        if (o[i].y < b[2]) b[2] = o[i].y;
        if (o[i].y > b[3]) b[3] = o[i].y;
        if (o[i].z < b[4]) b[4] = o[i].z;
        if (o[i].z > b[5]) b[5] = o[i].z;
    }
}
/* a car's own draw: its page's record, or a non-paint draw inside a shaded car's box and depth */
static void cs_apply(DV *o, int nv, int page){
    if (!g_ncs_prev || nv <= 0) return;
    if (page){
        for (int i = 0; i < g_ncs_prev; i++) if (g_cs_prev[i].slot == page){ cs_scale(o, nv, cs_factor(&g_cs_prev[i])); return; }
        return;
    }
    /* car batches only: the car light layer (pu_paint.c, 00446530) puts each vertex's normal into its specular dword */
    for (int i = 0; i < nv; i += 3) if (!(o[i].specular & 0x00FFFFFFu)) return;
    float b[6] = { 1e30f, -1e30f, 1e30f, -1e30f, 1e30f, -1e30f };
    cs_box(o, nv, b);
    for (int i = 0; i < g_ncs_prev; i++){
        const CarShade *r = &g_cs_prev[i];
        const float f = cs_factor(r);
        if (f >= 0.995f) continue;
        const float mx = (r->x1 - r->x0) * 0.15f + 4.0f, my = (r->y1 - r->y0) * 0.15f + 4.0f, mz = (r->z1 - r->z0) * 0.5f + 1e-4f;
        if (b[0] >= r->x0 - mx && b[1] <= r->x1 + mx && b[2] >= r->y0 - my && b[3] <= r->y1 + my && b[4] >= r->z0 - mz && b[5] <= r->z1 + mz){
            cs_scale(o, nv, f); g_cs_other++; return;
        }
    }
}
static void cs_page(const DV *o, int nv, int page){
    CarShade *r = cs_get(page);
    if (!r) return;
    float b[6] = { r->x0, r->x1, r->y0, r->y1, r->z0, r->z1 };
    cs_box(o, nv, b);
    r->x0 = b[0]; r->x1 = b[1]; r->y0 = b[2]; r->y1 = b[3]; r->z0 = b[4]; r->z1 = b[5];
    r->n += nv;
}
/* a texture coordinate's cell under the draw's addressing (0 clamp, 1 repeat, 2 mirror): a car beside the projector reads the
 * clamped edge, not the wrapped-round middle (the shadow came on metres before the car reached it) */
static int shade_cell(float t, int mode){
    if (mode == 0) t = t < 0.0f ? 0.0f : t > 0.9999f ? 0.9999f : t;
    else if (mode == 2){ t = fmodf(fabsf(t), 2.0f); if (t >= 1.0f) t = 1.9999f - t; }
    else t -= floorf(t);
    int k = (int)(t * SHADE_N);
    return k < 0 ? 0 : k >= SHADE_N ? SHADE_N - 1 : k;
}
/* the shadow layer over a page's vertices: how much light it takes, as the game's blend would (alpha over, colour tex x vertex) */
static int cs_shadow(const DV *o, int nv, int bound, int page){
    const uint8_t *g = bound ? g_tex_shade[bound - 1] : NULL;
    if (!g || g_blend != 0) return 0;
    CarShade *r = cs_get(page);
    if (!r) return 1;
    static int lg = -1; if (lg < 0) lg = getenv("CSLOG") != NULL;
    if (lg){ static int n; if (n < 4){ n++; fprintf(stderr, "[carshade] shadow layer tex %d clamp %d %d\n", bound, g_clamp_s, g_clamp_t); } }
    for (int i = 0; i < nv; i++){
        const int cx = shade_cell(o[i].u, g_clamp_s), cy = shade_cell(o[i].v, g_clamp_t);
        const uint32_t c = o[i].color;
        const float va = (float)(c >> 24) / 255.0f, vl = (float)(((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255)) / 765.0f;
        const float ta = g[(cy * SHADE_N + cx) * 2] / 255.0f, tl = g[(cy * SHADE_N + cx) * 2 + 1] / 255.0f;
        r->dark += ta * va * (1.0f - tl * vl);
    }
    return 1;
}
static int pv_same(const DV *o, int nv){
    if (nv != g_pv_nv) return 0;
    for (int i = 0; i < nv; i++)
        if (fabsf(o[i].x - g_pv_xy[i * 2]) > 0.01f || fabsf(o[i].y - g_pv_xy[i * 2 + 1]) > 0.01f) return 0;
    return 1;
}
static void draw_any(const char *n, const uint32_t *a);
void r_draw(const char *n, const uint32_t *a){
    if (g_hud_w0 > 0 && g_race && g_lw != g_hud_w0){
        g_hud_collect = 1; g_hud_lo = 1e30f; g_hud_hi = -1e30f; g_hp_n = 0;
        draw_any(n, a);
        g_hud_collect = 0;
        if (g_hud_hi > g_hud_lo){
            hud_groups();
            g_hud_dx = g_hud_hi - g_hud_lo < 0.9f * (float)g_hud_w0 ? hud_side_dx(g_hud_lo, g_hud_hi) : 0.0f;
            g_hud_xform = 1; g_hp_at = 0;
        }
        /* the HUD clips to the old screen, or to a piece of it (the map): the clip goes along with the piece, the screen's grows */
        const int keep[4] = { g_clip[0], g_clip[1], g_clip[2], g_clip[3] }, keepvalid = g_clipvalid;
        if (g_clipvalid){
            if (g_clip[0] <= 0 && g_clip[2] >= g_hud_w0 - 1){ g_clip[0] = 0; g_clip[2] = g_lw; }
            else if (g_hud_xform){ const int dx = (int)lroundf(hud_side_dx((float)g_clip[0], (float)g_clip[2])); g_clip[0] += dx; g_clip[2] += dx; }
            g_s_scissor_ok = 0;
        }
        draw_any(n, a);
        g_hud_xform = 0;
        if (keepvalid){ memcpy(g_clip, keep, sizeof keep); g_s_scissor_ok = 0; }
        return;
    }
    draw_any(n, a);
}
static void draw_any(const char *n, const uint32_t *a){
    const int count = (int)a[0];
    if (!strcmp(n, "drawtri@12")){ DV v[3]; v[0] = conv(GP(a[0])); v[1] = conv(GP(a[1])); v[2] = conv(GP(a[2])); flush_verts(v, 3, 0); return; }
    if (!strcmp(n, "drawquad@16")){ quad(GP(a[0]), GP(a[1]), GP(a[2]), GP(a[3])); return; }
    if (!strcmp(n, "drawline@8")){ line(GP(a[0]), GP(a[1])); return; }
    if (!strcmp(n, "drawpoint@4")){ point(GP(a[0])); return; }
    if (!strcmp(n, "drawsprite@8")){ sprite(GP(a[0]), GP(a[1])); return; }
    const uint8_t *vb = GP(a[1]);
    if (!vb || count <= 0) return;
    const int vs = g_vstride;
    DV *o = g_scratch; int k = 0;
    if (!strcmp(n, "drawtrifan@8")){
        for (int i = 1; i < count - 1 && k + 3 <= MAX_VERTS; i++){ o[k++] = conv(vb); o[k++] = conv(vb + i * vs); o[k++] = conv(vb + (i + 1) * vs); }
        flush_verts(o, k, 0); return; }
    if (!strcmp(n, "drawtristrip@8")){
        for (int i = 0; i < count - 2 && k + 3 <= MAX_VERTS; i++){
            const uint8_t *p0 = vb + i * vs, *p1 = vb + (i + 1) * vs, *p2 = vb + (i + 2) * vs;
            if (i & 1){ o[k++] = conv(p1); o[k++] = conv(p0); } else { o[k++] = conv(p0); o[k++] = conv(p1); }
            o[k++] = conv(p2);
        }
        flush_verts(o, k, 0); return; }
    if (!strcmp(n, "drawlinestrip@8")){ for (int i = 0; i < count - 1; i++) line(vb + i * vs, vb + (i + 1) * vs); return; }
    if (!strcmp(n, "drawpointstrip@8")){ for (int i = 0; i < count; i++) point(vb + i * vs); return; }
    const uint8_t *ib = GP(a[2]);
    if (!ib) return;
    { static long gd = -2; if (gd == -2){ const char *e = getenv("GDUMP"); gd = e ? atol(e) : -1; }
      if (gd == g_frames && !strncmp(n, "draw", 4)){
          const int bd = g_bound; const uint32_t m = bd ? g_tex_mean[bd - 1] : 0;
          const DV v0 = conv(VI(vb, GP(a[2]), 0));
          fprintf(stderr, "[gdump] %s tex %d %dx%d paint %d mean %06x a%02x blend %d aref %.2f n %d xy %.0f,%.0f uv %.3f,%.3f col %08x spec %08x\n", n, bd,
                  bd ? g_tex[bd - 1].w : 0, bd ? g_tex[bd - 1].h : 0, bd ? g_tex[bd - 1].paint : 0, m & 0xFFFFFF, m >> 24, g_blend, g_alpharef, count, v0.x, v0.y, v0.u, v0.v, v0.color, v0.specular);
      } }
    if (!strcmp(n, "drawtrimesh@12")){
        int nv = count * 3; if (nv > MAX_VERTS) nv = MAX_VERTS;
        for (int i = 0; i < nv; i++) o[i] = conv(VI(vb, ib, i));
        const int bound = tan_substitute(g_bound);
        const int paintable = (g_race || MR32(0x0065B928u)) && !g_cap;
        const int armed = g_refl_armed; g_refl_armed = 0;
        const int again = armed && paintable && bound && !g_tex[bound - 1].paint && g_pv_slot && g_pv_vb == a[1] && g_pv_ib == a[2] && g_pv_count == count && pv_same(o, nv);
        if (paintable && bound && (g_tex[bound - 1].paint || again)) car_clip_normals(a[1], vb, ib, o, nv);
        if (paintable && bound && g_tex[bound - 1].paint == 2) glass_log(bound, o, nv, 2);
        if (paintable && bound && g_tex[bound - 1].paint){
            g_pv_vb = a[1]; g_pv_ib = a[2]; g_pv_count = count; g_pv_slot = bound; g_pv_nv = nv;
            if (g_race){ cs_page(o, nv, bound); cs_apply(o, nv, bound); }
            for (int i = 0; i < nv; i++){ g_pv_uv[i * 2] = o[i].u; g_pv_uv[i * 2 + 1] = o[i].v; g_pv_xy[i * 2] = o[i].x; g_pv_xy[i * 2 + 1] = o[i].y; }
        }
        else if (again){
            for (int i = 0; i < nv; i++){ o[i].rect[0] = g_pv_uv[i * 2]; o[i].rect[1] = g_pv_uv[i * 2 + 1]; o[i].rect[2] = 0.0f; o[i].rect[3] = 7.0f; }
            g_mask_slot = g_pv_slot;
            glass_log(bound, o, nv, 1);
            if (g_race) cs_apply(o, nv, g_pv_slot);
            flush_verts(o, nv, 0);
            g_mask_slot = 0;
            return;
        }
        else {
            /* another layer over the same vertices keeps the record: the reflection may come after it */
            if (paintable && g_pv_slot && g_pv_vb == a[1] && g_pv_ib == a[2] && g_pv_count == count && pv_same(o, nv)){
                glass_log(bound, o, nv, 0);
                if (g_race && cs_shadow(o, nv, bound, g_pv_slot)) return;     /* the car's shadow layer: measured, not drawn */
            }
            else {
                g_pv_vb = 0;
                if (g_race && paintable) cs_apply(o, nv, 0);
            }
        }
        flush_verts(o, nv, 0); return; }
    if (!strcmp(n, "drawtrifan@12")){
        if (count < 3) return;
        DV c = conv(VI(vb, ib, 0));
        for (int i = 0; i < count - 2 && k + 3 <= MAX_VERTS; i++){ o[k++] = c; o[k++] = conv(VI(vb, ib, i + 1)); o[k++] = conv(VI(vb, ib, i + 2)); }
        flush_verts(o, k, 0); return; }
    if (!strcmp(n, "drawtristrip@12")){
        for (int i = 0; i < count - 2 && k + 3 <= MAX_VERTS; i++){
            if (i & 1){ o[k++] = conv(VI(vb, ib, i + 1)); o[k++] = conv(VI(vb, ib, i)); }
            else { o[k++] = conv(VI(vb, ib, i)); o[k++] = conv(VI(vb, ib, i + 1)); }
            o[k++] = conv(VI(vb, ib, i + 2));
        }
        flush_verts(o, k, 0); return; }
    if (!strcmp(n, "drawquadmesh@12")){ for (int i = 0; i < count; i++) quad(VI(vb, ib, i * 4), VI(vb, ib, i * 4 + 1), VI(vb, ib, i * 4 + 2), VI(vb, ib, i * 4 + 3)); return; }
    if (!strcmp(n, "drawlinemesh@12")){ for (int i = 0; i < count; i++) line(VI(vb, ib, i * 2), VI(vb, ib, i * 2 + 1)); return; }
    if (!strcmp(n, "drawlinestrip@12")){ for (int i = 0; i < count - 1; i++) line(VI(vb, ib, i), VI(vb, ib, i + 1)); return; }
    if (!strcmp(n, "drawpointmesh@12")){ for (int i = 0; i < count; i++) point(VI(vb, ib, i)); return; }
    if (!strcmp(n, "drawspritemesh@12")){ for (int i = 0; i < count; i++) sprite(VI(vb, ib, i * 2), VI(vb, ib, i * 2 + 1)); return; }
}

/* ---- textures (dx11_texture.cpp) ---- */
static int mip_count(int w, int h){ int n = 1; while (w > 1 || h > 1){ w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; n++; } return n; }
uint32_t r_talloc(int w, int h, int fmt, int flags, int mips){
    (void)flags; (void)mips;
    int slot = -1;
    for (int i = 0; i < LFB_SLOT; i++) if (!g_tex[i].used){ slot = i; break; }
    if (slot < 0 || w <= 0 || h <= 0) return 0;
    Tex *t = &g_tex[slot];
    memset(t, 0, sizeof *t);
    t->used = 1; t->w = w; t->h = h; t->fmt = fmt; t->levels = mip_count(w, h); t->seg = -1;
    cmd_room(6);
    uint32_t *c = g_cmd + g_ncmd; g_ncmd += 5;
    c[0] = C_TEX_CREATE; c[1] = (uint32_t)slot + 1; c[2] = (uint32_t)w; c[3] = (uint32_t)h; c[4] = (uint32_t)t->levels;
    return (uint32_t)slot + 1;
}
static void tan_free(int slot);
static void tan_reset(void);
static void tex_destroy(int slot){
    if (!g_tex[slot].used) return;
    g_tex[slot].used = 0;
    /* the page frees it after this segment's draws */
    cmd_room(3);
    g_cmd[g_ncmd++] = C_TEX_DESTROY; g_cmd[g_ncmd++] = (uint32_t)slot + 1;
}
void r_tfree(uint32_t t){
    if (!t || t >= LFB_SLOT + 1u) return;
    if ((int)t == g_bound) g_bound = 0;
    glass_flush();
    close_draw();
    tan_free((int)t - 1);
    tex_destroy((int)t - 1);
}
void pu_text_reset(int free_handles);
void r_treset(void){
    glass_flush();
    close_draw();
    pu_text_reset(0);
    for (int i = 0; i < LFB_SLOT; i++) tex_destroy(i);
    g_bound = 0;
    tan_reset();
}
void r_settexture(uint32_t t){ g_bound = (t && t < LFB_SLOT + 1u && g_tex[t - 1].used) ? (int)t : 0; }

static void to_rgba(const void *src, uint32_t *dst, int w, int h, int fmt){
    const int n = w * h;
    switch (fmt){
    case 3: case 11: case 0: case 10:
        for (int i = 0; i < n; i++){
            uint16_t c = ((const uint16_t *)src)[i];
            uint32_t a = (fmt == 11) ? 0xFF : ((c & 0x8000) ? 0xFF : 0);
            uint32_t r = ((c >> 10) & 0x1F) * 255 / 31, g = ((c >> 5) & 0x1F) * 255 / 31, b = (c & 0x1F) * 255 / 31;
            dst[i] = a << 24 | b << 16 | g << 8 | r;
        }
        break;
    case 4: case 1: case 9:
        for (int i = 0; i < n; i++){
            uint16_t c = ((const uint16_t *)src)[i];
            uint32_t r = ((c >> 11) & 0x1F) * 255 / 31, g = ((c >> 5) & 0x3F) * 255 / 63, b = (c & 0x1F) * 255 / 31;
            dst[i] = 0xFF000000u | b << 16 | g << 8 | r;
        }
        break;
    case 7: case 2:
        for (int i = 0; i < n; i++){
            uint16_t c = ((const uint16_t *)src)[i];
            uint32_t a = ((c >> 12) & 0xF) * 17, r = ((c >> 8) & 0xF) * 17, g = ((c >> 4) & 0xF) * 17, b = (c & 0xF) * 17;
            dst[i] = a << 24 | b << 16 | g << 8 | r;
        }
        break;
    case 8:
        for (int i = 0; i < n; i++){
            uint8_t c = ((const uint8_t *)src)[i];
            uint32_t a = ((c >> 4) & 0xF) * 17, l = (c & 0xF) * 17;
            dst[i] = a << 24 | l << 16 | l << 8 | l;
        }
        break;
    case 5:
        for (int i = 0; i < n; i++){
            const uint8_t *p = (const uint8_t *)src + i * 3;
            dst[i] = 0xFF000000u | (uint32_t)p[2] | (uint32_t)p[1] << 8 | (uint32_t)p[0] << 16;
        }
        break;
    default:
        for (int i = 0; i < n; i++){
            uint32_t c = ((const uint32_t *)src)[i];
            dst[i] = (c & 0xFF00FF00u) | ((c & 0x00FF0000u) >> 16) | ((c & 0xFFu) << 16);
        }
        break;
    }
}
static void classify(const uint32_t *px, int n, int *cutout, int *clear_, int *mid_, int *solid_){
    int clear = 0, solid = 0, mid = 0;
    for (int i = 0; i < n; i++){
        unsigned a = px[i] >> 24;
        if (a < 8) clear++; else if (a >= 128) solid++; else mid++;
    }
    *clear_ = clear; *mid_ = mid; *solid_ = solid;
    *cutout = !(!clear || !solid || mid * 20 > n || clear * 12 < n);
}
static int edge_opaque(const uint32_t *px, int w, int h, int e){
    int n = 0;
    if (e < 2){ const uint32_t *row = px + (e ? (h - 1) * w : 0); for (int x = 0; x < w; x++) n += (row[x] >> 24) >= 8; }
    else { const int x = e == 2 ? 0 : w - 1; for (int y = 0; y < h; y++) n += (px[y * w + x] >> 24) >= 8; }
    return n;
}
static void clear_border(uint32_t *px, int w, int h){
    if (w < 4 || h < 4) return;
    const int top = edge_opaque(px, w, h, 0), bot = edge_opaque(px, w, h, 1), l = edge_opaque(px, w, h, 2), r = edge_opaque(px, w, h, 3);
    if (top * 8 < w) for (int x = 0; x < w; x++) px[x] &= 0x00FFFFFFu;
    if (bot * 8 < w) for (int x = 0; x < w; x++) px[(h - 1) * w + x] &= 0x00FFFFFFu;
    if (l * 8 < h) for (int y = 0; y < h; y++) px[y * w] &= 0x00FFFFFFu;
    if (r * 8 < h) for (int y = 0; y < h; y++) px[y * w + w - 1] &= 0x00FFFFFFu;
}
/* flood the nearest opaque RGB into the A=0 texels, frontier by frontier */
static void dilate(uint32_t *px, int w, int h){
    const int n = w * h;
    int any_clear = 0, any_solid = 0;
    for (int i = 0; i < n && !(any_clear && any_solid); i++){ if (px[i] >> 24) any_solid = 1; else any_clear = 1; }
    if (!any_clear || !any_solid) return;
    uint8_t *has = malloc((size_t)n), *queued = calloc((size_t)n, 1);
    int *fr = malloc((size_t)n * sizeof(int)), *nx = malloc((size_t)n * sizeof(int));
    uint32_t *val = malloc((size_t)n * 4);
    for (int i = 0; i < n; i++) has[i] = (px[i] >> 24) != 0;
    static const int dx[8] = { -1, -1, -1, 0, 0, 1, 1, 1 }, dy[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
    int nf = 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++){
        const int i = y * w + x;
        if (has[i]) continue;
        for (int k = 0; k < 8; k++){
            const int xx = x + dx[k], yy = y + dy[k];
            if (xx >= 0 && yy >= 0 && xx < w && yy < h && has[yy * w + xx]){ fr[nf++] = i; queued[i] = 1; break; }
        }
    }
    const int limit = w > h ? w : h;
    for (int pass = 0; pass < limit && nf; pass++){
        for (int f = 0; f < nf; f++){
            const int i = fr[f], x = i % w, y = i / w;
            unsigned r = 0, g = 0, b = 0, c = 0;
            for (int k = 0; k < 8; k++){
                const int xx = x + dx[k], yy = y + dy[k];
                if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                const int j = yy * w + xx;
                if (!has[j]) continue;
                const uint32_t p = px[j];
                r += p & 255; g += (p >> 8) & 255; b += (p >> 16) & 255; c++;
            }
            val[f] = (px[i] & 0xFF000000u) | (b / c) << 16 | (g / c) << 8 | (r / c);
        }
        for (int f = 0; f < nf; f++){ px[fr[f]] = val[f]; has[fr[f]] = 1; }
        int nn = 0;
        for (int f = 0; f < nf; f++){
            const int i = fr[f], x = i % w, y = i / w;
            for (int k = 0; k < 8; k++){
                const int xx = x + dx[k], yy = y + dy[k];
                if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                const int j = yy * w + xx;
                if (has[j] || queued[j]) continue;
                queued[j] = 1; nx[nn++] = j;
            }
        }
        int *t = fr; fr = nx; nx = t; nf = nn;
    }
    free(has); free(queued); free(fr); free(nx); free(val);
}
static void downsample(const uint32_t *src, int sw, int sh, uint32_t *dst, int dw, int dh){
    for (int y = 0; y < dh; y++){
        const int y0 = (2 * y) * sw, y1 = (sh > 1) ? (2 * y + 1) * sw : y0;
        for (int x = 0; x < dw; x++){
            const int x0 = 2 * x, x1 = (sw > 1) ? x0 + 1 : x0;
            unsigned aR = 0, aG = 0, aB = 0, aA = 0, sR = 0, sG = 0, sB = 0;
            const int idx[4] = { y0 + x0, y0 + x1, y1 + x0, y1 + x1 };
            for (int i = 0; i < 4; i++){
                const uint32_t p = src[idx[i]];
                const unsigned r = p & 255, g = (p >> 8) & 255, b = (p >> 16) & 255, a = p >> 24;
                aR += r * a; aG += g * a; aB += b * a; sR += r; sG += g; sB += b; aA += a;
            }
            const unsigned r = aA ? aR / aA : sR / 4, g = aA ? aG / aA : sG / 4, b = aA ? aB / aA : sB / 4;
            dst[y * dw + x] = (aA / 4) << 24 | b << 16 | g << 8 | r;
        }
    }
}
/* one level's pixels into the segment; a texture this segment drew with waits for the next */
static void tex_write(int slot, int level, int x, int y, int w, int h, const uint32_t *px){
    const uint32_t bytes = (uint32_t)w * (uint32_t)h * 4u;
    if (bytes > UP_BYTES){ fprintf(stderr, "[gpu] upload of %ux%u is too big\n", w, h); return; }
    if (g_tex[slot].seg == g_seg_serial || g_nup + bytes > UP_BYTES) seg_publish(0);
    cmd_room(9);
    memcpy(g_up + g_nup, px, bytes);
    uint32_t *c = g_cmd + g_ncmd; g_ncmd += 8;
    c[0] = C_TEX_WRITE; c[1] = (uint32_t)slot + 1; c[2] = (uint32_t)level; c[3] = (uint32_t)x; c[4] = (uint32_t)y; c[5] = (uint32_t)w; c[6] = (uint32_t)h; c[7] = g_nup;
    g_nup += bytes;
}
static void upload_chain(int slot, uint32_t *mip0){
    int w = g_tex[slot].w, h = g_tex[slot].h;
    const int levels = g_tex[slot].levels;
    tex_write(slot, 0, 0, 0, w, h, mip0);
    if (levels <= 1) return;
    uint32_t *src = mip0; int free_src = 0;
    for (int l = 1; l < levels; l++){
        const int dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1;
        uint32_t *dst = malloc((size_t)dw * dh * 4);
        downsample(src, w, h, dst, dw, dh);
        tex_write(slot, l, 0, 0, dw, dh, dst);
        if (free_src) free(src);
        src = dst; free_src = 1; w = dw; h = dh;
    }
    if (free_src) free(src);
}
/* ---- HZ4: the NFS4 animated textures of a converted track, GameData/Track/<track>.tan (PC wrapper tex_anim.cpp).
 * "TAN1", count, then per entry: tag[4], kind (1 frames, 2 scroll), frames, delay (1/64 s), float scroll (v per 1/64 s),
 * w, h, then BGRA of frames 1..n-1. An upload of the base texture brings its other frames along as extra slots. ---- */
#define TAN_FRAMES 8
typedef struct { char tag[4]; int kind, frames, delay; float scroll; int w, h; uint8_t *px; } TanEntry;
static TanEntry *g_tan; static int g_ntan, g_tan_loaded;
static char g_tan_track[64], g_tex_name[12];
static short g_tan_of[MAX_TEX], g_tan_slot[MAX_TEX][TAN_FRAMES];    /* entry + 1; frame slot + 1 */
int fileio_resolve_game(const char *win, char *out, size_t outsz);
void r_tex_name(const char *name){ snprintf(g_tex_name, sizeof g_tex_name, "%s", name); }
static uint32_t tan_ticks(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 64 + t.tv_nsec / 15625000); }
static void tan_load(void){
    const char *track = (const char *)(g_mem + 0x00657444u);
    if (g_tan_loaded && !strncmp(track, g_tan_track, sizeof g_tan_track - 1)) return;
    for (int i = 0; i < g_ntan; i++) free(g_tan[i].px);
    free(g_tan); g_tan = NULL; g_ntan = 0;
    snprintf(g_tan_track, sizeof g_tan_track, "%s", track);
    g_tan_loaded = 1;
    char win[128], path[1024];
    snprintf(win, sizeof win, "GameData\\Track\\%s.tan", g_tan_track);
    FILE *f = fileio_resolve_game(win, path, sizeof path) ? fopen(path, "rb") : NULL;
    if (!f) return;
    char magic[4]; uint32_t count = 0;
    if (fread(magic, 1, 4, f) == 4 && !memcmp(magic, "TAN1", 4) && fread(&count, 4, 1, f) == 1 && count < 1024){
        g_tan = calloc(count ? count : 1, sizeof *g_tan);
        for (uint32_t i = 0; i < count; i++){
            TanEntry *e = &g_tan[g_ntan];
            uint8_t kind, frames; uint16_t delay, w, h;
            if (fread(e->tag, 1, 4, f) != 4 || fread(&kind, 1, 1, f) != 1 || fread(&frames, 1, 1, f) != 1 || fread(&delay, 2, 1, f) != 1 ||
                fread(&e->scroll, 4, 1, f) != 1 || fread(&w, 2, 1, f) != 1 || fread(&h, 2, 1, f) != 1 ||
                frames < 1 || frames > TAN_FRAMES || !w || !h || w > 1024 || h > 1024) break;
            e->kind = kind; e->frames = frames; e->delay = delay ? delay : 1; e->w = w; e->h = h;
            const size_t bytes = (size_t)(frames - 1) * w * h * 4;
            e->px = malloc(bytes ? bytes : 1);
            if (bytes && fread(e->px, 1, bytes, f) != bytes){ free(e->px); e->px = NULL; break; }
            g_ntan++;
        }
    }
    fclose(f);
    fprintf(stderr, "[hs] %s: %d animated textures\n", win, g_ntan);
}
static void tan_free(int slot){
    for (int k = 0; k < TAN_FRAMES; k++){
        if (g_tan_slot[slot][k]) tex_destroy(g_tan_slot[slot][k] - 1);
        g_tan_slot[slot][k] = 0;
    }
    g_tan_of[slot] = 0;
}
static void tan_upload(int slot){
    if (g_tan_of[slot]) tan_free(slot);
    if (!g_tex_name[0] || g_tex_name[0] == '?') return;
    tan_load();
    const int w = g_tex[slot].w, h = g_tex[slot].h;
    for (int i = 0; i < g_ntan; i++){
        const TanEntry *e = &g_tan[i];
        if (memcmp(e->tag, g_tex_name, 4)) continue;
        if (w > e->w || h > e->h || e->w % w || e->h % h) return;
        g_tan_of[slot] = (short)(i + 1);
        const int fx = e->w / w, fy = e->h / h;
        for (int k = 1; k < e->frames; k++){
            const uint8_t *src = e->px + (size_t)(k - 1) * e->w * e->h * 4;
            const uint32_t t = r_talloc(w, h, g_tex[slot].fmt, 0, 0);
            if (!t) return;
            uint32_t *px = malloc((size_t)w * h * 4);
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++){       /* box filter down to the uploaded size; BGRA -> RGBA */
                uint32_t acc[4] = { 0, 0, 0, 0 };
                for (int j = 0; j < fy; j++) for (int m = 0; m < fx; m++){
                    const uint8_t *p = src + ((size_t)(y * fy + j) * e->w + x * fx + m) * 4;
                    for (int c = 0; c < 4; c++) acc[c] += p[c];
                }
                const uint32_t d = (uint32_t)(fx * fy);
                px[y * w + x] = (acc[2] / d) | (acc[1] / d) << 8 | (acc[0] / d) << 16 | (acc[3] / d) << 24;
            }
            g_tex[t - 1].cutout = g_tex[slot].cutout; g_tex[t - 1].keyed = g_tex[slot].keyed;
            upload_chain((int)t - 1, px);
            free(px);
            g_tan_slot[slot][k] = (short)t;
        }
        return;
    }
}
static void tan_reset(void){ memset(g_tan_of, 0, sizeof g_tan_of); memset(g_tan_slot, 0, sizeof g_tan_slot); g_tan_loaded = 0; }
static int tan_substitute(int bound){
    if (!bound || !g_tan_of[bound - 1]) return bound;
    const TanEntry *e = &g_tan[g_tan_of[bound - 1] - 1];
    if (e->kind != 1 || e->frames < 2) return bound;
    const int k = (int)((tan_ticks() / (uint32_t)e->delay) % (uint32_t)e->frames);
    const int t = k ? g_tan_slot[bound - 1][k] : 0;
    return t ? t : bound;
}
static float tan_voffset(int bound){
    if (!bound || !g_tan_of[bound - 1]) return 0.0f;
    const TanEntry *e = &g_tan[g_tan_of[bound - 1] - 1];
    return e->kind == 2 ? (float)(tan_ticks() & 0x3FF) * e->scroll : 0.0f;     /* whole repeats at the wrap */
}
int paint_map(uint32_t data, int w, int h, int fmt, uint8_t *rg);
static long g_frames;
/* GLASSLOG=1 (debug): the car glass pages' draws and the game's reflection pass over the car pages, each new kind once: texture,
 * its mean colour, states, vertex colour, screen box */
static void glass_log(int bound, const DV *o, int nv, int refl){
    static int on = -1; static long last;
    if (on < 0){ const char *e = getenv("GLASSLOG"); on = e && *e && *e != '0'; }
    if (!on || !g_race) return;
    /* each new kind of pass once: texture, blend, alpha reference */
    static uint32_t seen[256]; static int nseen;
    const uint32_t key = (uint32_t)bound << 8 | (uint32_t)g_blend << 4 | (uint32_t)refl << 1 | (g_alpharef > 0.0f) | (refl == 2 ? (uint32_t)(g_frames / 600) << 20 : 0u);
    for (int i = 0; i < nseen; i++) if (seen[i] == key) return;
    if (nseen >= 256) return;
    seen[nseen++] = key; (void)last;
    double r = 0, g = 0, b = 0, a = 0;
    for (int i = 0; i < nv; i++){ const uint32_t cl = o[i].color; a += cl >> 24; r += (cl >> 16) & 255; g += (cl >> 8) & 255; b += cl & 255; }
    if (nv){ r /= nv; g /= nv; b /= nv; a /= nv; }
    const uint32_t m = bound ? g_tex_mean[bound - 1] : 0;
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (int i = 0; i < nv; i++){ x0 = fminf(x0, o[i].x); x1 = fmaxf(x1, o[i].x); y0 = fminf(y0, o[i].y); y1 = fmaxf(y1, o[i].y); }
    fprintf(stderr, "[glass] %s frame %ld tick %u: tex %d %dx%d keyed %d mean %02x%02x%02x a%02x | blend %d zw %d aref %.2f hard %d fog %d cube %d | verts %d argb %.0f %.0f %.0f %.0f | box %.0f..%.0f x %.0f..%.0f\n",
            refl == 2 ? "glass page" : refl ? "reflection" : "other", g_frames, MR32(0x00606AC4u), bound, bound ? g_tex[bound - 1].w : 0, bound ? g_tex[bound - 1].h : 0, bound ? g_tex[bound - 1].keyed : 0,
            (m >> 16) & 255, (m >> 8) & 255, m & 255, m >> 24, g_blend, g_zwrite, g_alpharef, hard_wanted(), g_fogon, g_cube_faces, nv, a, r, g, b, x0, x1, y0, y1);
}
void r_tupdate(uint32_t t, uint32_t data, int palette){
    (void)palette;
    if (!t || t > LFB_SLOT || !data || !g_tex[t - 1].used) return;
    const int slot = (int)t - 1, w = g_tex[slot].w, h = g_tex[slot].h, n = w * h;
    uint32_t *rgba = malloc((size_t)n * 4);
    to_rgba(g_mem + data, rgba, w, h, g_tex[slot].fmt);
    dilate(rgba, w, h);
    int cutout, clear, mid, solid;
    classify(rgba, n, &cutout, &clear, &mid, &solid);
    /* HZ6 keyed: car pages' rule needs the texture's name, which the mod's material system knows: not here yet */
    const int keyed = cutout || (solid && clear * 100 >= n && mid * 100 <= n);
    g_tex[slot].cutout = cutout; g_tex[slot].keyed = keyed;
    if (keyed) clear_border(rgba, w, h);
    { uint64_t sr = 0, sg = 0, sb = 0, sa = 0;
      for (int i = 0; i < n; i++){ const uint32_t px = rgba[i]; sr += px & 255; sg += (px >> 8) & 255; sb += (px >> 16) & 255; sa += px >> 24; }
      { uint32_t nb = 0;
        for (int i = 0; i < n; i++){ const uint32_t px = rgba[i], r = px & 255, g = (px >> 8) & 255, b = (px >> 16) & 255;
            const uint32_t m = r > g ? (r > b ? r : b) : (g > b ? g : b); if ((px >> 24) >= 128 && m > 217) nb++; }
        g_tex_bright[slot] = n ? (uint16_t)((uint64_t)nb * 1000 / (uint64_t)n) : 0; }
      if (n) g_tex_mean[slot] = (uint32_t)(sa / n) << 24 | (uint32_t)(sr / n) << 16 | (uint32_t)(sg / n) << 8 | (uint32_t)(sb / n);
      free(g_tex_shade[slot]); g_tex_shade[slot] = NULL;
      if (n && (sr + sg + sb) / 3 / (uint64_t)n < 0x40 && w >= SHADE_N && h >= SHADE_N && (g_tex_shade[slot] = calloc(SHADE_N * SHADE_N * 2, 1))){
          for (int cy = 0; cy < SHADE_N; cy++) for (int cx = 0; cx < SHADE_N; cx++){
              uint32_t ca = 0, cl = 0, cn = 0;
              for (int y = cy * h / SHADE_N; y < (cy + 1) * h / SHADE_N; y++) for (int x = cx * w / SHADE_N; x < (cx + 1) * w / SHADE_N; x++){
                  const uint32_t px = rgba[y * w + x]; ca += px >> 24; cl += ((px & 255) + ((px >> 8) & 255) + ((px >> 16) & 255)) / 3; cn++; }
              g_tex_shade[slot][(cy * SHADE_N + cx) * 2] = (uint8_t)(ca / cn); g_tex_shade[slot][(cy * SHADE_N + cx) * 2 + 1] = (uint8_t)(cl / cn);
          }
      } }
    upload_chain(slot, rgba);
    free(rgba);
    tan_upload(slot);
    /* runtime/pu_paint.c: a car page's paint map or a glass page's glass map, RG8 */
    const uint32_t bytes = (uint32_t)n * 2u;
    uint8_t *rg = malloc(bytes);
    const int had = g_tex[slot].paint;
    g_tex[slot].paint = rg && bytes <= UP_BYTES ? paint_map(data, w, h, g_tex[slot].fmt, rg) : 0;     /* 1 paint, 2 glass */
    { static int gl = -1; if (gl < 0){ const char *e = getenv("GLASSLOG"); gl = e && *e && *e != '0'; }
      if (gl && w <= 128 && w == h && (g_tex_mean[slot] >> 24) < 0xC0)
          fprintf(stderr, "[glass] slot %d tex %08x %dx%d fmt %d mean %06x a%02x: paint %d\n", slot + 1, data, w, h, g_tex[slot].fmt,
                  g_tex_mean[slot] & 0xFFFFFF, g_tex_mean[slot] >> 24, g_tex[slot].paint); }
    if (g_tex[slot].paint || had){
        if (g_tex[slot].seg == g_seg_serial || g_nup + bytes > UP_BYTES) seg_publish(0);
        cmd_room(6);
        if (g_tex[slot].paint) memcpy(g_up + g_nup, rg, bytes);
        uint32_t *c = g_cmd + g_ncmd; g_ncmd += 5;
        c[0] = C_PAINT; c[1] = (uint32_t)slot + 1; c[2] = g_tex[slot].paint ? (uint32_t)w : 0u; c[3] = (uint32_t)h; c[4] = g_nup;
        if (g_tex[slot].paint) g_nup += bytes;
    }
    free(rg);
}
/* runtime/pu_text.c: a generated A8R8G8B8 image, its clear texels already coloured */
void r_tupload_argb(uint32_t t, const uint32_t *argb){
    if (!t || t > LFB_SLOT || !g_tex[t - 1].used) return;
    const int slot = (int)t - 1, w = g_tex[slot].w, h = g_tex[slot].h;
    uint32_t *rgba = malloc((size_t)w * h * 4);
    if (!rgba) return;
    to_rgba((const uint8_t *)argb, rgba, w, h, 6);
    upload_chain(slot, rgba);
    free(rgba);
}
/* the screen's pixels per pixel of the game's mode: the menus are 640x480 drawn at the window's size */
float r_display_factor(void){
    const int wh = __atomic_load_n(&g_ring.win_h, __ATOMIC_ACQUIRE);
    if (wh <= 0 || g_lh <= 0) return 1.0f;
    const float f = (float)wh / (float)g_lh;
    return f < 1.0f ? 1.0f : f;
}
void r_tupdaterect(uint32_t t, int mip, int x, int y, int rw, int rh, int pitch, uint32_t data, int fmt){
    if (!t || t > LFB_SLOT || !data || !g_tex[t - 1].used) return;
    const int slot = (int)t - 1;
    int tw = g_tex[slot].w, th = g_tex[slot].h, sf = fmt >= 0 ? fmt : g_tex[slot].fmt;
    if (mip < 0 || mip >= g_tex[slot].levels) return;
    for (int m = 0; m < mip; m++){ tw = tw > 1 ? tw / 2 : 1; th = th > 1 ? th / 2 : 1; }
    if (x + rw > tw) rw = tw - x;
    if (y + rh > th) rh = th - y;
    if (rw <= 0 || rh <= 0) return;
    uint32_t *rgba = malloc((size_t)rw * rh * 4);
    for (int r = 0; r < rh; r++) to_rgba(g_mem + data + (uint32_t)(r * pitch), rgba + r * rw, rw, 1, sf);
    tex_write(slot, mip, x, y, rw, rh, rgba);
    free(rgba);
    if (mip == 0 && g_tex[slot].levels > 1){ cmd_room(3); g_cmd[g_ncmd++] = C_TEX_GENMIPS; g_cmd[g_ncmd++] = (uint32_t)slot + 1; }
}

/* ---- driver calls (thrash_dx11.cpp) ---- */
void r_setstate(int id, int value){
    if (id < 0 || id >= NST) return;
    if ((id == THRASH_STATE_ZBUFFER_COMPARE && (value < 0 || value > 7)) || (id == THRASH_STATE_ZBUFFER_MODE && (value < 0 || value > 2))) return;
    g_st[id] = value;
    switch (id){
    case THRASH_STATE_CLEAR_COLOR: g_clearcolor = (uint32_t)value; break;
    case THRASH_STATE_ZBUFFER_MODE:
        g_zcmp = value ? g_st[THRASH_STATE_ZBUFFER_COMPARE] : 7;
        g_zwrite = value != 0;
        break;
    case THRASH_STATE_ZBUFFER_COMPARE: g_zcmp = value; break;
    case THRASH_STATE_ZBUFFER_WRITE: g_zwrite = value != 0; break;
    case THRASH_STATE_ALPHA_TEST_REF: g_alpharef = (float)(value & 0xFF) / 255.0f; break;
    case THRASH_STATE_BLEND_MODE: if (value >= 0 && value < 4) g_blend = value; break;
    case THRASH_STATE_CULL_MODE: if (value >= 0 && value <= 2) g_cull = value; break;
    case THRASH_STATE_DEPTH_BIAS:
        if (value < -1000000 || value > 1000000) value = 0;
        g_depthbias = (float)value / 4096.0f;
        break;
    case THRASH_STATE_TEXTURE_FILTER: if (value == 0 || value == 1) g_filter = value; break;
    case THRASH_STATE_TEXTURE_LOD_BIAS: memcpy(&g_lodbias, &value, 4); break;
    case THRASH_STATE_TEXTURE_CLAMP_S: if (value >= 0 && value <= 2) g_clamp_s = value; break;
    case THRASH_STATE_TEXTURE_CLAMP_T: if (value >= 0 && value <= 2) g_clamp_t = value; break;
    case THRASH_STATE_VERTEX_STRIDE: if (value >= 24 && value <= 128) g_vstride = value; break;
    case THRASH_STATE_INDEX_FORMAT:
        switch (value){ case 1: g_istride = 1; break; case 2: g_istride = 2; break; case 3: g_istride = 4; break; case 4: g_istride = 8; break; default: g_istride = 2; }
        break;
    case THRASH_STATE_FOG_MODE_GLIDE: if (value == 0 || value == 1) g_fogon = value != 0; break;
    case THRASH_STATE_FOG_DENSITY: {
        float d = 0.0f;
        if (value != 0 && (value & 0xFFFFF000) == 0) d = 1.0f / (float)value; else memcpy(&d, &value, 4);
        if (!(d >= 0.0f && d < 64.0f)) d = 0.0f;
        g_fogdensity = d; update_fogscale();
        break; }
    case THRASH_STATE_FOG_COLOR_GLIDE: g_fogcolor = (uint32_t)value; break;
    default: break;
    }
}
int r_getstate(int id){ return id >= 0 && id < NST ? g_st[id] : 0; }

static long g_frames;
static int g_curwin;
int r_init(int w, int h){
    if (w <= 0) w = 640;
    if (h <= 0) h = 480;
    g_lw = w; g_lh = h;
    if (!g_cur){
        g_ring.magic = 0x4E465335u; g_ring.seg_count = SEGS;
        for (int i = 0; i < SEGS; i++){
            g_ring.seg[i].cmd = (uint32_t)(uintptr_t)malloc(CMD_WORDS * 4);
            g_ring.seg[i].vtx = (uint32_t)(uintptr_t)malloc(VTX_BYTES);
            g_ring.seg[i].up = (uint32_t)(uintptr_t)malloc(UP_BYTES);
            if (!g_ring.seg[i].cmd || !g_ring.seg[i].vtx || !g_ring.seg[i].up){ fprintf(stderr, "[gpu] no memory for the frames\n"); return 0; }
        }
        seg_acquire();
        nfsu2_register(HOST_KIND_GPU, &g_ring, sizeof g_ring);
    }
    memset(g_st, 0, sizeof g_st);
    g_st[THRASH_STATE_CLEAR_COLOR] = (int)0xFF000000u;
    g_st[THRASH_STATE_ZBUFFER_COMPARE] = 3;
    g_st[0x1E] = 32; g_st[0x46] = 3; g_st[0x43] = 0x3F800000; g_st[0x2E] = 0x3F800000; g_st[0x17] = 0x3F800000; g_st[0x3E] = 1;
    g_st[THRASH_STATE_SCREEN_WIDTH] = w; g_st[THRASH_STATE_SCREEN_HEIGHT] = h;
    g_vstride = 32; g_istride = 4; g_clipvalid = 0;
    dirty_all();
    fprintf(stderr, "[gpu] renderer ready, %dx%d\n", w, h);
    return 1;
}
void r_restore(void){ close_draw(); seg_publish(0); }
void r_setvideomode(int w, int h, int bpp){
    glass_flush();
    pu_text_reset(1);
    g_race = bpp == 32 && w > 640;         /* the frontend is 640x480x32 too (patches), the race never (pu_hooks.c) */
    g_st[THRASH_STATE_SCREEN_WIDTH] = w; g_st[THRASH_STATE_SCREEN_HEIGHT] = h;
    if (w > 0 && h > 0 && (w != g_lw || h != g_lh)){ close_draw(); g_lw = w; g_lh = h; g_cube_faces = 0; g_s_video = 0; g_clipvalid = 0; }   /* the old clip is the old mode's: the loading screen comes before the next THRASH_clip */
}
void r_window(int idx){ if (idx >= 0 && idx <= 7) g_curwin = idx; }
void r_clip(int x1, int y1, int x2, int y2){
    if (g_gov_n && (!g_clipvalid || g_clip[0] != x1 || g_clip[1] != y1 || g_clip[2] != x2 || g_clip[3] != y2)) glass_flush();
    g_clip[0] = x1; g_clip[1] = y1; g_clip[2] = x2; g_clip[3] = y2; g_clipvalid = 1; }
void r_flushwindow(void){ }
/* a quad at the far plane through the clip, with its own state: the depth-only window (3) keeps the colour */
void r_clearwindow(void){
    glass_flush();
    DrawState d; memset(&d, 0, sizeof d);
    d.blend = 4; d.zcmp = 7; d.zwrite = 1; d.colorwrite = g_curwin != 3; d.filter = 1; d.cs = d.ct = 0;
    const float W = (float)g_lw, H = (float)g_lh;
    const uint32_t c = g_clearcolor;
    DV v[6]; memset(v, 0, sizeof v);
    const float xs[6] = { 0, W, W, 0, W, 0 }, ys[6] = { 0, 0, H, 0, H, H };
    for (int i = 0; i < 6; i++){ v[i].x = xs[i]; v[i].y = ys[i]; v[i].z = 1.0f; v[i].w = 1.0f; v[i].color = c; }
    submit(v, 6, &d);
}
/* the software frame (loading screens): RGB565, one opaque full-screen quad */
void r_unlockwindow(uint32_t pixels, int w, int h, int pitch){
    if (!pixels || w <= 0 || h <= 0) return;
    Tex *t = &g_tex[LFB_SLOT];
    if (!t->used || t->w != w || t->h != h){
        close_draw();
        if (t->used){ cmd_room(3); g_cmd[g_ncmd++] = C_TEX_DESTROY; g_cmd[g_ncmd++] = LFB_SLOT + 1; }
        t->used = 1; t->w = w; t->h = h; t->levels = 1; t->seg = -1;
        cmd_room(6);
        uint32_t *c = g_cmd + g_ncmd; g_ncmd += 5;
        c[0] = C_TEX_CREATE; c[1] = LFB_SLOT + 1; c[2] = (uint32_t)w; c[3] = (uint32_t)h; c[4] = 1;
    }
    uint32_t *rgba = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) to_rgba(g_mem + pixels + (uint32_t)(y * pitch), rgba + y * w, w, 1, 4);
    tex_write(LFB_SLOT, 0, 0, 0, w, h, rgba);
    free(rgba);
    DrawState d; memset(&d, 0, sizeof d);
    d.blend = 4; d.zcmp = 7; d.colorwrite = 1; d.tex = LFB_SLOT + 1; d.filter = 1;
    const float W = (float)g_lw, H = (float)g_lh;
    DV v[6]; memset(v, 0, sizeof v);
    const float xs[6] = { 0, W, W, 0, W, 0 }, ys[6] = { 0, 0, H, 0, H, H }, us[6] = { 0, 1, 1, 0, 1, 0 }, vs[6] = { 0, 0, 1, 0, 1, 1 };
    /* a race mode's loading screen: the game lays its 640x480 picture out around the centre of the big frame; show that part */
    /* a wide race mode: the part of the frame with the window's shape, the 4:3 picture in its middle */
    const float aspect = (float)g_lw / (float)g_lh, half_w = 240.0f * (aspect > 4.0f / 3.0f ? aspect : 4.0f / 3.0f);
    float cu = w > 640 ? half_w / (float)w : 0.5f;
    if (cu > 0.5f) cu = 0.5f;
    const float cv = h > 480 ? 240.0f / (float)h : 0.5f;
    for (int i = 0; i < 6; i++){ v[i].x = xs[i]; v[i].y = ys[i]; v[i].w = 1.0f; v[i].u = 0.5f + (us[i] - 0.5f) * 2.0f * cu; v[i].v = 0.5f + (vs[i] - 0.5f) * 2.0f * cv; v[i].color = 0xFFFFFFFFu; }
    submit(v, 6, &d);
}
/* the screen's pixels are in the GPU, a frame away: the game's only use is the lens flares' sun test (G7);
 * WEB3: reads as black, which leaves the flare test without a change to see */
/* The lens flare's sun test (FUN_00460290 takes 4x4 pixels under the sun once as its reference, FUN_00460460 reads them again
 * every 200 ms and counts the equal ones). The picture lives on the GPU, on the page's side. A read stalls the game for up to
 * three queued frames, too much five times a second, so only the reference (the first read, or the first after a pause)
 * waits; the later ones ask for the pixels and return the answer to the previous question, 200 ms old. */
#define READ_MAX 64
static uint16_t g_read_px[READ_MAX * READ_MAX], g_read_last[READ_MAX * READ_MAX];
static volatile int32_t g_read_done;
static int32_t g_read_seq;
static int g_read_w, g_read_h;
static double g_read_time;
static double now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
int r_readrect(int x, int y, int w, int h, uint32_t buf){
    if (!buf || w <= 0 || h <= 0) return 0;
    if (w > READ_MAX || h > READ_MAX){ memset(g_mem + buf, 0, (size_t)w * h * 2); return 1; }
    const double now = now_s();
    const int wait = w != g_read_w || h != g_read_h || now - g_read_time > 1.0;
    int32_t done = __atomic_load_n(&g_read_done, __ATOMIC_ACQUIRE);
    if (done == g_read_seq && !wait) memcpy(g_read_last, g_read_px, (size_t)w * h * 2);
    if (done == g_read_seq || wait){
        g_read_seq++;
        close_draw();
        cmd_room(10);
        g_cmd[g_ncmd++] = C_READRECT; g_cmd[g_ncmd++] = (uint32_t)g_read_seq;
        g_cmd[g_ncmd++] = (uint32_t)x; g_cmd[g_ncmd++] = (uint32_t)y; g_cmd[g_ncmd++] = (uint32_t)w; g_cmd[g_ncmd++] = (uint32_t)h;
        g_cmd[g_ncmd++] = (uint32_t)(uintptr_t)g_read_px; g_cmd[g_ncmd++] = (uint32_t)(uintptr_t)&g_read_done;
        seg_publish(0);
    }
    if (wait){
        const double t0 = now;
        guest_wait_begin();
        while ((done = __atomic_load_n(&g_read_done, __ATOMIC_ACQUIRE)) - g_read_seq < 0 && now_s() - t0 < 1.0)
            __builtin_wasm_memory_atomic_wait32((int32_t *)&g_read_done, done, 100000000LL);
        guest_wait_end();
        if (done == g_read_seq) memcpy(g_read_last, g_read_px, (size_t)w * h * 2);
        else memset(g_read_last, 0, (size_t)w * h * 2);
        g_read_w = w; g_read_h = h;
        if (getenv("NFS5_READLOG")) fprintf(stderr, "[read] reference (%d,%d %dx%d) %04x %04x, %.1f ms\n", x, y, w, h, g_read_last[0], g_read_last[w + 1], (now_s() - t0) * 1e3);
    }
    else if (getenv("NFS5_READLOG")){
        static int n; if (n++ < 40) fprintf(stderr, "[read] (%d,%d) %04x %04x\n", x, y, g_read_last[0], g_read_last[w + 1]);
    }
    g_read_time = now;
    memcpy(g_mem + buf, g_read_last, (size_t)w * h * 2);
    return 1;
}
/* a window of another shape during a race: the game's screen takes the window's shape at the race's height (the page draws at the
 * window's pixels in any case). The width is asked for once it has held for 16 simulation steps; pu_ext.c moves the game's views over. */
int r_reshape_wanted(int *pw){
    static int want, stable;
    if (!g_race) return 0;
    const int ww = __atomic_load_n(&g_ring.win_w, __ATOMIC_ACQUIRE), wh = __atomic_load_n(&g_ring.win_h, __ATOMIC_ACQUIRE);
    if (ww <= 0 || wh <= 0) return 0;
    const int nw = ((int)lroundf((float)g_lh * (float)ww / (float)wh) + 4) & ~7;
    if (abs(nw - g_lw) < 8){ stable = 0; return 0; }
    if (nw != want){ want = nw; stable = 0; return 0; }
    if (++stable < 16) return 0;     /* about a quarter of a second: not every size a dragged edge passes */
    stable = 0;
    *pw = nw;
    return 1;
}
void thrash_screen_width(int w);
void r_reshape(int nw){
    glass_flush();
    fprintf(stderr, "[reshape] race screen %dx%d -> %dx%d\n", g_lw, g_lh, nw, g_lh);
    close_draw();
    g_st[THRASH_STATE_SCREEN_WIDTH] = nw;
    thrash_screen_width(nw);
    g_lw = nw; g_s_video = 0; g_clipvalid = 0;
}
void r_pageflip(void){
    glass_flush();
    g_frames++;
    cs_flip();
    close_draw();
    cmd_room(2);
    g_cmd[g_ncmd++] = C_PRESENT;
    seg_publish(1);
    /* one frame queued at most: the page's display rate paces the game */
    int pub = __atomic_load_n(&g_ring.presents_pub, __ATOMIC_ACQUIRE);
    int done = __atomic_load_n(&g_ring.presents_done, __ATOMIC_ACQUIRE);
    if (g_pf_on < 0){ const char *e = getenv("PERFSTAT"); g_pf_on = e && *e && *e != '0'; }
    const double w0 = g_pf_on > 0 ? pf_now() : 0;
    if (pub - done > 1){
        guest_wait_begin();
        while (pub - (done = __atomic_load_n(&g_ring.presents_done, __ATOMIC_ACQUIRE)) > 1)
            __builtin_wasm_memory_atomic_wait32((int32_t *)&g_ring.presents_done, done, 100000000LL);
        guest_wait_end();
    }
    if (g_pf_on > 0){
        const double t = pf_now(), wp = t - w0;
        g_pf_wait_present += wp;
        if (g_pf_last > 0){
            const double frame = t - g_pf_last;
            static double seg_before;
            const double work = frame - wp - (g_pf_wait_seg - seg_before);
            seg_before = g_pf_wait_seg;
            if (frame > g_pf_frame_max) g_pf_frame_max = frame;
            if (work > g_pf_work_max) g_pf_work_max = work;
            if (frame > 25.0) g_pf_slow++;
            g_pf_n++;
        } else g_pf_t0 = t;
        g_pf_last = t;
        if (t - g_pf_t0 >= 5000.0 && g_pf_n){
            const double span = t - g_pf_t0;
            fprintf(stderr, "[perf game] %.1f fps, per frame: work %.2f ms (max %.1f), wait segment %.2f ms, wait present %.2f ms; longest frame %.1f ms, frames over 25 ms %d\n",
                    g_pf_n * 1000.0 / span, (span - g_pf_wait_seg - g_pf_wait_present) / g_pf_n, g_pf_work_max, g_pf_wait_seg / g_pf_n, g_pf_wait_present / g_pf_n, g_pf_frame_max, g_pf_slow);
            g_pf_t0 = t; g_pf_n = 0; g_pf_slow = 0; g_pf_wait_seg = g_pf_wait_present = 0; g_pf_frame_max = g_pf_work_max = 0;
        }
    }
}
long r_frames(void){ return g_frames; }
void r_capture_target(int face){
    glass_flush();
    close_draw();
    cmd_room(4);
    g_cmd[g_ncmd++] = C_TARGET; g_cmd[g_ncmd++] = (uint32_t)face;
    g_cap = face >= 0;
    dirty_all();
}
void r_capture_mips(int face){
    g_cube_faces++;
    cmd_room(4);
    g_cmd[g_ncmd++] = C_CUBE_MIPS; g_cmd[g_ncmd++] = (uint32_t)face;
}
void r_cube_reset(void){ g_cube_faces = 0; }
void r_capture_phase(int clear_alpha, int opaque_gain){ g_cap_clear = clear_alpha; g_cap_opaque = opaque_gain; }
void r_capture_sun(int on){ g_cap_sun = on; }
int r_window_mode(int *w, int *h){
    *w = __atomic_load_n(&g_ring.win_w, __ATOMIC_ACQUIRE); *h = __atomic_load_n(&g_ring.win_h, __ATOMIC_ACQUIRE);
    return *w > 640 && *h > 480;
}
