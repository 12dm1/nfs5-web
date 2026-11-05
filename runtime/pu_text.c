/* Sharper text (PC enhanced/subsystems/text/text_quality.cpp, NE1). The fonts are 4-bit glyph sheets made for 640x480: each sheet is enlarged
 * 2x or 4x by the window's height, Catmull-Rom over the palette index with each ramp steepened around its middle, and drawn in place of the
 * game's tile whenever the text is magnified enough. One texture set per font colour, the least recently used of 8 is dropped.
 * ?env=TEXT=0 keeps the original sheets. */
#ifdef __wasm__
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "decls.h"

uint32_t r_talloc(int w, int h, int fmt, int flags, int mips);
void r_tfree(uint32_t t);
void r_settexture(uint32_t t);
void r_tupload_argb(uint32_t t, const uint32_t *argb);
float r_display_factor(void);
int r_window_mode(int *w, int *h);

#define MAX_TILES 10
#define MAX_SETS 8
#define MAX_SHEETS 32
#define BORDER 2
#define LEVEL_SCALE 17.0f
#define MAX_INDEX 15.0f
#define FONT_DRIVER_TABLE 0x005DFB60u
#define CURRENT_FONT_TEXTURE 0x006A642Cu
#define DISPLAY_BPP 0x005DEAD0u

typedef struct { uint32_t palette[16], handles[MAX_TILES]; uint64_t last_use; } ColorSet;
typedef struct {
    uint32_t texture, pixels, checksum, palette[16];
    int width, height, tile_size, tile_count, tops[MAX_TILES], segments, have_palette, failed, nsets;
    uint8_t *levels[MAX_TILES];
    ColorSet sets[MAX_SETS];
} Sheet;

static Sheet g_sheets[MAX_SHEETS];
static int g_on = -1, g_upscale;
static float g_edge_slope, g_mag;
static uint64_t g_use;

