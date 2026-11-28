/* Car paint for every car (PC wrapper car_paint.cpp CP1, car_shading.cpp CAR1). Browser only.
 * - The page builder 00443F60 tints a car's page with 00444130: a grey source texel whose palette alpha names a colour band becomes band
 *   colour x source red. Each tinted texel is recorded (band, red, the texel the blit writes); when the page is uploaded the record is
 *   checked against the uploaded data (numbers, plates drawn later fall out) and becomes a paint map (R = paint, G = shine kept: source
 *   red over the band's panel level, so seams and grilles go matte). r_web.c sends it to the page with the texture.
 * - The car light layer 00446530 lights every car batch from its object normals; after it the camera-space normal of each output vertex
 *   goes into the vertex's specular dword (zero for cars in the game), one byte per axis, which the paint shader decodes. */
#ifdef __wasm__
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "decls.h"
#include "pu_cars.h"

uint32_t pu_guest_call(CPU *c, uint32_t fn, uint32_t ecx, const uint32_t *args, int n);
extern int g_cam_capture;

static int g_file = -1;
static uint32_t g_serial;

/* ---- the paint map ---- */
#define PAGES 48                    /* two builds a car, 16 cars (CARS2) + the showroom: a page evicted before its upload got another car's map (stripes) */
typedef struct {
    uint32_t bits;                  /* guest address of the page's texels */
    int stride, rows, fmt, file, consumed;
    uint32_t serial, paint;
    uint8_t *band, *red;
    uint32_t *expect;
} Page;
static Page g_pages[PAGES];
static int g_next_page;

static int band_of_alpha(uint8_t a){
    const int k = ((a + 1) * 20) >> 8;
    if (k == 0 || k == 10 || k == 20) return -1;
    return 19 - k > 9 ? 18 - k : 19 - k;
}
static uint32_t tinted_texel(uint32_t pal, uint32_t band, int fmt){
    const uint32_t red = (pal >> 16) & 0xFF;
    const uint32_t r = ((band >> 16) & 0xFF) * red >> 8, g = ((band >> 8) & 0xFF) * red >> 8, b = (band & 0xFF) * red >> 8;
    const uint32_t a = (pal >> 24) ? 0xFFu : 0u;
    if (fmt == 0x613) return ((((a ? 1u : 0u) << 8 | (r & 0xF8)) << 5 | (g & 0xF8)) << 2) | ((b >> 3) & 0x1F);
    if (fmt == 0x115C) return (a & 0xF0) << 8 | (r & 0xF0) << 4 | (g & 0xF0) | (b >> 4);
    return a << 24 | r << 16 | g << 8 | b;
}
static int fmt16(int fmt){ return fmt == 0x613 || fmt == 0x115C; }
static int fmt_fits(int blit, int slot){ return (blit == 0x613 && slot == 3) || (blit == 0x115C && slot == 7) || (blit == 0x20 && slot == 6); }
static uint32_t raw_at(const uint8_t *data, int f16, size_t t){
    if (f16){ uint16_t v; memcpy(&v, data + t * 2, 2); return v; }
    uint32_t v; memcpy(&v, data + t * 4, 4); return v;
}

static Page *page_for(uint32_t bits, int stride, int fmt){
    for (int i = 0; i < PAGES; i++){
        Page *p = &g_pages[i];
        if (p->bits == bits && p->serial == g_serial && p->stride == stride && p->fmt == fmt) return p;
    }
    Page *p = &g_pages[g_next_page];
    g_next_page = (g_next_page + 1) % PAGES;
    if (p->band && !p->consumed && getenv("PAINTLOG")) fprintf(stderr, "[paintlog] page %08x evicted before its upload (%d paint texels)\n", p->bits, (int)p->paint);
    free(p->band); free(p->red); free(p->expect);
    memset(p, 0, sizeof *p);
    p->bits = bits; p->stride = stride; p->fmt = fmt; p->serial = g_serial; p->file = g_file;
    return p;
}
static void record(CPU *c, uint32_t page, uint32_t image, uint32_t tint, uint32_t fmt, uint32_t bands, int x, int y){
    const int stride = (int)MR32(page + 4);
    uint32_t bits = MR32(page + 0x28);
    if (!bits || stride <= 0 || stride > 4096) return;
    bits += 0x10;
    int16_t w16, h16; memcpy(&w16, g_mem + image + 4, 2); memcpy(&h16, g_mem + image + 6, 2);
    const int w = w16, h = h16;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    const int32_t pos = (int32_t)MR32(image + 0xC);
    const int x0 = ((pos << 20) >> 20) + x, y0 = ((pos << 4) >> 20) + y;
    if (y0 < 0 || y0 + h > 4096) return;
    Page *p = page_for(bits, stride, (int)fmt);
    if (y0 + h > p->rows){
        const size_t n = (size_t)p->stride * (size_t)(y0 + h), o = (size_t)p->stride * (size_t)p->rows;
        p->band = realloc(p->band, n); p->red = realloc(p->red, n); p->expect = realloc(p->expect, n * 4);
        memset(p->band + o, 0, n - o); memset(p->red + o, 0, n - o); memset(p->expect + o, 0, (n - o) * 4);
        p->rows = y0 + h;
    }
    const uint32_t bpp = pu_guest_call(c, 0x00537F90u, 0, &image, 1);
    const int tinted = (tint & 0xFF) && ((bpp == 8 && (fmt == 0x20 || fmt == 0x613 || fmt == 0x115C)) || (bpp == 32 && (fmt == 0x20 || fmt == 0x613)));
    uint8_t pal_band[256] = {0}, pal_red[256] = {0};
    uint32_t pal_raw[256] = {0};
    if (tinted && bpp == 8){
        const uint32_t pal = pu_guest_call(c, 0x00537FB0u, 0, &image, 1) + 0x10;
        for (int i = 0; i < 255; i++){      /* the blit converts 255 entries */
            const uint32_t e = MR32(pal + 4u * (uint32_t)i);
            const int b = band_of_alpha((uint8_t)(e >> 24));
            if (b < 0 || b == 14 || b == 15) continue;      /* untinted, interior colour */
            pal_band[i] = (uint8_t)(b + 1); pal_red[i] = (uint8_t)(e >> 16);
            pal_raw[i] = tinted_texel(e, MR32(bands + 4u * (uint32_t)b), (int)fmt);
        }
    }
    const uint8_t *src8 = g_mem + image + 0x10;
    for (int j = 0; j < h; j++){
        const size_t row = (size_t)(y0 + j) * (size_t)p->stride;
        for (int i = 0; i < w; i++){
            const int tx = x0 + i;
            if (tx < 0 || tx >= p->stride) continue;
            const size_t t = row + (size_t)tx;
            uint8_t band = 0, red = 0; uint32_t raw = 0;
            if (tinted && bpp == 8){ const uint8_t k = src8[(size_t)j * w + i]; band = pal_band[k]; red = pal_red[k]; raw = pal_raw[k]; }
            else if (tinted){
                uint32_t e; memcpy(&e, src8 + ((size_t)j * w + i) * 4, 4);
                const int b = band_of_alpha((uint8_t)(e >> 24));
                if (b >= 0 && b != 14 && b != 15){ band = (uint8_t)(b + 1); red = (uint8_t)(e >> 16); raw = tinted_texel(e, MR32(bands + 4u * (uint32_t)b), (int)fmt); }
            }
            if (p->band[t]) p->paint--;
            p->band[t] = band; p->red[t] = red; p->expect[t] = raw;
            if (band) p->paint++;
        }
    }
}
/* ---- GLASS1: the glass pages' map (the builder's files 3 and 4: tpage5 "glass", window=1, and tpage6 "intglass", global) ----
 * The builder blits the window image onto the glass page; its see-through texels (alpha between the clear cut-outs and the
 * opaque seals and frames: 128 in a 32-bit source, 68 on the 4444 showroom pages; on the intglass page any but the frame) are
 * intact glass. Each blit's texels are
 * kept as the blit left them (the page may be uploaded inside the build); at the upload a texel still the same and
 * see-through gets G = 255 in the page's map (R = 0: no paint). A texel drawn over later (the broken-glass image a damaged
 * window gets) differs and keeps the game's look, as does a texel a later blit of the build covers. r_web.c draws the page
 * as the game does, then the glass shader's reflection pass over it, and masks the game's sphere-map pass off the intact glass.
 * GLASS=0 turns it off. */
