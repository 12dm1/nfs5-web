#include <pthread.h>
/* nfs5-web: the THRASH driver as host functions (WEB1: headless).
 *
 * PU's renderer is a driver DLL (THRASH_opendll: LoadLibraryA of
 * THRASH_DRIVERNAME / the "Thrash Driver" setting, default dx7z.dll) whose 40
 * stdcall exports are fetched by decorated name ("_THRASH_about@0" ...).
 * win32.c answers LoadLibraryA of an unknown DLL with a placeholder handle and
 * GetProcAddress by searching the import table by name, so this file adds the
 * 40 names as extra "imports" (g_pu_imp, appended in win32_init) and serves
 * them from pu_call().
 *
 * The values follow our PC driver (_mod/wrapper/src/thrash_exports.cpp, the
 * dx11z THRASH wrapper): about struct layout, the 10 video modes, the state
 * defaults, the window descriptor and the lock descriptor.  WEB2 puts dx11z
 * itself behind these entry points; here nothing is drawn, only counted. */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { uint32_t iat; const char*dll; const char*name; int nargs; } ImpEnt;
uint32_t gheap_alloc(int heap, uint32_t n, int zero);
void mem_commit(uint32_t, uint32_t);

#define T(n, a) {0u, "dx11z.dll", "_THRASH_" #n "@" #a, (a) / 4}
const ImpEnt g_pu_imp[] = {
    T(about,0), T(clearwindow,0), T(clip,16), T(createwindow,16), T(destroywindow,4),
    T(drawline,8), T(drawlinemesh,12), T(drawlinestrip,12), T(drawlinestrip,8), T(drawpoint,4),
    T(drawpointmesh,12), T(drawpointstrip,8), T(drawquad,16), T(drawquadmesh,12), T(drawsprite,8),
    T(drawspritemesh,12), T(drawtri,12), T(drawtrifan,12), T(drawtrifan,8), T(drawtrimesh,12),
    T(drawtristrip,12), T(drawtristrip,8), T(flushwindow,0), T(getstate,4), T(getwindowtexture,4),
    T(idle,0), T(init,0), T(is,0), T(lockwindow,0), T(pageflip,0),
    T(readrect,20), T(restore,0), T(selectdisplay,4), T(setstate,8), T(settexture,4),
    T(setvideomode,12), T(sync,4), T(talloc,20), T(tfree,4), T(treset,0),
    T(tupdate,12), T(tupdaterect,36), T(unlockwindow,4), T(window,4), T(writerect,20),
    /* gimme.dll!ConvertFile(src, dst, index): cdecl (the caller pops), called
     * unchecked after GetProcAddress (0x4DBE36); the PC mod has its own
     * (wrapper driver_picture.cpp), here: pu_imports.c */
    {0u, "gimme.dll", "ConvertFile", 0},
};
const uint32_t g_pu_imp_count = sizeof g_pu_imp / sizeof *g_pu_imp;

/* state ids: _mod/wrapper/include/thrash_api.h */
#define ST_CLEAR_COLOR   0x03
#define ST_HWND          0x19   /* the game's HWND, set before init */
#define ST_WINDOW_DESC   0x1D   /* get: the current window's descriptor */
#define ST_ZCOMPARE      0x28
#define ST_SCREEN_W      0x12F
#define ST_SCREEN_H      0x132
#define NSTATE 0x400

static uint32_t g_state[NSTATE];
static uint32_t g_about, g_modes, g_fmtflags, g_fmtdescs, g_build, g_windesc, g_lockdesc, g_lockbuf;
static uint32_t g_lockw, g_lockh, g_curwin, g_ntex;
static long g_frames;
static uint64_t g_draws, g_tex_up, g_calls;
static int g_log = -1;

static const uint32_t k_modes[11][3] = {
    {0,0,0}, {640,480,16}, {640,480,32}, {800,600,16}, {800,600,32}, {1024,768,16},
    {1024,768,32}, {1280,960,16}, {1280,960,32}, {1600,1200,16}, {1600,1200,32},
};

static void state_init(void){
    uint32_t hwnd = g_state[ST_HWND];
    memset(g_state, 0, sizeof g_state);
    g_state[ST_HWND] = hwnd;
    g_state[ST_SCREEN_W] = 640; g_state[ST_SCREEN_H] = 480;
    g_state[ST_CLEAR_COLOR] = 0xFF000000u;
    g_state[ST_ZCOMPARE] = 3;                   /* LEQUAL (dx7z) */
}

