#include "dx11_texture.h"
#include "dx11_device.h"
#include "dx11_state.h"
#include "materials.h"
#include "tex_anim.h"
#include "car_paint.h"
#include "log.h"
#include <cstring>
#include <vector>
#include <algorithm>

static const int MAX_TEXTURES = 4096;
static TexSlot g_texSlots[MAX_TEXTURES];
static void* g_boundTexture = nullptr;
static bool g_texMips = true;   // [display] texture_mips

static ID3D11Texture2D*          g_whiteTex = nullptr;
static ID3D11ShaderResourceView* g_whiteSRV = nullptr;

void Tex_Configure(bool mips) { g_texMips = mips; }

static void CreateWhiteTexture() {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    uint32_t white = 0xFFFFFFFF;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = &white;
    init.SysMemPitch = 4;

    g_dx.device->CreateTexture2D(&desc, &init, &g_whiteTex);
    g_dx.device->CreateShaderResourceView(g_whiteTex, nullptr, &g_whiteSRV);
}

void Tex_Init() {
    memset(g_texSlots, 0, sizeof(g_texSlots));
    g_boundTexture = nullptr;
    CreateWhiteTexture();
}

void Tex_Shutdown() {
    Tex_Reset();
    if (g_whiteSRV) { g_whiteSRV->Release(); g_whiteSRV = nullptr; }
    if (g_whiteTex) { g_whiteTex->Release(); g_whiteTex = nullptr; }
}

void Tex_Reset() {
    CarPaint_OnResetAll();   // CP1
    for (int i = 0; i < MAX_TEXTURES; i++) {
        if (g_texSlots[i].used) {
            if (g_texSlots[i].srv) g_texSlots[i].srv->Release();
            if (g_texSlots[i].tex) g_texSlots[i].tex->Release();
            g_texSlots[i] = {};
        }
    }
    g_boundTexture = nullptr;
}