#define GLASS_PAGES 24
typedef struct {
    uint32_t bits, serial;
    int stride, rows, fmt, blits, consumed;
    uint8_t *mask;                  /* 1: see-through glass of the build's first blit */
    uint32_t *snap;                 /* the texel as the build left it */
} GlassPage;
static GlassPage g_glass[GLASS_PAGES];
static int g_glass_next;
static int glass_log_on(void){ static int on = -1; if (on < 0){ const char *e = getenv("GLASSLOG"); on = e && *e && *e != '0'; } return on; }
static int glass_on(void){ static int on = -1; if (on < 0){ const char *e = getenv("GLASS"); on = !(e && *e == '0'); } return on; }
static int texel_alpha(int fmt, uint32_t v){
    if (fmt == 0x115C) return (int)((v >> 12) & 0xF) * 17;
    if (fmt == 0x613) return (v & 0x8000u) ? 255 : 0;
    return (int)(v >> 24);
}
static GlassPage *glass_for(uint32_t bits, int stride, int fmt){
    for (int i = 0; i < GLASS_PAGES; i++){
        GlassPage *g = &g_glass[i];
        if (g->bits == bits && g->serial == g_serial && g->stride == stride && g->fmt == fmt) return g;
    }
    GlassPage *g = &g_glass[g_glass_next];
    g_glass_next = (g_glass_next + 1) % GLASS_PAGES;
    free(g->mask); free(g->snap);
    memset(g, 0, sizeof *g);
    g->bits = bits; g->stride = stride; g->fmt = fmt; g->serial = g_serial;
    return g;
}
/* after the blit: its texels as it left them */
static void glass_blit(CPU *c, uint32_t page, uint32_t image, uint32_t tint, uint32_t fmt, int x, int y){
    const int stride = (int)MR32(page + 4);
    uint32_t bits = MR32(page + 0x28);
    if (!bits || stride <= 0 || stride > 1024) return;
    bits += 0x10;
    int16_t w16, h16; memcpy(&w16, g_mem + image + 4, 2); memcpy(&h16, g_mem + image + 6, 2);
    const int w = w16, h = h16;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return;
    const int32_t pos = (int32_t)MR32(image + 0xC);
    const int x0 = ((pos << 20) >> 20) + x, y0 = ((pos << 4) >> 20) + y;
    if (y0 < 0 || y0 + h > 1024) return;
    GlassPage *g = glass_for(bits, stride, (int)fmt);
    if (y0 + h > g->rows){
        const size_t n = (size_t)stride * (size_t)(y0 + h), o = (size_t)stride * (size_t)g->rows;
        uint8_t *m = realloc(g->mask, n); uint32_t *sn = realloc(g->snap, n * 4);
        if (m) g->mask = m;
        if (sn) g->snap = sn;
        if (!m || !sn) return;
        memset(g->mask + o, 0, n - o); memset(g->snap + o, 0, (n - o) * 4);
        g->rows = y0 + h;
    }
    if (g->blits < 255) g->blits++;
    const int f16 = fmt16(g->fmt);
    uint32_t glass = 0, hist[16] = {0};
    /* the glass page's window alpha (the most common one between the cut-outs and the seals: 128 in a 32-bit source, 68 on
     * the 4444 showroom pages); the fainter texels round it are the edges by the seals, and with the reflection at full
     * strength they lit the seals' outline */
    int amin = 0x10;
    if (g_file == 3){
        uint32_t ah[256] = {0};
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++){
            const int tx = x0 + i;
            if (tx >= 0 && tx < stride) ah[texel_alpha(g->fmt, raw_at(g_mem + bits, f16, (size_t)(y0 + j) * (size_t)stride + (size_t)tx))]++;
        }
        int mode = -1;
        for (int a = 0x10; a <= 0xB0; a++) if (ah[a] && (mode < 0 || ah[a] > ah[mode])) mode = a;
        if (mode > 0) amin = mode * 3 / 4 > 0x10 ? mode * 3 / 4 : 0x10;
    }
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++){
        const int tx = x0 + i;
        if (tx < 0 || tx >= stride) continue;
        const size_t t = (size_t)(y0 + j) * (size_t)stride + (size_t)tx;
        const uint32_t v = raw_at(g_mem + bits, f16, t);
        const int a = texel_alpha(g->fmt, v);
        g->snap[t] = v;
        /* not the opaque seals and frames; on the glass page not its clear cut-outs either. The intglass page is window but for
         * its frame: the race's image (INTGLASS.FSH "glat") is clear, alpha 0, the menus' ("glai") a light tint */
        g->mask[t] = g->blits == 1 && a <= 0xB0 && (a >= amin || g_file == 4);
        glass += g->mask[t]; hist[a >> 4]++;
    }
    if (glass_log_on()){
        const uint32_t bpp = pu_guest_call(c, 0x00537F90u, 0, &image, 1);
        fprintf(stderr, "[glass] build page %08x stride %d fmt %x: blit %d image %08x %dx%d at %d,%d bpp %u tint %x: %u see-through texels; alpha by 16:",
                bits, stride, fmt, g->blits, image, w, h, x0, y0, bpp, tint, glass);
        for (int k = 0; k < 16; k++) fprintf(stderr, " %u", hist[k]);
        fprintf(stderr, "\n");
    }
}
/* r_web.c r_tupdate (through paint_map): the glass map of an upload, the number of intact glass texels */
/* a page's texel (builder formats 0x613 1555, 0x115C 4444, 0x20 8888) or an upload's (slot formats 3, 7, 6) as A8R8G8B8 */
static uint32_t texel_argb(int fmt, uint32_t v){
    if (fmt == 0x613 || fmt == 3){
        const uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        return ((v & 0x8000u) ? 0xFF000000u : 0u) | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
    }
    if (fmt == 0x115C || fmt == 7) return ((v >> 12) & 15) * 17u << 24 | ((v >> 8) & 15) * 17u << 16 | ((v >> 4) & 15) * 17u << 8 | (v & 15) * 17u;
    return v;
}
static int coarse(int fmt){ return fmt == 0x115C || fmt == 7 ? 17 : fmt == 0x613 || fmt == 3 ? 8 : 0; }
/* the build's texel and the upload's are the same within the coarser format's step: the race's glass page is built in 32
 * bits and uploaded as a 4444 copy */