static int enabled(void){
    if (g_on < 0){ const char *e = getenv("TEXT"); g_on = !(e && *e == '0'); }
    return g_on;
}
/* the factor comes from the window, which the page reports after the first fonts are loaded; a resize keeps the first answer */
static int upscale_ready(void){
    if (g_upscale) return g_upscale >= 2;
    int w = 0, h = 0;
    if (!r_window_mode(&w, &h)) return 0;
    const float ratio = (float)h / 480.0f;
    g_upscale = ratio >= 3.0f ? 4 : ratio >= 1.5f ? 2 : 1;
    const float slope = 0.6f * (float)g_upscale;
    g_edge_slope = slope < 1.0f ? 1.0f : slope;
    return g_upscale >= 2;
}
static Sheet *find_sheet(uint32_t texture, int create){
    Sheet *fresh = NULL;
    for (int i = 0; i < MAX_SHEETS; i++){
        if (g_sheets[i].texture == texture) return &g_sheets[i];
        if (!g_sheets[i].texture && !fresh) fresh = &g_sheets[i];
    }
    if (create && fresh) fresh->texture = texture;
    return create ? fresh : NULL;
}
static void free_set(ColorSet *s, int free_handles){
    for (int i = 0; i < MAX_TILES; i++){
        if (s->handles[i] && free_handles) r_tfree(s->handles[i]);
        s->handles[i] = 0;
    }
}
static void release_sets(Sheet *s, int free_handles){
    for (int i = 0; i < s->nsets; i++) free_set(&s->sets[i], free_handles);
    s->nsets = 0;
}
static void drop_levels(Sheet *s){
    for (int i = 0; i < MAX_TILES; i++){ free(s->levels[i]); s->levels[i] = NULL; }
}
static void drop_sheet(Sheet *s){
    release_sets(s, 1);
    drop_levels(s);
    memset(s, 0, sizeof *s);
}
/* r_web.c: the driver dropped all textures (free_handles 0), or the mode changes */
void pu_text_reset(int free_handles){
    for (int i = 0; i < MAX_SHEETS; i++) if (g_sheets[i].texture){ release_sets(&g_sheets[i], free_handles); g_sheets[i].failed = 0; }
}
static void catmull(float t, float *w){
    const float t2 = t * t, t3 = t2 * t;
    w[0] = (-t3 + 2.0f * t2 - t) * 0.5f;
    w[1] = (3.0f * t3 - 5.0f * t2 + 2.0f) * 0.5f;
    w[2] = (-3.0f * t3 + 4.0f * t2 + t) * 0.5f;
    w[3] = (t3 - t2) * 0.5f;
}
static uint32_t lerp_color(uint32_t a, uint32_t b, float t){
    uint32_t r = 0;
    for (int shift = 0; shift < 32; shift += 8){
        const float x = (float)((a >> shift) & 255u), y = (float)((b >> shift) & 255u);
        long c = lroundf(x + (y - x) * t);
        r |= (uint32_t)(c > 255 ? 255 : c) << shift;
    }
    return r;
}
static int build_levels(Sheet *s){
    const int factor = g_upscale, tile = s->tile_size, size = tile * factor, padded = tile + 2 * BORDER;
    const int row_bytes = (s->width + 1) / 2;
    float weights[4][4];
    int first_tap[4];
    for (int o = 0; o < factor; o++){
        const float pos = ((float)o + 0.5f) / (float)factor - 0.5f, base = floorf(pos);
        catmull(pos - base, weights[o]);
        first_tap[o] = (int)base - 1;
    }
    const float seg_width = MAX_INDEX / (float)s->segments, last_seg = (float)(s->segments - 1);
    float *source = malloc((size_t)padded * padded * sizeof(float));
    float *hor = malloc((size_t)padded * size * sizeof(float));
    if (!source || !hor){ free(source); free(hor); return 0; }
    int ok = 1;
    for (int t = 0; t < s->tile_count && ok; t++){
        for (int y = 0; y < padded; y++)
            for (int x = 0; x < padded; x++){
                const int sy = s->tops[t] + y - BORDER, sx = x - BORDER;
                float v = 0.0f;
                if (sx >= 0 && sx < s->width && sy >= 0 && sy < s->height){
                    const uint8_t packed = g_mem[s->pixels + (uint32_t)sy * (uint32_t)row_bytes + (uint32_t)(sx / 2)];
                    v = (float)((sx & 1) ? (packed & 15) : (packed >> 4));   /* the high nibble is the left pixel */
                }
                source[y * padded + x] = v;
            }
        for (int y = 0; y < padded; y++){
            const float *row = source + y * padded;
            float *out = hor + y * size;
            for (int x = 0; x < tile; x++)
                for (int o = 0; o < factor; o++){
                    const int tap = x + first_tap[o] + BORDER;
                    const float *w = weights[o];
                    out[x * factor + o] = row[tap] * w[0] + row[tap + 1] * w[1] + row[tap + 2] * w[2] + row[tap + 3] * w[3];
                }
        }
        uint8_t *levels = malloc((size_t)size * size);
        if (!levels){ ok = 0; break; }
        s->levels[t] = levels;
        for (int y = 0; y < tile; y++)
            for (int o = 0; o < factor; o++){
                const float *w = weights[o];
                const float *r0 = hor + (y + first_tap[o] + BORDER) * size, *r1 = r0 + size, *r2 = r1 + size, *r3 = r2 + size;
                uint8_t *out = levels + (y * factor + o) * size;
                for (int x = 0; x < size; x++){
                    float index = r0[x] * w[0] + r1[x] * w[1] + r2[x] * w[2] + r3[x] * w[3];
                    index = index < 0.0f ? 0.0f : index > MAX_INDEX ? MAX_INDEX : index;
                    /* each ramp segment steepened around its middle: the filtered antialiasing levels become a narrow smooth edge again */
                    float seg = floorf(index / seg_width);
                    if (seg > last_seg) seg = last_seg;
                    float sharp = ((index - seg * seg_width) / seg_width - 0.5f) * g_edge_slope + 0.5f;
                    sharp = sharp < 0.0f ? 0.0f : sharp > 1.0f ? 1.0f : sharp;
                    out[x] = (uint8_t)lroundf((seg + sharp) * seg_width * LEVEL_SCALE);
                }
            }
    }
    free(source); free(hor);
    return ok;
}
static int upload_set(Sheet *s, ColorSet *set){
    uint32_t lut[256];
    for (int level = 0; level < 256; level++){
        const float index = (float)level / LEVEL_SCALE;
        int low = (int)index; if (low > 14) low = 14;
        float t = index - (float)low; if (t > 1.0f) t = 1.0f;
        lut[level] = lerp_color(set->palette[low], set->palette[low + 1], t);
    }
    /* fully clear levels take the colour of the first visible one: what alpha dilation would give them */
    int visible = 0;
    while (visible < 256 && !(lut[visible] >> 24)) visible++;
    if (visible < 256) for (int level = 0; level < visible; level++) lut[level] = (lut[level] & 0xFF000000u) | (lut[visible] & 0x00FFFFFFu);
    const int size = s->tile_size * g_upscale, n = size * size;
    uint32_t *px = malloc((size_t)n * 4);
    if (!px) return 0;
    int ok = 1;
    for (int t = 0; t < s->tile_count; t++){
        const uint32_t h = r_talloc(size, size, 6, 0, 0);
        if (!h){ ok = 0; break; }
        set->handles[t] = h;
        const uint8_t *levels = s->levels[t];
        for (int i = 0; i < n; i++) px[i] = lut[levels[i]];
        r_tupload_argb(h, px);
    }
    free(px);
    return ok;
}
static ColorSet *find_or_create_set(Sheet *s){
    for (int i = 0; i < s->nsets; i++)
        if (!memcmp(s->sets[i].palette, s->palette, sizeof s->palette)){ s->sets[i].last_use = ++g_use; return &s->sets[i]; }
    if (!s->levels[0] && !build_levels(s)){ drop_levels(s); s->failed = 1; return NULL; }
    if (s->nsets >= MAX_SETS){
        int oldest = 0;
        for (int i = 1; i < s->nsets; i++) if (s->sets[i].last_use < s->sets[oldest].last_use) oldest = i;
        free_set(&s->sets[oldest], 1);
        s->sets[oldest] = s->sets[--s->nsets];
    }
    ColorSet *set = &s->sets[s->nsets++];
    memset(set, 0, sizeof *set);
    memcpy(set->palette, s->palette, sizeof s->palette);
    set->last_use = ++g_use;
    if (!upload_set(s, set)){
        free_set(set, 1);
        s->nsets--;
        s->failed = 1;
        return NULL;
    }
    return set;
}
/* 00571A10: rebuilds a font's palette and uploads its glyph sheet; cdecl(font) */
void sub_00571a10(CPU *restrict c){
    const uint32_t font = MR32(c->esp + 4);
    sub_00571a10_orig(c);
    if (!enabled() || !font || MR32(font + 0xE8) != FONT_DRIVER_TABLE) return;
    const uint32_t tex = MR32(font + 0x44), header = MR32(font + 0x24), shape = MR32(font + 0x2C);
    if (!tex || !header || !shape) return;
    const uint32_t flags = MR32(header + 0x0C);
    const int w = (int)MR32(tex), h = (int)MR32(tex + 4), fmt = (int)MR32(tex + 8), tile = (int)MR32(tex + 0x10), count = (int)MR32(tex + 0x14);
    /* multi colour outline palettes have no ordered index ramp to interpolate along */
    if (fmt != 1 || count <= 0 || count > MAX_TILES || tile < 8 || tile > 512 || w <= 0 || w > tile || h <= 0 || (flags & 2)){
        Sheet *s = find_sheet(tex, 0);
        if (s) drop_sheet(s);
        return;
    }
    const uint32_t pixels = shape + 16, bytes = (uint32_t)((w + 1) / 2) * (uint32_t)h;
    uint32_t sum = 2166136261u;
    for (uint32_t i = 0; i < bytes; i++) sum = (sum ^ g_mem[pixels + i]) * 16777619u;
    Sheet *s = find_sheet(tex, 1);
    if (!s) return;
    if (s->pixels != pixels || s->width != w || s->height != h || s->tile_size != tile || s->tile_count != count || s->checksum != sum){
        release_sets(s, 1);
        drop_levels(s);
        s->pixels = pixels; s->width = w; s->height = h; s->tile_size = tile; s->tile_count = count; s->checksum = sum;
        for (int i = 0; i < count; i++) s->tops[i] = (int)MR32(tex + 0x18 + 8u * (uint32_t)i);
        s->segments = (flags & 4) ? 2 : 1;
        s->failed = 0;
    }
    memcpy(s->palette, g_mem + font + 0x6C, sizeof s->palette);
    s->have_palette = 1;
}
/* 005722C0: draws glyph quads of the selected font texture, UVs still in texels; cdecl(quads, vertices, indices) */
void sub_005722c0(CPU *restrict c){
    const int quads = (int)MR32(c->esp + 4);
    const uint32_t v = MR32(c->esp + 8);
    float mag = 0.0f;
    if (quads > 0 && v){
        float x0, x1, u0, u1;
        memcpy(&x0, g_mem + v, 4); memcpy(&x1, g_mem + v + 32, 4);
        memcpy(&u0, g_mem + v + 24, 4); memcpy(&u1, g_mem + v + 32 + 24, 4);
        const float du = fabsf(u1 - u0);
        if (du > 0.0f) mag = fabsf(x1 - x0) / du;
    }
    g_mag = mag;
    sub_005722c0_orig(c);
    g_mag = 0.0f;
}
/* 00572230: frees a font texture object; cdecl(texture) */
void sub_00572230(CPU *restrict c){
    const uint32_t tex = MR32(c->esp + 4);
    if (tex){ Sheet *s = find_sheet(tex, 0); if (s) drop_sheet(s); }
    sub_00572230_orig(c);
}
/* at 005725A5, after the tile's dtexture call inside 005722C0: its argument is still on the stack */
void pu_text_tile(CPU *c){
    if (g_mag <= 0.0f || !enabled() || !upscale_ready()) return;
    const uint32_t texture = MR32(c->esp);
    if (!texture || (int)MR32(DISPLAY_BPP) <= 8) return;
    /* text drawn near its original size keeps the original sheet: a large one would only be minified */
    if (g_mag * r_display_factor() < (float)g_upscale * 0.5f) return;
    const uint32_t tex = MR32(CURRENT_FONT_TEXTURE);
    Sheet *s = tex ? find_sheet(tex, 0) : NULL;
    if (!s || !s->have_palette || s->failed) return;
    int tile = -1;
    for (int i = 0; i < s->tile_count && tile < 0; i++) if (MR32(tex + 0x68 + 4u * (uint32_t)i) == texture) tile = i;
    if (tile < 0) return;
    const ColorSet *set = find_or_create_set(s);
    if (set && set->handles[tile]) r_settexture(set->handles[tile]);
}
#else
#include "cpu.h"
#include "decls.h"
void sub_00571a10(CPU *restrict c){ sub_00571a10_orig(c); }
void sub_005722c0(CPU *restrict c){ sub_005722c0_orig(c); }
void sub_00572230(CPU *restrict c){ sub_00572230_orig(c); }
#endif
