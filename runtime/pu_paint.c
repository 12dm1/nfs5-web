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
/* 00443F60, the page builder: thiscall(file, page, fmt) ret 0C */
void sub_00443f60(CPU *restrict c){
    const int outer = g_file;
    g_serial++;
    g_file = (int)MR32(c->esp + 4);
    sub_00443f60_orig(c);
    g_file = outer;
    g_serial++;
}
/* 00444130, the tinting blit: cdecl(page, image, tint, fmt, bands, x, y) */
void sub_00444130(CPU *restrict c){
    uint32_t a[7];
    for (int i = 0; i < 7; i++) a[i] = MR32(c->esp + 4 + 4u * (uint32_t)i);
    sub_00444130_orig(c);
    if (g_file != 0) return;            /* the exterior page only ([car_paint] files=1) */
    const uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx;
    record(c, a[0], a[1], a[2], a[3], a[4], (int)a[5], (int)a[6]);
    c->eax = eax; c->ecx = ecx; c->edx = edx;
}
/* r_web.c r_tupdate: the paint map of an upload (w x h, two bytes a texel) or 0 */
int paint_map(uint32_t data, int w, int h, int fmt, uint8_t *rg){
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
    memset(rg, 0, (size_t)w * h * 2);
    for (size_t t = 0; t < n; t++){
        if (!live[t]) continue;
        const int pk = peak[page->band[t]];
        rg[t * 2] = 255; rg[t * 2 + 1] = (uint8_t)(page->red[t] >= pk ? 255 : page->red[t] * 255 / pk);
    }
    free(live);
    page->consumed = 1;
    static int logs = 8;
    if (logs > 0 || getenv("PAINTLOG")){ logs--; fprintf(stderr, "[paint] page %dx%d fmt %d: %u paint texels (build match %.2f)\n", w, h, fmt, kept, best); }
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
    for (int j = 0; j < count; j++){
        const uint32_t src = indices ? g_mem[indices + (uint32_t)j] : (uint32_t)j;
        float n[3], w[3], cam[3];
        memcpy(n, g_mem + normals + src * 16u, 12);
        for (int k = 0; k < 3; k++) w[k] = n[0] * m[k] + n[1] * m[4 + k] + n[2] * m[8 + k];
        uint32_t code = 0;
        if (norm3(w)){
            cam[0] = w[0] * R[0] + w[1] * R[1] + w[2] * R[2];
            cam[1] = w[0] * U[0] + w[1] * U[1] + w[2] * U[2];
            cam[2] = -(w[0] * F[0] + w[1] * F[1] + w[2] * F[2]);
            if (norm3(cam)) code = byte_of(cam[0]) | byte_of(cam[1]) << 8 | byte_of(cam[2]) << 16;
        }
        MR32(eagl + 0x448u + (uint32_t)(j * stride)) = code;
    }
}
#else
#include "cpu.h"
#include "decls.h"
/* native builds: the game's own functions */
void sub_00443f60(CPU *restrict c){ sub_00443f60_orig(c); }
void sub_00444130(CPU *restrict c){ sub_00444130_orig(c); }
void sub_00446530(CPU *restrict c){ sub_00446530_orig(c); }
#endif