static int texel_same(uint32_t a, uint32_t b, int tol){
    if (!tol) return a == b;
    for (int k = 0; k < 32; k += 8){ const int d = (int)((a >> k) & 255) - (int)((b >> k) & 255); if (d > tol || d < -tol) return 0; }
    return 1;
}
static int glass_map(uint32_t data, int w, int h, int fmt, uint8_t *rg){
    if (fmt != 3 && fmt != 6 && fmt != 7) return 0;
    const int up16 = fmt != 6;
    GlassPage *page = NULL;
    float best = 0.0f;
    for (int i = 0; i < GLASS_PAGES; i++){        /* by content, as the paint: the build the upload matches best */
        GlassPage *g = &g_glass[i];
        if (!g->mask || g->stride != w || g->rows > h) continue;
        const int tol = coarse(g->fmt) > coarse(fmt) ? coarse(g->fmt) : coarse(fmt);
        const size_t n = (size_t)g->stride * (size_t)g->rows, step = n / 2048 + 1;
        uint32_t seen = 0, same = 0, seen_f = 0, same_f = 0;
        for (size_t t = 0; t < n; t += step){
            const int eq = texel_same(texel_argb(fmt, raw_at(g_mem + data, up16, t)), texel_argb(g->fmt, g->snap[t]), tol);
            if (g->mask[t]){ seen++; same += eq; } else { seen_f++; same_f += eq; }
        }
        float r = seen ? (float)same / (float)seen : 0.0f;
        /* the frame too: the clear intglass page is all transparent black but for its frame, as are other, empty pages */
        /* a real copy matches all but the format's rounding (1.00 in the logs); half matches were room textures with soft
         * alpha (the showroom's spotlight cones) taken for the gradient intglass page */
        if (seen < 64 || seen_f < 16 || same_f * 10 < seen_f * 9) r = 0.0f;
        if (g->consumed) r -= 0.001f;
        if (g->bits == data) r += 0.0005f;
        if (r > best){ best = r; page = g; }
    }
    if (!page || best < 0.9f) return 0;
    const int tol = coarse(page->fmt) > coarse(fmt) ? coarse(page->fmt) : coarse(fmt);
    const size_t n = (size_t)page->stride * (size_t)page->rows;
    memset(rg, 0, (size_t)w * h * 2);
    uint32_t kept = 0, broken = 0;
    for (size_t t = 0; t < n; t++){
        if (!page->mask[t]) continue;
        if (!texel_same(texel_argb(fmt, raw_at(g_mem + data, up16, t)), texel_argb(page->fmt, page->snap[t]), tol)){ broken++; continue; }
        rg[t * 2 + 1] = 255; kept++;
    }
    /* the page's opaque seals and frame: G = 100, under the glass pass's 0.5 (no reflection) but enough to keep the game's
     * sphere-map pass off them (it lit the rubber grey, like a specular) */
    uint32_t seals = 0;
    for (size_t t = 0; t < n; t++)
        if (!page->mask[t] && texel_alpha(page->fmt, page->snap[t]) >= 0xC0 &&
            texel_same(texel_argb(fmt, raw_at(g_mem + data, up16, t)), texel_argb(page->fmt, page->snap[t]), tol)){ rg[t * 2 + 1] = 100; seals++; }
    if (glass_log_on()) fprintf(stderr, "[glass] %u seal and frame texels off the stock reflection\n", seals);
    page->consumed = 1;
    if (glass_log_on()) fprintf(stderr, "[glass] upload %08x %dx%d fmt %d (built as %x): %u intact glass texels, %u drawn over (build match %.2f)\n", data, w, h, fmt, page->fmt, kept, broken, best);
    return (int)kept;
}
/* ---- WHEEL1: the rim's page (the builder's file 3, tpage4 128 x 128: the rim image whg/whf, 64 x 64 at (0, 0), on every car).
 * Its upload becomes paint kind 4 (r_web.c): drawn by the renderer's fs_wheel, which smears the rim quarter round its centre by the
 * wheels' turn in one frame (pu_wheel_spin). WHEELBLUR=0 off, WHEELLOG=1 the pages and the speed. ---- */