static uint32_t about(void){
    if (!g_about){
        g_about    = gheap_alloc(1, 0xD0, 1);
        g_modes    = gheap_alloc(1, 14 * 0x28, 1);
        g_fmtflags = gheap_alloc(1, 21 * 4, 1);
        g_fmtdescs = gheap_alloc(1, 24 * 4, 1);
        g_build    = gheap_alloc(1, 64, 1);
        g_windesc  = gheap_alloc(1, 8 * 64, 1);
        g_lockdesc = gheap_alloc(1, 0x1C, 1);
        static const uint32_t ff[21] = {0,0,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0};   /* 32-bit textures */
        for (int i = 0; i < 21; i++) MR32(g_fmtflags + 4u * i) = ff[i];
        for (int i = 1; i <= 10; i++){
            uint32_t m = g_modes + 0x28u * (uint32_t)i;
            MR32(m) = k_modes[i][0]; MR32(m + 4) = k_modes[i][1]; MR32(m + 8) = k_modes[i][2];
            MR32(m + 12) = k_modes[i][2] == 16 ? 4u : 5u;     /* RGB565 / ARGB8888 */
            MR32(m + 16) = 1; MR32(m + 20) = 2; MR32(m + 24) = 2;
        }
        strcpy((char *)(g_mem + g_build), "nfs5-web THRASH HLE (headless)");
    }
    uint32_t a = g_about;
    memset(g_mem + a, 0, 0xD0);
    MR32(a + 0x00) = 0x33444632u;               /* "3DF2" */
    MR32(a + 0x04) = 0xD0;
    MR32(a + 0x08) = 0x6B; MR32(a + 0x0C) = 0x4F;
    MR32(a + 0x10) = 1; MR32(a + 0x14) = 256; MR32(a + 0x18) = 1; MR32(a + 0x1C) = 1;
    MR32(a + 0x20) = 256; MR32(a + 0x24) = 1; MR32(a + 0x28) = 1;
    MR32(a + 0x2C) = 21; MR32(a + 0x30) = g_fmtflags;
    MR32(a + 0x34) = 4;  MR32(a + 0x38) = g_fmtdescs;
    MR32(a + 0x3C) = 10; MR32(a + 0x40) = g_modes;
    memcpy(g_mem + a + 0x4C, "nfs5-web HLE   ", 16);
    MR32(a + 0x6C) = 2;
    MR32(a + 0x70) = 32u << 20;                 /* texture memory: 32 MB (see wrapper G2) */
    MR32(a + 0x74) = 1;
    MR32(a + 0x78) = g_build; MR32(a + 0x7C) = 5;
    return a;
}

static void window_desc(uint32_t h){
    uint32_t w = g_state[ST_SCREEN_W], hh = g_state[ST_SCREEN_H];
    uint32_t d = g_windesc + 64u * h;
    MR32(d) = 0; MR32(d + 4) = w * 2u; MR32(d + 8) = 4; MR32(d + 12) = w; MR32(d + 16) = hh;
    g_state[ST_WINDOW_DESC] = d;
}

/* headless frame pacing: the game paces itself on QPC; this only keeps a
 * runaway loop from burning a core (and gives the log a frame clock) */