void* Tex_Alloc(int width, int height, int format, int flags, int mipLevels) {
    int slot = -1;
    for (int i = 0; i < MAX_TEXTURES; i++) {
        if (!g_texSlots[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        Log("Tex_Alloc: out of texture slots");
        return nullptr;
    }

    // The game always allocates mip=0 and uploads level 0 only (the DTEX pool
    // never carries mip data). A full chain is still required. GPU GenerateMips
    // averages transparent-black holes into foliage RGB and draws a dark collar;
    // Tex_Update builds the lower levels with alpha-weighted RGB instead.
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = g_texMips ? 0 : 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (g_texMips) {
        desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    }

    HRESULT hr = g_dx.device->CreateTexture2D(&desc, nullptr, &g_texSlots[slot].tex);
    if (FAILED(hr)) {
        Log("Tex_Alloc: CreateTexture2D failed (%dx%d fmt=%d) hr=0x%08X", width, height, format, hr);
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC created = {};
    g_texSlots[slot].tex->GetDesc(&created);

    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = desc.Format;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MostDetailedMip = 0;
    sv.Texture2D.MipLevels = (UINT)-1;   // every level
    hr = g_dx.device->CreateShaderResourceView(g_texSlots[slot].tex, &sv, &g_texSlots[slot].srv);
    if (FAILED(hr)) {
        g_texSlots[slot].tex->Release();
        g_texSlots[slot].tex = nullptr;
        Log("Tex_Alloc: CreateSRV failed hr=0x%08X", hr);
        return nullptr;
    }

    g_texSlots[slot].width  = width;
    g_texSlots[slot].height = height;
    g_texSlots[slot].format = format;
    g_texSlots[slot].mipLevels = (int)created.MipLevels;
    g_texSlots[slot].used   = true;
    g_texSlots[slot].cutout = false;
    g_texSlots[slot].keyed = false;

    Log("Tex_Alloc slot=%d %dx%d fmt=%d game_mip=%d levels=%u",
        slot, width, height, format, mipLevels, created.MipLevels);
    return (void*)(intptr_t)(slot + 1);
}

void Tex_Free(void* handle) {
    int slot = (int)(intptr_t)handle - 1;
    if (slot < 0 || slot >= MAX_TEXTURES || !g_texSlots[slot].used) return;
    TexAnim_OnFree(slot);   // HZ4: its extra animation frames go with it
    CarPaint_OnFree(slot);  // CP1

    if (g_texSlots[slot].srv) g_texSlots[slot].srv->Release();
    if (g_texSlots[slot].tex) g_texSlots[slot].tex->Release();
    g_texSlots[slot] = {};

    if (g_boundTexture == handle) g_boundTexture = nullptr;
}

// Convert THRASH texture formats to RGBA8888 for DX11.
// Format numbering is the game's own table at 0x5DF15C ({alpha,r,g,b,bpp} per
// format) and its FSH-code mapping FUN_00556690:
//   fmt 2 → P8 (FSH 0x7B, palette)      fmt 3 → ARGB1555 (0x7E)   fmt 4 → RGB565 (0x78)
//   fmt 5 → RGB888 24-bit (0x7F)        fmt 6 → ARGB8888 (0x7D)   fmt 7 → ARGB4444 (0x6D)
//   fmt 8 → AI44 (8-bit)                fmt 9 → AP88 (16-bit)     fmt 10/11 → 565/555
// Pixels arrive as the game packs them: little-endian words, i.e. ARGB8888 is
// B,G,R,A in memory and RGB888 is B,G,R.
// Output is ABGR byte order (R in low byte) matching DXGI_FORMAT_R8G8B8A8_UNORM.
static void ConvertToRGBA(const void* src, uint32_t* dst, int w, int h, int fmt) {
    int count = w * h;
    switch (fmt) {
    case 3:  // ARGB1555 (Glide 0x0B, confirmed from dx7z.dll mapping)
    case 11: // RGB555 (no alpha bit)
    case 0:  // legacy aliases kept from the first mapping attempt
    case 10:
        for (int i = 0; i < count; i++) {
            uint16_t c = ((const uint16_t*)src)[i];
            uint32_t a = (fmt == 11) ? 0xFF : ((c & 0x8000) ? 0xFF : 0x00);
            uint32_t r = ((c >> 10) & 0x1F) * 255 / 31;
            uint32_t g = ((c >> 5) & 0x1F) * 255 / 31;
            uint32_t b = (c & 0x1F) * 255 / 31;
            dst[i] = (a << 24) | (b << 16) | (g << 8) | r;
        }
        break;
    case 4:  // RGB565 (Glide 0x0A, confirmed: pitch=1280 for w=640)
    case 1:
    case 9:  // legacy aliases
        for (int i = 0; i < count; i++) {
            uint16_t c = ((const uint16_t*)src)[i];
            uint32_t r = ((c >> 11) & 0x1F) * 255 / 31;
            uint32_t g = ((c >> 5) & 0x3F) * 255 / 63;
            uint32_t b = (c & 0x1F) * 255 / 31;
            dst[i] = 0xFF000000 | (b << 16) | (g << 8) | r;
        }
        break;
    case 7: // ARGB4444 (Glide 0x0C, confirmed: alpha gradient in hex data)
    case 2:
        for (int i = 0; i < count; i++) {
            uint16_t c = ((const uint16_t*)src)[i];
            uint32_t a = ((c >> 12) & 0xF) * 17;
            uint32_t r = ((c >> 8) & 0xF) * 17;
            uint32_t g = ((c >> 4) & 0xF) * 17;
            uint32_t b = (c & 0xF) * 17;
            dst[i] = (a << 24) | (b << 16) | (g << 8) | r;
        }
        break;
    case 8: // ALPHA_INTENSITY_44 (Glide 0x04: 4-bit alpha + 4-bit intensity)
        for (int i = 0; i < count; i++) {
            uint8_t c = ((const uint8_t*)src)[i];
            uint32_t a = ((c >> 4) & 0xF) * 17;
            uint32_t l = (c & 0xF) * 17;
            dst[i] = (a << 24) | (l << 16) | (l << 8) | l;
        }
        break;
    case 5: // RGB888, 3 bytes per pixel (B,G,R), opaque
        for (int i = 0; i < count; i++) {
            const uint8_t* p = (const uint8_t*)src + i * 3;
            dst[i] = 0xFF000000 | ((uint32_t)p[2]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[0] << 16);
        }
        break;
    case 6: // ARGB8888 (little-endian dword: B,G,R,A in memory)
    default:
        for (int i = 0; i < count; i++) {
            uint32_t c = ((const uint32_t*)src)[i];
            dst[i] = (c & 0xFF00FF00) | ((c & 0x00FF0000) >> 16) | ((c & 0x000000FF) << 16);
        }
        break;
    }
}

struct AlphaHistogram { int clear, mid, solid; };

static bool ClassifyBinaryCutout(const uint32_t* px, int n, AlphaHistogram* out) {
    // Foliage/fences are colour-keyed: A=0 holes and A>=128 leaves, almost
    // no mid-alpha. Glass and sprites keep a gradient and stay on 10/255.
    int clear = 0, solid = 0, mid = 0;
    for (int i = 0; i < n; ++i) {
        const unsigned a = px[i] >> 24;
        if (a < 8) ++clear;
        else if (a >= 128) ++solid;
        else ++mid;
    }
    if (out) { out->clear = clear; out->mid = mid; out->solid = solid; }
    if (!clear || !solid) return false;
    if (mid * 20 > n) return false;     // >5% mid-alpha
    if (clear * 12 < n) return false;   // <~8% holes
    return true;
}

// Opaque (A >= 8) texels on one edge of the outer texel ring:
// 0 top row, 1 bottom row, 2 left column, 3 right column.
static int EdgeOpaque(const uint32_t* px, int w, int h, int edge) {
    int n = 0;
    if (edge < 2) {
        const uint32_t* row = px + (edge ? (h - 1) * w : 0);
        for (int x = 0; x < w; ++x) n += (row[x] >> 24) >= 8;
    } else {
        const int x = edge == 2 ? 0 : w - 1;
        for (int y = 0; y < h; ++y) n += (px[y * w + x] >> 24) >= 8;
    }
    return n;
}

// NFS4 quads UV to exact 0/1. A single opaque texel on that row becomes
// a sky hairline once anisotropy clamps and stretches it, so an edge that
// is transparent apart from a few stray texels (< 1/8 opaque) is cleared.
// An edge that carries content is left alone: the horizon panorama atlas
// (horz/hor0..3, ~80% opaque columns, whose panels meet at U = 0 and 1)
// and tiling cutouts (barriers, U = 0..16) otherwise get a see-through slit
// at every panel/tile junction -- the alps/canyon sky "gap" (SKY1,
// docs/findings/SKY1-horizon-slit.md). The four characters of `decision`
// record which edges were cleared (T/B/L/R or -) for the HS11 log line.
static void ClearCutoutBorder(uint32_t* px, int w, int h, char decision[5]) {
    memcpy(decision, "----", 5);
    if (w < 4 || h < 4) return;
    const int top = EdgeOpaque(px, w, h, 0), bottom = EdgeOpaque(px, w, h, 1);
    const int left = EdgeOpaque(px, w, h, 2), right = EdgeOpaque(px, w, h, 3);
    if (top * 8 < w) {
        for (int x = 0; x < w; ++x) px[x] &= 0x00FFFFFFu;
        decision[0] = 'T';
    }
    if (bottom * 8 < w) {
        for (int x = 0; x < w; ++x) px[(h - 1) * w + x] &= 0x00FFFFFFu;
        decision[1] = 'B';
    }
    if (left * 8 < h) {
        for (int y = 0; y < h; ++y) px[y * w] &= 0x00FFFFFFu;
        decision[2] = 'L';
    }
    if (right * 8 < h) {
        for (int y = 0; y < h; ++y) px[y * w + (w - 1)] &= 0x00FFFFFFu;
        decision[3] = 'R';
    }
}

static void DilateRgbIntoAlpha(uint32_t* px, int w, int h) {
    // Flood nearest opaque RGB into A=0 holes so bilinear does not mix in
    // colour-key black or leftover sky. Alpha is unchanged.
    const int n = w * h;
    bool anyClear = false, anySolid = false;
    for (int i = 0; i < n; ++i) {
        if (px[i] >> 24) anySolid = true;
        else anyClear = true;
        if (anyClear && anySolid) break;
    }
    if (!anyClear || !anySolid) return;
    std::vector<uint8_t> has((size_t)n);
    for (int i = 0; i < n; ++i) has[i] = (uint8_t)((px[i] >> 24) != 0);
    const int dx[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
    const int dy[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
    // Each pass fills the clear texels that touch a filled one with the average of those
    // neighbours as they were before the pass. Only that frontier is visited (NE1: a sparse
    // 1024x1024 glyph sheet took 4.3 s with a full scan per pass); same result.
    std::vector<uint8_t> queued((size_t)n);
    std::vector<int> frontier, next;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int i = y * w + x;
            if (has[i]) continue;
            for (int k = 0; k < 8; ++k) {
                const int xx = x + dx[k], yy = y + dy[k];
                if (xx >= 0 && yy >= 0 && xx < w && yy < h && has[yy * w + xx]) {
                    frontier.push_back(i);
                    queued[i] = 1;
                    break;
                }
            }
        }
    }
    std::vector<uint32_t> value;
    const int limit = std::max(w, h);
    for (int pass = 0; pass < limit && !frontier.empty(); ++pass) {
        value.resize(frontier.size());
        for (size_t f = 0; f < frontier.size(); ++f) {
            const int i = frontier[f], x = i % w, y = i / w;
            unsigned accR = 0, accG = 0, accB = 0, count = 0;
            for (int k = 0; k < 8; ++k) {
                const int xx = x + dx[k], yy = y + dy[k];
                if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                const int j = yy * w + xx;
                if (!has[j]) continue;
                const uint32_t p = px[j];
                accR += p & 0xFFu;
                accG += (p >> 8) & 0xFFu;
                accB += (p >> 16) & 0xFFu;
                ++count;
            }
            value[f] = (px[i] & 0xFF000000u)
                | ((accB / count) << 16) | ((accG / count) << 8) | (accR / count);
        }
        for (size_t f = 0; f < frontier.size(); ++f) {
            px[frontier[f]] = value[f];
            has[frontier[f]] = 1;
        }
        next.clear();
        for (const int i : frontier) {
            const int x = i % w, y = i / w;
            for (int k = 0; k < 8; ++k) {
                const int xx = x + dx[k], yy = y + dy[k];
                if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                const int j = yy * w + xx;
                if (has[j] || queued[j]) continue;
                queued[j] = 1;
                next.push_back(j);
            }
        }
        frontier.swap(next);
    }
}

static void DownsampleAlphaWeighted(const uint32_t* src, int sw, int sh,
                                    uint32_t* dst, int dw, int dh) {
    // RGB weighted by alpha so A=0 colour-key black does not darken foliage.
    // Alpha is a plain box average, matching nfs5_fsh.downsample. A 2x2
    // block with no alpha at all keeps its plain RGB average instead of
    // black, so the lower mips of a keyed bitmap gain no dark holes for
    // bilinear to drag into the leaf edges.
    for (int y = 0; y < dh; ++y) {
        const int y0 = (2 * y) * sw;
        const int y1 = (sh > 1) ? (2 * y + 1) * sw : y0;
        for (int x = 0; x < dw; ++x) {
            const int x0 = 2 * x;
            const int x1 = (sw > 1) ? x0 + 1 : x0;
            unsigned accR = 0, accG = 0, accB = 0, accA = 0, weight = 0;
            unsigned sumR = 0, sumG = 0, sumB = 0;
            const int idx[4] = { y0 + x0, y0 + x1, y1 + x0, y1 + x1 };
            for (int i = 0; i < 4; ++i) {
                const uint32_t p = src[idx[i]];
                const unsigned r = p & 0xFFu;
                const unsigned g = (p >> 8) & 0xFFu;
                const unsigned b = (p >> 16) & 0xFFu;
                const unsigned a = (p >> 24) & 0xFFu;
                accR += r * a;
                accG += g * a;
                accB += b * a;
                sumR += r; sumG += g; sumB += b;
                accA += a;
                weight += a;
            }
            const unsigned r = weight ? accR / weight : sumR / 4u;
            const unsigned g = weight ? accG / weight : sumG / 4u;
            const unsigned b = weight ? accB / weight : sumB / 4u;
            dst[y * dw + x] = (accA / 4u) << 24 | b << 16 | g << 8 | r;
        }
    }
}

static void UploadMipChain(int slot, uint32_t* mip0) {
    int w = g_texSlots[slot].width;
    int h = g_texSlots[slot].height;
    const int levels = g_texSlots[slot].mipLevels;
    g_dx.context->UpdateSubresource(g_texSlots[slot].tex, 0, nullptr, mip0, w * 4, 0);
    if (levels <= 1) return;
    std::vector<uint32_t> src(mip0, mip0 + (size_t)w * h);
    int sw = w, sh = h;
    for (int level = 1; level < levels; ++level) {
        const int dw = std::max(1, sw / 2);
        const int dh = std::max(1, sh / 2);
        std::vector<uint32_t> dst((size_t)dw * dh);
        DownsampleAlphaWeighted(src.data(), sw, sh, dst.data(), dw, dh);
        g_dx.context->UpdateSubresource(g_texSlots[slot].tex, (UINT)level, nullptr,
                                        dst.data(), dw * 4, 0);
        src.swap(dst);
        sw = dw;
        sh = dh;
    }
}

static void RegenerateMips(int slot) {
    if (g_texSlots[slot].mipLevels > 1)
        g_dx.context->GenerateMips(g_texSlots[slot].srv);
}

static void ProcessAndUpload(int slot, uint32_t* rgba, bool game);

bool g_generatedUpload = false;
bool g_texStreamingUpdate = false;

bool Tex_NoteUpdate(int slot) {
    static LARGE_INTEGER s_last[MAX_TEXTURES];
    static LARGE_INTEGER s_freq;
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    bool streaming = false;
    if (slot >= 0 && slot < MAX_TEXTURES) {
        streaming = s_last[slot].QuadPart && now.QuadPart - s_last[slot].QuadPart < s_freq.QuadPart / 10;
        s_last[slot] = now;
    }
    g_texStreamingUpdate = streaming;
    return streaming;
}
extern "C" void Tex_SetGeneratedUpload(int on) { g_generatedUpload = on != 0; }

void Tex_Update(void* handle, int palette, void* data) {
    int slot = (int)(intptr_t)handle - 1;
    if (slot < 0 || slot >= MAX_TEXTURES || !g_texSlots[slot].used) {
        Log("Tex_Update: invalid slot %d", slot);
        return;
    }
    if (!data) {
        Log("Tex_Update: null data");
        return;
    }
    if (palette) {
        // Only reachable if a palettised format were advertised.
        static int s_palWarn = 0;
        if (s_palWarn++ < 5)
            Log("Tex_Update slot=%d fmt=%d: palette pointer %p ignored",
                slot, g_texSlots[slot].format, (void*)(intptr_t)palette);
    }

    int w = g_texSlots[slot].width;
    int h = g_texSlots[slot].height;
    int fmt = g_texSlots[slot].format;

    static int s_dumpCount = 0;
    if (s_dumpCount < 50) {
        const uint8_t* p = (const uint8_t*)data;
        int bppGuess = 0;
        if (w >= 4) {
            const uint16_t* p16 = (const uint16_t*)data;
            const uint32_t* p32 = (const uint32_t*)data;
            int match16 = 0, match32 = 0;
            for (int i = 1; i < w && i < 16; i++) {
                if (p16[i] == p16[0]) match16++;
                if (p32[i] == p32[0]) match32++;
            }
            if (match16 > match32) bppGuess = 16;
            else if (match32 > 0) bppGuess = 32;
        }
        Log("Tex_Update slot=%d %dx%d fmt=%d levels=%d bppGuess=%d data: %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X",
            slot, w, h, fmt, g_texSlots[slot].mipLevels, bppGuess,
            p[0],p[1], p[2],p[3], p[4],p[5], p[6],p[7],
            p[8],p[9], p[10],p[11], p[12],p[13], p[14],p[15]);
        s_dumpCount++;
    }

    uint32_t* rgba = new uint32_t[w * h];
    ConvertToRGBA(data, rgba, w, h, fmt);
    Materials_OnTexUpdate(slot, rgba, w, h, fmt);
    // CP1: a car page the paint hook composed gets its paint map.
    CarPaint_OnUpload(slot, data, w, h, fmt, Materials_SlotName(slot));
    ProcessAndUpload(slot, rgba, true);
    delete[] rgba;
    // HZ4: an NFS4 animated texture brings its other frames along.
    TexAnim_OnUpload(slot, Materials_SlotName(slot), w, h);
}

void* Tex_CreateInternal(int w, int h, const void* bgra) {
    void* handle = Tex_Alloc(w, h, 6, 0, 0);
    if (!handle) return nullptr;
    const int slot = (int)(intptr_t)handle - 1;
    uint32_t* rgba = new uint32_t[w * h];
    ConvertToRGBA(bgra, rgba, w, h, 6);
    ProcessAndUpload(slot, rgba, false);
    delete[] rgba;
    return handle;
}

// Everything Tex_Update does to a level-0 image after the material hook:
// alpha dilation, cutout classification and border clearing, mip chain.
static void ProcessAndUpload(int slot, uint32_t* rgba, bool game) {
    const int w = g_texSlots[slot].width;
    const int h = g_texSlots[slot].height;
    const int fmt = g_texSlots[slot].format;
    if (!g_generatedUpload) DilateRgbIntoAlpha(rgba, w, h);
    AlphaHistogram hist = {};
    bool cutout = ClassifyBinaryCutout(rgba, w * h, &hist);
    // MAT5: pack's cutout flag overrides auto-detection when HD albedo is active.
    // (Material slots are keyed by game uploads; our own frames have none.)
    int cutoverride = game ? Materials_CutoutOverride(slot) : 0;
    if (cutoverride > 0) cutout = true;
    else if (cutoverride < 0) cutout = false;
    const int n = w * h;
    // HZ6: >= 1% holes, <= 1% mid alpha (NFS4 bitmaps are snapped to 0/255).
    // Car pages (CAR*) keep the strict rule: their alpha is the paint zone.
    const char* name = game ? Materials_SlotName(slot) : "";
    const bool carPage = name[0] == 'C' && name[1] == 'A' && name[2] == 'R';
    const bool keyed = cutout || (cutoverride >= 0 && !carPage && hist.solid &&
                                  hist.clear * 100 >= n && hist.mid * 100 <= n);
    const bool changed = cutout != g_texSlots[slot].cutout || keyed != g_texSlots[slot].keyed;
    g_texSlots[slot].cutout = cutout;
    g_texSlots[slot].keyed = keyed;
    if (changed && (void*)(intptr_t)(slot + 1) == g_boundTexture)
        State_RefreshCutoutPolicy();
    char cleared[5] = "----";
    if (keyed) ClearCutoutBorder(rgba, w, h, cleared);
    // SKY1 diagnostic: every cutout classification with its alpha histogram,
    // the opaque texel count of each outer edge and which edges were cleared.
    static int s_cutoutLogs = 0;
    if (keyed && s_cutoutLogs < 160) {
        ++s_cutoutLogs;
        Log("HS11 tex cutout=%d slot=%d %dx%d fmt=%d clear=%d%% mid=%d%% solid=%d%% "
            "edge opaque T/B/L/R=%d/%d/%d/%d cleared=%s",
            cutout ? 1 : 2, slot, w, h, fmt,
            hist.clear * 100 / n, hist.mid * 100 / n, hist.solid * 100 / n,
            EdgeOpaque(rgba, w, h, 0), EdgeOpaque(rgba, w, h, 1),
            EdgeOpaque(rgba, w, h, 2), EdgeOpaque(rgba, w, h, 3), cleared);
    } else if (changed && !cutout && s_cutoutLogs < 160) {
        ++s_cutoutLogs;
        Log("HS11 tex cutout=0 slot=%d %dx%d fmt=%d", slot, w, h, fmt);
    }
    UploadMipChain(slot, rgba);
}

void Tex_UpdateRect(void* handle, int mipLevel, int x, int y, int rw, int rh,
                    int pitch, void* data, int fmt) {
    int slot = (int)(intptr_t)handle - 1;
    if (slot < 0 || slot >= MAX_TEXTURES || !g_texSlots[slot].used || !data) return;

    int texW = g_texSlots[slot].width;
    int texH = g_texSlots[slot].height;
    int srcFmt = (fmt >= 0) ? fmt : g_texSlots[slot].format;
    if (mipLevel < 0 || mipLevel >= g_texSlots[slot].mipLevels) return;
    if (mipLevel == 0) CarPaint_OnUpdateRect(slot, x, y, rw, rh);   // CP1 diagnostics

    for (int m = 0; m < mipLevel; m++) {
        texW = (texW > 1) ? texW / 2 : 1;
        texH = (texH > 1) ? texH / 2 : 1;
    }

    if (x + rw > texW) rw = texW - x;
    if (y + rh > texH) rh = texH - y;
    if (rw <= 0 || rh <= 0) return;

    uint32_t* rgba = new uint32_t[rw * rh];
    const uint8_t* srcBytes = (const uint8_t*)data;

    for (int row = 0; row < rh; row++) {
        const void* rowSrc = srcBytes + row * pitch;
        ConvertToRGBA(rowSrc, rgba + row * rw, rw, 1, srcFmt);
    }

    D3D11_BOX box = {};
    box.left = x;
    box.top = y;
    box.right = x + rw;
    box.bottom = y + rh;
    box.front = 0;
    box.back = 1;

    g_dx.context->UpdateSubresource(
        g_texSlots[slot].tex, mipLevel, &box,
        rgba, rw * 4, 0);

    delete[] rgba;
    if (mipLevel == 0) RegenerateMips(slot);
}

// Bind texture and switch pixel shader accordingly.
// When handle is NULL or invalid, use white 1x1 texture + vertex-color-only shader (PSMainNoTex).
// When valid, use the texture shader (PSMain) which does tex * vertex_color.
// NOTE: state 0xA (grTexCombine/grColorCombine/grAlphaCombine) is NOT yet implemented here.
// It was attempted but broke all textures (99% of rendering uses values 0/2 which set TMU passthrough —
// naive implementation switched to flat shader, making everything gray). Needs careful re-analysis.
void* Tex_Bound() { return g_boundTexture; }

bool Tex_BoundIsCutout() {
    const int slot = (int)(intptr_t)g_boundTexture - 1;
    if (slot < 0 || slot >= MAX_TEXTURES || !g_texSlots[slot].used) return false;
    return g_texSlots[slot].cutout;
}

bool Tex_BoundIsKeyed() {
    const int slot = (int)(intptr_t)g_boundTexture - 1;
    if (slot < 0 || slot >= MAX_TEXTURES || !g_texSlots[slot].used) return false;
    return g_texSlots[slot].keyed;
}

int Tex_BoundSlot() {
    int slot = (int)(intptr_t)g_boundTexture - 1;
    if (slot < 0 || slot >= MAX_TEXTURES || !g_texSlots[slot].used) return -1;
    return slot;
}

void Tex_Stats(int* count, double* megabytes) {
    int n = 0;
    double bytes = 0.0;
    for (const TexSlot& t : g_texSlots) {
        if (!t.used) continue;
        ++n;
        bytes += (double)t.width * t.height * 4.0 * (t.mipLevels > 1 ? 4.0 / 3.0 : 1.0);
    }
    *count = n;
    *megabytes = bytes / (1024.0 * 1024.0);
}

const TexSlot* Tex_SlotAt(int slot) {
    if (slot < 0 || slot >= MAX_TEXTURES || !g_texSlots[slot].used) return nullptr;
    return &g_texSlots[slot];
}

ID3D11ShaderResourceView* Tex_SrvForHandle(void* handle) {
    const int slot = (int)(intptr_t)handle - 1;
    if (slot < 0 || slot >= MAX_TEXTURES || !g_texSlots[slot].used) return nullptr;
    return g_texSlots[slot].srv;
}

void Tex_Bind(void* handle) {
    g_boundTexture = handle;

    if (!handle) {
        g_dx.context->PSSetShaderResources(0, 1, &g_whiteSRV);
        g_dx.context->PSSetShader(g_dx.pixelShaderFlat, nullptr, 0);
        g_dx.boundPS = g_dx.pixelShaderFlat;
        State_RefreshCutoutPolicy();
        return;
    }

    // HZ4: an animated NFS4 texture shows its current frame.
    handle = TexAnim_Substitute(handle);
    g_boundTexture = handle;
    int slot = (int)(intptr_t)handle - 1;
    if (slot < 0 || slot >= MAX_TEXTURES || !g_texSlots[slot].used) {
        g_dx.context->PSSetShaderResources(0, 1, &g_whiteSRV);
        g_dx.context->PSSetShader(g_dx.pixelShaderFlat, nullptr, 0);
        g_dx.boundPS = g_dx.pixelShaderFlat;
        State_RefreshCutoutPolicy();
        return;
    }

    // MAT5: use HD albedo SRV when the pack provides one for this slot.
    ID3D11ShaderResourceView* srv = Materials_HdAlbedoSrv(slot);
    if (!srv) srv = g_texSlots[slot].srv;
    g_dx.context->PSSetShaderResources(0, 1, &srv);
    g_dx.context->PSSetShader(g_dx.pixelShaderTex, nullptr, 0);
    g_dx.boundPS = g_dx.pixelShaderTex;
    State_RefreshCutoutPolicy();
}