#define WHEELS 64
/* the rim quarter as the blit left it (the page's memory is used again before its upload): its texels in the build's format */
static uint32_t g_wheel_bits[WHEELS], g_wheel_fmt[WHEELS], g_wheel_sig[WHEELS][64 * 64]; static int g_wheel_next;
static int wheel_on(void){ static int on = -1; if (on < 0){ const char *e = getenv("WHEELBLUR"); on = !(e && *e == '0'); } return on; }
static int wheel_log(void){ static int on = -1; if (on < 0){ const char *e = getenv("WHEELLOG"); on = e && *e && *e != '0'; } return on; }
static void wheel_blit(uint32_t page, uint32_t image, uint32_t fmt, int x, int y){
    const int stride = (int)MR32(page + 4);
    const uint32_t bits = MR32(page + 0x28);
    if (!bits || stride != 128 || (fmt != 0x613 && fmt != 0x115C && fmt != 0x20)) return;
    int16_t w16, h16; memcpy(&w16, g_mem + image + 4, 2); memcpy(&h16, g_mem + image + 6, 2);
    const int32_t pos = (int32_t)MR32(image + 0xC);
    const int x0 = ((pos << 20) >> 20) + x, y0 = ((pos << 4) >> 20) + y;
    if (w16 != 64 || h16 != 64 || x0 != 0 || y0 != 0) return;
    /* kept by content: the page's memory goes to the next car's build too (another rim over the same bits), and a car's texture can
     * go up again later (a reload): every rim seen stays matchable, one slot per different image */
    const int f16 = fmt16((int)fmt);
    static uint32_t sig[64 * 64];
    for (int j = 0; j < 64; j++) for (int i = 0; i < 64; i++) sig[j * 64 + i] = raw_at(g_mem + bits + 0x10, f16, (size_t)j * 128u + (size_t)i);
    for (int i = 0; i < WHEELS; i++) if (g_wheel_bits[i] && g_wheel_fmt[i] == fmt && !memcmp(g_wheel_sig[i], sig, sizeof sig)) return;
    const int k = g_wheel_next; g_wheel_next = (g_wheel_next + 1) % WHEELS;
    g_wheel_bits[k] = bits + 0x10; g_wheel_fmt[k] = fmt;
    memcpy(g_wheel_sig[k], sig, sizeof sig);
    if (wheel_log()) fprintf(stderr, "[wheel] rim page %08x fmt %x\n", bits + 0x10, fmt);
}
/* an upload whose top-left quarter is a rim as built */
static int wheel_map(uint32_t data, int w, int h, int fmt){
    if (w != 128 || h != 128) return 0;
    int best = 0;
    for (int i = 0; i < WHEELS; i++){
        if (!g_wheel_bits[i] || !fmt_fits((int)g_wheel_fmt[i], fmt)) continue;
        const int f16 = fmt16((int)g_wheel_fmt[i]);
        int same = 0;
        for (int t = 0; t < 64 * 64; t += 7) same += raw_at(g_mem + data, f16, (size_t)(t / 64) * 128u + (size_t)(t % 64)) == g_wheel_sig[i][t];
        const int seen = (64 * 64 + 6) / 7;
        if (same > best) best = same;
        if (same * 100 >= seen * 95){
            if (wheel_log()) fprintf(stderr, "[wheel] upload %08x = rim page %08x (%d of %d)\n", data, g_wheel_bits[i], same, seen);
            return 1;                   /* kept: the same car's page goes up again after a reload */
        }
    }
    if (wheel_log()) fprintf(stderr, "[wheel] upload %08x 128x128 fmt %d: no rim (best %d)\n", data, fmt, best);
    return 0;
}
/* The wheels' turn in one frame (radians, renderer uniform x1.x), per wheel of every car. The sim car (racer / car lists) holds four wheel
 * blocks at +7EC, step C4 (0, 1 front left / right, 2, 3 rear): +0 the hub's world position, +64 the wheel's drive (+) / brake (-) torque,
 * +70 its spin, always the ground speed x -15.5 (the physics never lets a wheel slip or lock). So what the eye expects is made here:
 * - a driven wheel (torque > 0) turns at least as fast as the engine says: rpm (+DB4) x the gear's ratio ([+760] + 190 + 4 x gear +D82)
 *   x k, k learned per car while the wheels grip (the largest ground / engine ratio seen, slowly forgotten): a launch spins the tyres;
 * - the hand brake (rear torque < -1 while the front ones brake not) stops the rear wheels.
 * Speed over a 0.33 m tyre, x 1/90 s. WHEELLOG=1 logs the match. */
#define WHEEL_SPIN_K 15.5f
static float g_wheel_k[CARS_MAX];
static float wheel_spin_of(uint32_t car, int slot, int w){
    float spin, tq, tqf, rpm, gr = 0.0f;
    const uint32_t b = car + 0x7ECu + 0xC4u * (uint32_t)w;
    memcpy(&spin, g_mem + b + 0x70, 4); memcpy(&tq, g_mem + b + 0x64, 4);
    memcpy(&tqf, g_mem + car + 0x7ECu + 0x64, 4);
    float v = fabsf(spin) / WHEEL_SPIN_K;
    if (!(v < 200.0f)) v = 0.0f;
    if (w >= 2 && tq < -1.0f && tqf > -0.3f) return 0.0f;        /* hand brake */
    memcpy(&rpm, g_mem + car + 0xDB4, 4);
    const uint32_t tr = MR32(car + 0x760u); const int g = g_mem[car + 0xD82u];
    if (tr && g >= 2 && g < 10) memcpy(&gr, g_mem + tr + 0x190u + 4u * (uint32_t)g, 4);
    if (gr > 0.0f && gr < 1.0f && rpm > 0.0f && rpm < 20000.0f){
        float *k = &g_wheel_k[slot];
        if (*k <= 0.0f) *k = 0.045f;
        const float e = rpm * gr;
        /* the coupled speed per (rpm x ratio): learnt slowly from the samples near it; a slipping wheel (r below) and a gear change
         * (the clutch open, rpm falling: r above) stay out */
        if (v > 6.0f && e > 1.0f){ const float r = v / e; if (r > *k * 0.8f && r < *k * 1.25f) *k = *k * 0.998f + r * 0.002f; }
        if (*k < 0.03f || *k > 0.08f) *k = 0.045f;
        /* wheel spin: the engine turns the driven wheels faster than the ground (10 % margin for the sim's own slip) */
        if (tq > 0.05f){ const float ve = e * *k; if (ve > v * 1.1f) v = ve; }
    }
    return v / 0.33f / 90.0f;
}
/* the render interpolation (pu_interp.c) draws a car's body between two simulation steps; the wheels and the beams go with the body
 * as drawn (0.5 m off at 100 km/h otherwise: rims found no wheel, the beams jumped at the step rate) */
int pu_interp_sim_xf(uint32_t position, float *pos, float *rot);
/* a car's front and rear axle middles in its own frame (x right, y up, z forward). The hubs (+7EC) trail the body by a step or two
 * (0.5 m at 70 km/h: the beams jumped at the step rate), so they are taken while the car stands (every car stands at the start) and
 * kept: geometry. Until then, from the hubs as they are. */