static void pace(void){
    static uint64_t last;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t now = (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
    if (last && now - last < 16000u){
        struct timespec s = {0, (long)(16000u - (now - last)) * 1000L}; nanosleep(&s, 0);
        clock_gettime(CLOCK_MONOTONIC, &t); now = (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
    }
    last = now;
}

/* WEB2: the drawing side on dx11z's D3D11 core (renderer/thrash_dx11.cpp);
 * weak, so a build without the renderer (or NFS5_HEADLESS=1) stays headless */
__attribute__((weak)) int r_init(int w, int h){ (void)w; (void)h; return 0; }
void r_restore(void) __attribute__((weak));
void r_setvideomode(int w, int h, int bpp) __attribute__((weak));
void r_setstate(int id, int v) __attribute__((weak));
int  r_getstate(int id) __attribute__((weak));
void r_window(int idx) __attribute__((weak));
void r_clearwindow(void) __attribute__((weak));
void r_flushwindow(void) __attribute__((weak));
void r_clip(int x1, int y1, int x2, int y2) __attribute__((weak));
void r_pageflip(void) __attribute__((weak));
void r_unlockwindow(uint32_t pixels, int w, int h, int pitch) __attribute__((weak));
int  r_readrect(int x, int y, int w, int h, uint32_t buf) __attribute__((weak));
uint32_t r_talloc(int w, int h, int fmt, int flags, int mips) __attribute__((weak));
void r_tfree(uint32_t t) __attribute__((weak));
void r_tupdate(uint32_t t, uint32_t data, int palette) __attribute__((weak));
void r_tupdaterect(uint32_t t, int mip, int x, int y, int w, int h, int pitch, uint32_t data, int fmt) __attribute__((weak));
void r_treset(void) __attribute__((weak));
void r_settexture(uint32_t t) __attribute__((weak));
void r_draw(const char *n, const uint32_t *a) __attribute__((weak));
static int g_rend;          /* 1: the D3D11 renderer is up */

/* the game's pixel format table (0x5DF16C, 5 dwords per THRASH format):
 * entry 0 (ARGB1555) is left zero by the EXE and dx7z fills it; a zero
 * bits-per-pixel divides by zero (wrapper THRASH_init does the same) */
static void patch_format_table(void){
    uint32_t t = 0x005DF16Cu;
    if (!MR32(t)){ MR32(t) = 16; MR32(t + 4) = 1; MR32(t + 8) = 5; MR32(t + 12) = 5; MR32(t + 16) = 5; }
}

#define ARG(i) MR32(c->esp + 4u * (uint32_t)(i))
static int thrash_call_locked(CPU *c, const char *name, uint32_t *r);
/* One thread at a time in the renderer: r_pageflip releases the guest lock
 * while Present waits for the GPU (so the timer, sound and window threads
 * keep running), and another guest thread may call THRASH meanwhile.  A
 * caller that has to wait releases the guest lock too (no deadlock with the
 * presenting thread, which wants it back). */
static pthread_mutex_t g_rmx = PTHREAD_MUTEX_INITIALIZER;
void guest_wait_begin(void); void guest_wait_end(void);
int thrash_call(CPU *c, const char *name, uint32_t *r){
    if (strncmp(name, "_THRASH_", 8)) return 0;
    if (pthread_mutex_trylock(&g_rmx) != 0){ guest_wait_begin(); pthread_mutex_lock(&g_rmx); guest_wait_end(); }
    int k = thrash_call_locked(c, name, r);
    pthread_mutex_unlock(&g_rmx);
    return k;
}
static int thrash_call_locked(CPU *c, const char *name, uint32_t *r){
    if (g_log < 0) g_log = getenv("THRASHLOG") ? 1 : 0;
    const char *n = name + 8;
    g_calls++;
    *r = 0;
    if (!strncmp(n, "draw", 4)){ g_draws++; if (g_rend) r_draw(n, (const uint32_t *)(g_mem + c->esp)); return 1; }
    if      (!strcmp(n, "about@0")){ *r = about(); fprintf(stderr, "[thrash] about -> 0x%08x\n", *r); }
    else if (!strcmp(n, "init@0")){
        state_init(); if (!getenv("NO_FMTPATCH")) patch_format_table(); *r = 1;
        const char *hl = getenv("NFS5_HEADLESS");
        if (!(hl && *hl && *hl != '0') && !g_rend){
            g_rend = r_init((int)g_state[ST_SCREEN_W], (int)g_state[ST_SCREEN_H]);
            if (!g_rend) fprintf(stderr, "[thrash] no D3D11 renderer: headless\n");
        }
        fprintf(stderr, "[thrash] init (hwnd 0x%x)%s\n", g_state[ST_HWND], g_rend ? " -> D3D11" : ""); }
    else if (!strcmp(n, "is@0")) *r = 1;
    else if (!strcmp(n, "restore@0")){ *r = 1; if (g_rend){ r_restore(); g_rend = 0; } fprintf(stderr, "[thrash] restore\n"); }
    else if (!strcmp(n, "selectdisplay@4")) *r = 1;
    else if (!strcmp(n, "setvideomode@12")){
        uint32_t i = ARG(0);
        if (i < 1 || i > 10) *r = 0;
        else { g_state[ST_SCREEN_W] = k_modes[i][0]; g_state[ST_SCREEN_H] = k_modes[i][1]; *r = 1;
               if (g_rend) r_setvideomode((int)k_modes[i][0], (int)k_modes[i][1], (int)k_modes[i][2]); }
        fprintf(stderr, "[thrash] setvideomode(%u, %u, %u) -> %u  (%ux%u %u bpp)\n", i, ARG(1), ARG(2), *r,
                i <= 10 ? k_modes[i][0] : 0, i <= 10 ? k_modes[i][1] : 0, i <= 10 ? k_modes[i][2] : 0); }
    else if (!strcmp(n, "setstate@8")){ uint32_t id = ARG(0); if (id < NSTATE) g_state[id] = ARG(1); *r = 1;
        if (g_rend && id != ST_HWND && id != ST_WINDOW_DESC) r_setstate((int)id, (int)ARG(1));
        if (g_log) fprintf(stderr, "[thrash] setstate(0x%x, 0x%x)\n", id, ARG(1)); }
    else if (!strcmp(n, "getstate@4")){ uint32_t id = ARG(0); *r = id < NSTATE ? g_state[id] : 0;
        if (g_rend && id != ST_HWND && id != ST_WINDOW_DESC) *r = (uint32_t)r_getstate((int)id);
        if (g_log) fprintf(stderr, "[thrash] getstate(0x%x) -> 0x%x\n", id, *r); }
    else if (!strcmp(n, "window@4")){ uint32_t h = ARG(0);
        if (h <= 7){ g_curwin = h; window_desc(h); if (g_rend) r_window((int)h); } *r = 1; }
    else if (!strcmp(n, "createwindow@16") || !strcmp(n, "destroywindow@4")) *r = 1;
    else if (!strcmp(n, "getwindowtexture@4")) *r = 0;
    else if (!strcmp(n, "lockwindow@0")){
        uint32_t w = g_state[ST_SCREEN_W], h = g_state[ST_SCREEN_H];
        if (!g_lockbuf || w != g_lockw || h != g_lockh){ g_lockbuf = gheap_alloc(1, w * h * 2u, 1); g_lockw = w; g_lockh = h; }
        uint32_t d = g_lockdesc;
        MR32(d) = g_lockbuf; MR32(d + 4) = w * 2u; MR32(d + 8) = 4; MR32(d + 12) = w; MR32(d + 16) = h;
        MR32(d + 20) = g_curwin == 1 ? 0u : 1u; MR32(d + 24) = 3;
        *r = d; }
    else if (!strcmp(n, "pageflip@0")){
        g_frames++;
        { void watchdog_on_present(void); watchdog_on_present(); }
        if (g_rend) r_pageflip(); else pace();
        if (g_frames <= 3 || g_frames % 300 == 0)
            fprintf(stderr, "[thrash] frame %ld: %llu draws, %u textures, %llu uploads, %ux%u\n", g_frames,
                    (unsigned long long)g_draws, g_ntex, (unsigned long long)g_tex_up,
                    g_state[ST_SCREEN_W], g_state[ST_SCREEN_H]); }
    else if (!strcmp(n, "talloc@20")){ *r = g_rend ? r_talloc((int)ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4)) : g_ntex + 1u; ++g_ntex;
        if (g_log) fprintf(stderr, "[thrash] talloc(%u, %u, fmt %u, flags 0x%x, mips %u) -> %u\n",
                           ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), *r); }
    else if (!strcmp(n, "tupdate@12")){ g_tex_up++; if (g_rend) r_tupdate(ARG(0), ARG(1), (int)ARG(2)); }
    else if (!strcmp(n, "tupdaterect@36")){ g_tex_up++;
        if (g_rend) r_tupdaterect(ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), (int)ARG(5), (int)ARG(6), ARG(7), (int)ARG(8)); }
    else if (!strcmp(n, "readrect@20")) *r = g_rend ? (uint32_t)r_readrect((int)ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), ARG(4)) : 1u;
    else if (g_rend && !strcmp(n, "tfree@4")) r_tfree(ARG(0));
    else if (g_rend && !strcmp(n, "treset@0")) r_treset();
    else if (g_rend && !strcmp(n, "settexture@4")) r_settexture(ARG(0));
    else if (g_rend && !strcmp(n, "clearwindow@0")) r_clearwindow();
    else if (g_rend && !strcmp(n, "flushwindow@0")) r_flushwindow();
    else if (g_rend && !strcmp(n, "clip@16")) r_clip((int)ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3));
    else if (g_rend && !strcmp(n, "unlockwindow@4")){
        uint32_t d = ARG(0);
        if (d == g_lockdesc && g_lockbuf) r_unlockwindow(g_lockbuf, (int)g_lockw, (int)g_lockh, (int)(g_lockw * 2u)); }
    /* clearwindow, flushwindow, unlockwindow, clip, sync, idle, tfree, treset, settexture, writerect: nothing */
    return 1;
}

long metal_frames(void){ return g_frames; }
void metal_game_size(unsigned *w, unsigned *h){ *w = g_state[ST_SCREEN_W] ? g_state[ST_SCREEN_W] : 640; *h = g_state[ST_SCREEN_H] ? g_state[ST_SCREEN_H] : 480; }
void thrash_report(void){
    fprintf(stderr, "\n=== THRASH (headless): %llu calls, %ld frames, %llu draws, %u textures, %llu uploads ===\n",
            (unsigned long long)g_calls, g_frames, (unsigned long long)g_draws, g_ntex, (unsigned long long)g_tex_up);
}