#define AXLE_SLOTS 64
static struct { uint32_t car; float l[2][3]; int still; } g_axle[AXLE_SLOTS];
static void axle_local(uint32_t car, float *front, float *rear){
    float sp[3], sr[9], vel[3];
    if (!pu_interp_sim_xf(car + 0x330u, sp, sr)){ memcpy(sp, g_mem + car + 0x330, 12); memcpy(sr, g_mem + car + 0x364, 36); }
    memcpy(vel, g_mem + car + 0x33C, 12);
    float l[2][3];
    for (int a = 0; a < 2; a++){
        const float *h0 = (const float *)(g_mem + car + 0x7ECu + 0xC4u * (uint32_t)(2 * a)), *h1 = (const float *)(g_mem + car + 0x7ECu + 0xC4u * (uint32_t)(2 * a + 1));
        const float d[3] = { 0.5f * (h0[0] + h1[0]) - sp[0], 0.5f * (h0[1] + h1[1]) - sp[1], 0.5f * (h0[2] + h1[2]) - sp[2] };
        for (int r = 0; r < 3; r++) l[a][r] = d[0] * sr[3 * r] + d[1] * sr[3 * r + 1] + d[2] * sr[3 * r + 2];
    }
    int slot = (int)((car >> 4) % AXLE_SLOTS);
    for (int t = 0; t < AXLE_SLOTS && g_axle[slot].car && g_axle[slot].car != car; t++) slot = (slot + 1) % AXLE_SLOTS;
    if (g_axle[slot].car != car){ g_axle[slot].car = car; g_axle[slot].still = 0; }
    const float sp2 = vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2];
    if (sp2 < 0.25f && fabsf(l[0][2] - l[1][2]) > 1.0f && fabsf(l[0][2] - l[1][2]) < 5.0f){
        memcpy(g_axle[slot].l, l, sizeof l); g_axle[slot].still = 1;
    }
    const float (*o)[3] = g_axle[slot].still ? g_axle[slot].l : (const float (*)[3])l;
    memcpy(front, o[0], 12); memcpy(rear, o[1], 12);
}
/* every car for a rim draw (r_web.c wheel_corners): 16 floats each = the drawn position (3), the drawn rows +364/+370/+37C (9), the
 * front and rear axles' forward offsets in the car's own frame, then nothing; and the four wheels' turns in spins[4 * car]. Count. */
int pu_wheel_cars(float *out, float *spins, int max){
    if (!wheel_on() || MR32(0x0065B928u)) return 0;
    int n = (int)MR32(CARS_N_CARS), k = 0; if (n > CARS_MAX) n = CARS_MAX;
    for (int i = 0; i < n && k < max; i++){
        const uint32_t car = MR32(CARS_L_CARS + 4u * (uint32_t)i);
        if (!car) continue;
        float *o = out + 16 * k;
        memcpy(o, g_mem + car + 0x330, 12); memcpy(o + 3, g_mem + car + 0x364, 36);
        for (int w = 0; w < 4; w++) spins[4 * k + w] = wheel_spin_of(car, i, w);
        float fr[3], re[3]; axle_local(car, fr, re);
        o[12] = fr[2]; o[13] = re[2]; o[14] = o[15] = 0.0f;
        if (wheel_log() && i == 0){ static int kk; if (++kk % 3000 == 0) fprintf(stderr, "[wheel] car 0 spins %.3f %.3f %.3f %.3f k %.4f axles %.2f %.2f\n",
                                                                          spins[0], spins[1], spins[2], spins[3], g_wheel_k[0], o[12], o[13]); }
        k++;
    }
    return k;
}

/* HEAD1: the player's car for the renderer's headlight beams (r_web.c head_uniform): the front axle's middle (the hubs of wheel blocks
 * 0 and 1), forward (+37C), up (+370), right = up x forward, all world; o[12] the night (1 / 0), o[13..15] left / right lamp, high
 * beams (car+D87). 0 in the menus or with no car.
 * HEADLIGHTS=0 off. */
int pu_head_frame(float *o){
    static int on = -1; if (on < 0){ const char *e = getenv("HEADLIGHTS"); on = !(e && *e == '0'); }
    if (!on || MR32(0x0065B928u)) return 0;
    const uint32_t n = MR32(CARS_N_RACERS), me = MR32(0x0065741Cu);
    if (!n || me >= n || me >= CARS_MAX) return 0;
    const uint32_t car = MR32(CARS_L_RACER + 4u * me);
    if (!car) return 0;
    float fr[3], re[3], f[3], up[3], dp[3], dr[9];
    axle_local(car, fr, re);
    memcpy(dp, g_mem + car + 0x330, 12); memcpy(dr, g_mem + car + 0x364, 36);   /* the body as this frame draws it */
    memcpy(up, g_mem + car + 0x370, 12); memcpy(f, g_mem + car + 0x37C, 12);
    for (int k = 0; k < 3; k++){ o[k] = dp[k] + fr[0] * dr[k] + fr[1] * dr[3 + k] + fr[2] * dr[6 + k]; o[3 + k] = f[k]; o[9 + k] = up[k]; }
    /* the world is left-handed (the camera's R = U x F): right = up x forward (= the car's row +364) */
    o[6] = up[1] * f[2] - up[2] * f[1]; o[7] = up[2] * f[0] - up[0] * f[2]; o[8] = up[0] * f[1] - up[1] * f[0];
    /* the night: the game's own flag (00628A28, the time variant of the track spec: its headlights on; the night Landstrasse 1, day
     * tracks 0). The sky's ambient tells no night from a shaded day track (night Landstrasse 0.09..0.68, day Normandie 0.18..0.43). */
    const float dark = MR32(0x00628A28u) ? 1.0f : 0.0f;
    static int lg = -1; if (lg < 0){ const char *e = getenv("HEADLOG"); lg = e && *e == '1'; }
    if (lg){ static int k; if (++k % 600 == 0) fprintf(stderr, "[head] night %d axle %.1f %.1f %.1f right %.2f %.2f %.2f\n",
                                                     (int)dark, o[0], o[1], o[2], o[6], o[7], o[8]); }
    o[12] = dark;
    /* the car's lamps as the game draws their glows: the car's R3DCar (car+74C) keeps one byte per lamp at [+2D4] (levels at [+2D8]),
     * set each frame by the lamp updater 00439360 from car+D87 and the damage; 0043D800 draws a lamp record's glow when its lamp's byte
     * is set: the headlight record with lamp 0 (the car's left), its mirrored copy with lamp 1 (the right). A broken headlight's byte
     * goes to 0 with its glow (front-left crushed: lamp 0 off, lamp 1 on). High beams: car+D87 bit 2 (action 15).
     * DMGPANEL=<i> (test): damage panel i (car+4E4 + 4 i; 0 front-left, 1 front, 2 front-right) at 100. */
    { static int tp = -2; if (tp == -2){ const char *e = getenv("DMGPANEL"); tp = e ? atoi(e) : -1; }
      if (tp >= 0 && tp < 9 && MR32(0x00606AC4u) > 200){ const float full = 100.0f; memcpy(g_mem + car + 0x4E4 + 4 * tp, &full, 4); } }
    const uint32_t r3d = MR32(car + 0x74Cu), lb = r3d ? MR32(r3d + 0x2D4u) : 0;
    if (!lb) return 0;
    o[13] = g_mem[lb] ? 1.0f : 0.0f; o[14] = g_mem[lb + 1] ? 1.0f : 0.0f;
    o[15] = (g_mem[car + 0xD87u] & 4) ? 1.0f : 0.0f;
    if (lg){ static int kp; if (++kp % 600 == 0){ float pd[3]; memcpy(pd, g_mem + car + 0x4E4, 12);
        fprintf(stderr, "[head] lamps %.0f %.0f high %.0f (D87 %02x) panels %.0f %.0f %.0f" "\n", o[13], o[14], o[15], g_mem[car + 0xD87u], pd[0], pd[1], pd[2]); } }
    /* the game's own headlights (LayerVertexLight@Headlights: track vertices lit per frame) give way to the beams: their apply method
     * (vtable slot 0 of the three Headlights layer classes) becomes the base layer's empty one, 0044A0A0 (mov eax,[ecx+8]; ret 8) */
    { static int patched; if (!patched){ patched = 1;
        static const uint32_t vt[3] = { 0x005B3B70u, 0x005B3B44u, 0x005B3B9Cu };
        for (int v = 0; v < 3; v++) if (MR32(vt[v]) == 0x00453110u || MR32(vt[v]) == 0x004535C0u) MR32(vt[v]) = 0x0044A0A0u; } }
    for (int k = 0; k < 16; k++) if (!(o[k] == o[k]) || fabsf(o[k]) > 1e6f) return 0;
    return 1;
}

/* HEAD2: the other cars as boxes for the beams' shadows (r_web.c head_boxes, renderer.ts headShadow), in the player's car frame of
 * pu_head_frame (x right, y up, z forward from the front axle's middle): 12 floats each = centre (3), half width; its right axis (3),
 * half height; its up axis (3), half length (forward = right x up in the shader; the slab test takes either sign). The box sits a
 * little inside the body (the hubs' height - 0.15 .. + 0.85, the axles -0.7 / +0.7, half width 0.78): the light grazes the body's
 * edges, and a lit car's own box never shades it (the shader skips a box that holds the lit point). Cars behind the lamps (-4 m), past
 * 120 m or out to the side of the beams left out. Count. */
int pu_head_boxes(float *out, int max){
    float h[16];
    if (!pu_head_frame(h)) return 0;
    const uint32_t me = MR32(CARS_L_RACER + 4u * MR32(0x0065741Cu));
    int n = (int)MR32(CARS_N_CARS), k = 0; if (n > CARS_MAX) n = CARS_MAX;
    for (int i = 0; i < n && k < max; i++){
        const uint32_t car = MR32(CARS_L_CARS + 4u * (uint32_t)i);
        if (!car || car == me) continue;
        float p[3], r[9], fr[3], re[3];
        memcpy(p, g_mem + car + 0x330, 12); memcpy(r, g_mem + car + 0x364, 36);   /* rows: right, up, forward */
        axle_local(car, fr, re);
        const float hub = 0.5f * (fr[1] + re[1]);
        const float y0 = hub - 0.15f, y1 = hub + 0.85f, z0 = re[2] - 0.7f, z1 = fr[2] + 0.7f;
        if (!(z1 - z0 > 1.5f && z1 - z0 < 8.0f)) continue;
        const float cl[3] = { 0.0f, 0.5f * (y0 + y1), 0.5f * (z0 + z1) };
        float cw[3];
        for (int a = 0; a < 3; a++) cw[a] = p[a] + r[a] * cl[0] + r[3 + a] * cl[1] + r[6 + a] * cl[2] - h[a];
        #define PL(v, o) do { (o)[0] = (v)[0] * h[6] + (v)[1] * h[7] + (v)[2] * h[8]; (o)[1] = (v)[0] * h[9] + (v)[1] * h[10] + (v)[2] * h[11]; \
                              (o)[2] = (v)[0] * h[3] + (v)[1] * h[4] + (v)[2] * h[5]; } while (0)
        float *o = out + 12 * k;
        PL(cw, o); PL(r, o + 4); PL(r + 3, o + 8);
        #undef PL
        if (o[2] < -4.0f || o[2] > 120.0f || fabsf(o[0]) > 0.9f * o[2] + 8.0f) continue;
        o[3] = 0.78f; o[7] = 0.5f * (y1 - y0); o[11] = 0.5f * (z1 - z0);
        int bad = 0; for (int a = 0; a < 12; a++) if (!(o[a] == o[a]) || fabsf(o[a]) > 1e5f) bad = 1;
        if (!bad) k++;
    }
    { static int lg = -1; if (lg < 0){ const char *e = getenv("HEADLOG"); lg = e && *e == '1'; }
      if (lg){ static int kk; if (++kk % 600 == 0){ fprintf(stderr, "[head] boxes %d", k);
          for (int j = 0; j < k && j < 3; j++) fprintf(stderr, "  (%.1f %.1f %.1f | %.2f %.2f %.2f)", out[12 * j], out[12 * j + 1], out[12 * j + 2], out[12 * j + 3], out[12 * j + 7], out[12 * j + 11]);
          fprintf(stderr, "\n"); } } }
    return k;
}

/* 00443F60, the page builder: thiscall(file, page, fmt) ret 0C */
void sub_00443f60(CPU *restrict c){
    const int outer = g_file;
    g_serial++;
    g_file = (int)MR32(c->esp + 4);
    if (glass_log_on()) fprintf(stderr, "[glass] builder: file %d page %08x fmt %x" "\n", g_file, MR32(c->esp + 8), MR32(c->esp + 12));
    sub_00443f60_orig(c);
    g_file = outer;
    g_serial++;
}
/* 00444130, the tinting blit: cdecl(page, image, tint, fmt, bands, x, y) */
void sub_00444130(CPU *restrict c){
    uint32_t a[7];
    for (int i = 0; i < 7; i++) a[i] = MR32(c->esp + 4 + 4u * (uint32_t)i);
    sub_00444130_orig(c);
    /* the glass pages: tpage5 "glass", tpage6 "intglass" (global, the tinted side windows). In the menus (4444 pages, 115C) file 3
     * is no window: it is the showroom floor's spotlight decal (alpha 68 with clear cut-outs, drawn from the platform to the
     * screen's bottom), and the glass pass made it shine like a mirror. File 4 there (the menus' intglass "glai") is the texture of
     * the garage's wall panes too (the same upload): the pass darkened them as the camera turned. The car shown in the menus has
     * its own 32-bit pages (fmt 20), which stay glass. */
    if ((g_file == 3 || g_file == 4) && a[3] != 0x115Cu && glass_on()){
        const uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx;
        glass_blit(c, a[0], a[1], a[2], a[3], (int)a[5], (int)a[6]);
        c->eax = eax; c->ecx = ecx; c->edx = edx;
        return;
    }
    if (g_file == 2 && wheel_on()){ wheel_blit(a[0], a[1], a[3], (int)a[5], (int)a[6]); return; }
    if (g_file != 0) return;            /* the exterior page only ([car_paint] files=1) */
    const uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx;
    record(c, a[0], a[1], a[2], a[3], a[4], (int)a[5], (int)a[6]);
    c->eax = eax; c->ecx = ecx; c->edx = edx;
}
static int paint_map_page(uint32_t data, int w, int h, int fmt, uint8_t *rg);
/* r_web.c r_tupdate: the map of an upload (w x h, two bytes a texel): 1 a paint page, 2 a glass page, 0 none */
int paint_map(uint32_t data, int w, int h, int fmt, uint8_t *rg){
    if (paint_map_page(data, w, h, fmt, rg)) return 1;
    if (wheel_map(data, w, h, fmt)){ for (size_t t = 0; t < (size_t)w * h; t++){ rg[t * 2] = 0; rg[t * 2 + 1] = 128; } return 4; }
    return glass_on() && glass_map(data, w, h, fmt, rg) ? 2 : 0;
}
static int paint_map_page(uint32_t data, int w, int h, int fmt, uint8_t *rg){
    if (fmt != 3 && fmt != 6 && fmt != 7) return 0;
    Page *page = NULL;
    for (int i = 0; i < PAGES && !page; i++){
        Page *p = &g_pages[i];
        if (p->band && !p->consumed && p->bits == data && p->stride == w && p->rows <= h && p->paint && fmt_fits(p->fmt, fmt)) page = p;
    }
    float best = page ? 1.0f : 0.0f;
    if (!page){
        for (int i = 0; i < PAGES; i++){        /* by content: the build whose paint texels match best */
            Page *p = &g_pages[i];
            if (!p->band || p->stride != w || p->rows > h || !p->paint || !fmt_fits(p->fmt, fmt)) continue;
            const int f16 = fmt16(p->fmt);
            const size_t n = (size_t)p->stride * (size_t)p->rows, step = n / 4096 + 1;
            uint32_t seen = 0, same = 0;
            for (size_t t = 0; t < n; t += step) if (p->band[t]){ seen++; same += raw_at(g_mem + data, f16, t) == p->expect[t]; }
            float r = seen ? (float)same / (float)seen : 0.0f;
            if (p->consumed) r -= 0.001f;
            if (r > best){ best = r; page = p; }
        }
        if (best < 0.5f){ if (getenv("PAINTLOG")) fprintf(stderr, "[paintlog] upload %08x %dx%d fmt %d: no paint map (best %.2f)\n", data, w, h, fmt, best); return 0; }
    }
    const int f16 = fmt16(page->fmt);
    const size_t n = (size_t)page->stride * (size_t)page->rows;
    static uint32_t hist[19][256];
    memset(hist, 0, sizeof hist);
    uint8_t *live = calloc(n, 1);
    uint32_t kept = 0;
    for (size_t t = 0; t < n; t++){
        if (!page->band[t] || raw_at(g_mem + data, f16, t) != page->expect[t]) continue;   /* drawn over later */
        live[t] = 1; kept++;
        if (page->red[t] >= 24) hist[page->band[t]][page->red[t]]++;
    }
    int peak[19];
    for (int b = 0; b < 19; b++){
        peak[b] = 255; uint32_t most = 0;
        for (int v = 24; v < 256; v++) if (hist[b][v] > most){ most = hist[b][v]; peak[b] = v; }
    }
    /* G: 128 neutral (full gloss, no damage; also off the paint, so a filtered edge reads nothing); 128..255 paint darker than
     * its band's peak (the seams: matte by the shader's dark power); 128..0 damage strength (darker and duller) */
    for (size_t t = 0; t < (size_t)w * h; t++){ rg[t * 2] = 0; rg[t * 2 + 1] = 128; }
    for (size_t t = 0; t < n; t++){
        if (!live[t]) continue;
        const int pk = peak[page->band[t]];
        const int keep = page->red[t] >= pk ? 255 : page->red[t] * 255 / pk;
        rg[t * 2] = 255; rg[t * 2 + 1] = (uint8_t)(128 + (255 - keep) / 2);
    }
    /* paint texels drawn over after the tint (the scrapes a damaged car's page is built with, numbers, plates) stay paint, matte
     * with their own colour: off the paint they took the game's sphere-map reflection pass (glinting like the glass) */
    uint32_t over = 0;
    /* the damage: its strength from how far the texel strays from the paint it covers (at least a little); the shader darkens
     * it by that and takes the gloss off as much (the stock look: scrapes darker and dull) */
    for (size_t t = 0; t < n; t++) if (page->band[t] && !live[t]){
        const uint32_t a = texel_argb(fmt, raw_at(g_mem + data, f16, t)), b = texel_argb(page->fmt, page->expect[t]);
        int dmax = 0;
        for (int k = 0; k < 24; k += 8){ const int dd = (int)((a >> k) & 255) - (int)((b >> k) & 255); if (dd > dmax) dmax = dd; if (-dd > dmax) dmax = -dd; }
        int s = 20 + dmax * 108 / 160; if (s > 128) s = 128;      /* 0..128: 0.15 .. 1 */
        rg[t * 2] = 255; rg[t * 2 + 1] = (uint8_t)(128 - s); over++;
    }
    /* untinted texels with paint all round (a 5 x 5 window at least 70 % live paint): the damage scrapes are drawn untinted, and
     * off the paint they took the game's sphere-map reflection pass (the sun's glint, as on the glass); glass, lamps and plates are
     * larger than that. They shade as matte paint with their own colour. */
    uint32_t absorbed = 0;
    {
        const int W = page->stride, H = page->rows;
        uint32_t *sum = calloc((size_t)(W + 1) * (H + 1), 4);
        if (sum){
            for (int y = 0; y < H; y++){
                uint32_t row = 0;
                for (int x = 0; x < W; x++){ row += live[(size_t)y * W + x]; sum[(size_t)(y + 1) * (W + 1) + x + 1] = sum[(size_t)y * (W + 1) + x + 1] + row; }
            }
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++){
                const size_t t = (size_t)y * W + x;
                if (live[t] || page->band[t]) continue;     /* the paint, and the damage marked above */
                const int x0 = x - 2 < 0 ? 0 : x - 2, y0 = y - 2 < 0 ? 0 : y - 2, x1 = x + 3 > W ? W : x + 3, y1 = y + 3 > H ? H : y + 3;
                const uint32_t cnt = sum[(size_t)y1 * (W + 1) + x1] - sum[(size_t)y0 * (W + 1) + x1] - sum[(size_t)y1 * (W + 1) + x0] + sum[(size_t)y0 * (W + 1) + x0];
                const uint32_t area = (uint32_t)((x1 - x0) * (y1 - y0));
                if (cnt * 10 >= area * 7){ rg[t * 2] = 255; rg[t * 2 + 1] = 128 - 77; absorbed++; }   /* damage 0.6 */
            }
            free(sum);
        }
    }
    free(live);
    page->consumed = 1;
    static int logs = 8;
    if (logs > 0 || getenv("PAINTLOG")){ logs--; fprintf(stderr, "[paint] page %dx%d fmt %d: %u paint texels, %u drawn over and %u untinted islands as matte paint (build match %.2f)\n", w, h, fmt, kept, over, absorbed, best); }
    return kept > 0;
}

/* ---- normals into the specular dword ---- */
float g_paint_sun[3] = { 0.0f, 1.0f, 0.0f };   /* world direction to the game's car light */
static uint32_t byte_of(float v){
    int b = (int)((v * 0.5f + 0.5f) * 254.0f + 1.5f);
    return (uint32_t)(b < 1 ? 1 : b > 255 ? 255 : b);
}
static int norm3(float *v){
    const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(l > 1e-12f) || !isfinite(l)) return 0;
    v[0] /= l; v[1] /= l; v[2] /= l;
    return 1;
}
static uint32_t dep_stream(uint32_t dep){
    if (!dep) return 0;
    const uint32_t material = MR32(dep + 0x20);
    const uint32_t table = material ? MR32(material + 8) : 0;
    return table ? MR32(table + MR32(dep + 0x1C) * 8u + 4u) : 0;
}
/* 00446530, the car light layer's apply: thiscall(streamA, streamB) ret 8 */
void sub_00446530(CPU *restrict c){
    const uint32_t layer = c->ecx;
    sub_00446530_orig(c);
    if (g_cam_capture || !layer) return;
    const uint32_t eagl = MR32(0x0069E0D4u), engine = MR32(0x00628130u);
    const uint32_t view = engine ? MR32(engine + 0x10) : 0;
    if (!eagl || !view) return;
    const int count = (int)MR32(eagl + 4), stride = (int)MR32(layer + 0x14);
    if (count <= 0 || count > 4096 || stride < 32 || stride > 64) return;
    const uint32_t deps = MR32(layer + 0x28), ndeps = MR32(layer + 0x2C);
    if (!deps || ndeps < 1) return;
    const uint32_t normals = dep_stream(MR32(deps));
    const uint32_t indices = ndeps >= 3 ? dep_stream(MR32(deps + 8)) : 0;
    const uint32_t ctx = MR32(eagl), model = ctx ? MR32(ctx + 0x60) : 0;
    if (!normals || !model) return;
    float m[16], R[3], U[3], F[3];
    memcpy(m, g_mem + model, 64);
    memcpy(R, g_mem + view + 0xE8, 12); memcpy(U, g_mem + view + 0xF4, 12); memcpy(F, g_mem + view + 0x100, 12);
    const uint32_t sky = MR32(0x006115CCu);
    if (sky){
        float L[3]; memcpy(L, g_mem + sky + 0x48, 12);
        if (norm3(L)){ g_paint_sun[0] = -L[0]; g_paint_sun[1] = -L[1]; g_paint_sun[2] = -L[2]; }
    }
    /* PERF2: the model's rotation and the camera's in one 3x3 (the camera's rows are orthonormal, so one normalisation after
     * both is the same as one after each); this loop ran over every car vertex a frame and cost more than the game's own
     * light layer (990 ms of a 15 s 4K race against 580) */
    float C[3][3];
    for (int i = 0; i < 3; i++){
        C[0][i] = m[4 * i] * R[0] + m[4 * i + 1] * R[1] + m[4 * i + 2] * R[2];
        C[1][i] = m[4 * i] * U[0] + m[4 * i + 1] * U[1] + m[4 * i + 2] * U[2];
        C[2][i] = -(m[4 * i] * F[0] + m[4 * i + 1] * F[1] + m[4 * i + 2] * F[2]);
    }
    const uint8_t *nb = g_mem + normals, *ib = indices ? g_mem + indices : NULL;
    uint8_t *out = g_mem + eagl + 0x448u;
    /* byte indices: at most 256 normals a batch, shared by several vertices (seams): each one's code once a call */
    uint32_t memo[256]; uint8_t have[256];
    if (ib) memset(have, 0, sizeof have);
    for (int j = 0; j < count; j++, out += stride){
        const uint32_t src = ib ? ib[j] : (uint32_t)j;
        if (ib && have[src]){ memcpy(out, &memo[src], 4); continue; }
        float n[3];
        memcpy(n, nb + src * 16u, 12);
        const float c0 = C[0][0] * n[0] + C[0][1] * n[1] + C[0][2] * n[2];
        const float c1 = C[1][0] * n[0] + C[1][1] * n[1] + C[1][2] * n[2];
        const float c2 = C[2][0] * n[0] + C[2][1] * n[1] + C[2][2] * n[2];
        const float l2 = c0 * c0 + c1 * c1 + c2 * c2;
        uint32_t code = 0;
        if (l2 > 1e-24f && l2 < 3.4e38f){
            const float k = 1.0f / sqrtf(l2);
            code = byte_of(c0 * k) | byte_of(c1 * k) << 8 | byte_of(c2 * k) << 16;
        }
        memcpy(out, &code, 4);
        if (ib){ memo[src] = code; have[src] = 1; }
    }
}
#else
#include "cpu.h"
#include "decls.h"
/* native builds: the game's own functions */
void sub_00443f60(CPU *restrict c){ sub_00443f60_orig(c); }
void sub_00444130(CPU *restrict c){ sub_00444130_orig(c); }
void sub_00446530(CPU *restrict c){ sub_00446530_orig(c); }
int pu_wheel_cars(float *out, float *spins, int max){ (void)out; (void)spins; (void)max; return 0; }
int pu_head_frame(float *o){ (void)o; return 0; }
int pu_head_boxes(float *o, int max){ (void)o; (void)max; return 0; }
#endif
