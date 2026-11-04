#include "dx11_state.h"
#include "motion_blur.h"
#include "dx11_texture.h"
#include "dx11_device.h"
#include "log.h"
#include "thrash_api.h"
#include <unordered_map>
#include <vector>
#include <cstring>
#include <cmath>

static std::unordered_map<int, int> g_states;

int g_vertexStride = 32;
int g_indexStride = 4;

// ---- G1: Glide texture sampling state (dx7z setstate table @ 60002C7C) ----
// 0x07 grTexFilterMode (0 point / 1 bilinear), 0x08 grTexLodBiasValue (float),
// 0x41 / 0x42 grTexClampMode S / T (0 CLAMP, 1 WRAP, 2 MIRROR). One D3D11
// sampler per distinct combination, created on demand.
struct SamplerKey {
    int filter = 1;          // 0 point, 1 bilinear
    int lodBiasBits = 0;     // float bits
    int clampS = 1, clampT = 1;   // 0 clamp, 1 wrap, 2 mirror
    int mips = 1;            // 0 = mip 0 only (frontend atlases), 1 = full chain
    bool operator==(const SamplerKey& o) const {
        return filter == o.filter && lodBiasBits == o.lodBiasBits &&
               clampS == o.clampS && clampT == o.clampT && mips == o.mips;
    }
};
static SamplerKey g_samplerKey;
static std::vector<std::pair<SamplerKey, ID3D11SamplerState*>> g_samplerCache;
static int g_aniso = 16;
static bool g_honorLodBias = true;

static D3D11_TEXTURE_ADDRESS_MODE AddressMode(int clamp) {
    switch (clamp) {
    case 0:  return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 2:  return D3D11_TEXTURE_ADDRESS_MIRROR;
    default: return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

static int g_pointFilterMode = 2;   // 0 never, 1 always, 2 race only (32-bit video mode)
static bool g_raceVideoMode = false;
static bool g_cutoutWrapOverride = false;
static bool g_hardSuppress = false;        // HZ6: per-draw, see FlushVerts
static void ApplyBlend();
static void RefreshCutoutPolicy();

static void ApplySampler() {
    if (!g_dx.context || !g_dx.device) return;
    // G7: the game asks for point sampling for its 2D art. Its 640-designed
    // art is magnified on a 1920x1080 logical screen: in the race HUD that
    // gives the crisp digits the user wants, in the frontend it turns every
    // glyph into blocks. point_filter=2 honours it only while a 32-bit
    // (race) video mode is set (THRASH_setvideomode); 1 = always, 0 = never
    // (bilinear everywhere, the pre-G1 look).
    SamplerKey key = g_samplerKey;
    // Clamp the entire anisotropic footprint at every mip level, not only
    // the pixel center at mip 0. Preserve the game's requested wrap state.
    if (g_dx.hardCutoutActive && !g_cutoutWrapOverride) key.clampS = key.clampT = 0;
    if (key.filter == 0) {
        const bool honour = g_pointFilterMode == 1 || (g_pointFilterMode == 2 && g_raceVideoMode);
        if (!honour) key.filter = 1;
    }
    // G10: frontend atlas pages are often shown below their texel size and
    // the game adds a +0.25 LOD bias; mip 1 already averages neighbouring
    // sub-images together. Mip 0 only outside the race.
    if (!g_raceVideoMode) key.mips = 0;
    ID3D11SamplerState* sampler = nullptr;
    for (auto& e : g_samplerCache)
        if (e.first == key) { sampler = e.second; break; }
    if (!sampler) {
        float bias = 0.0f;
        if (g_honorLodBias) memcpy(&bias, &key.lodBiasBits, sizeof(bias));
        if (!(bias > -16.0f && bias < 16.0f)) bias = 0.0f;
        D3D11_SAMPLER_DESC sd = {};
        if (key.filter == 0)
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        else
            sd.Filter = (g_aniso >= 2) ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.MaxAnisotropy = (key.filter != 0 && g_aniso >= 2) ? (UINT)g_aniso : 1;
        sd.AddressU = AddressMode(key.clampS);
        sd.AddressV = AddressMode(key.clampT);
        sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MipLODBias = key.mips ? bias : 0.0f;
        sd.MinLOD = 0.0f;
        sd.MaxLOD = key.mips ? D3D11_FLOAT32_MAX : 0.0f;
        if (FAILED(g_dx.device->CreateSamplerState(&sd, &sampler)) || !sampler) return;
        g_samplerCache.push_back({key, sampler});
        Log("G1 sampler #%u: filter=%d lodBias=%.2f clampS=%d clampT=%d",
            (unsigned)g_samplerCache.size(), key.filter, bias, key.clampS, key.clampT);
    }
    DX11_SetSampler0(g_dx, sampler);
}

// ---- G5: THRASH_clip (scissor) is game state too ----
// The game caches its clip rectangle like every other state. The R1 capture
// runs mid-frame (right after the mirror pass), its faces set the clip to
// full screen, and PipelineSnapshot::Restore() put the pre-capture scissor
// (the 840,0,1080,84 mirror rectangle) back while the game believed the clip
// was full screen: the next frame's sky dome was scissored away and the clear
// colour (state 3, 0xFF8A627D) showed as a flat sky. Track the rectangle here
// and re-apply it from State_Reapply.
static int g_clip[4] = {0, 0, 0, 0};
static bool g_clipValid = false;

static void ApplyClip() {
    if (!g_dx.context || !g_clipValid) return;
    int lw = g_dx.logicalW > 0 ? g_dx.logicalW : 1;
    int lh = g_dx.logicalH > 0 ? g_dx.logicalH : 1;
    int gw = DX11_TargetWidth(g_dx) > 0 ? DX11_TargetWidth(g_dx) : lw;
    int gh = DX11_TargetHeight(g_dx) > 0 ? DX11_TargetHeight(g_dx) : lh;
    D3D11_RECT r;
    r.left   = (LONG)((long long)g_clip[0] * gw / lw);
    r.top    = (LONG)((long long)g_clip[1] * gh / lh);
    r.right  = (LONG)((long long)g_clip[2] * gw / lw);
    r.bottom = (LONG)((long long)g_clip[3] * gh / lh);
    g_dx.context->RSSetScissorRects(1, &r);
}

void State_SetClip(int x1, int y1, int x2, int y2) {
    g_clip[0] = x1; g_clip[1] = y1; g_clip[2] = x2; g_clip[3] = y2;
    g_clipValid = true;
    ApplyClip();
}

// ---- G1v3: state 2 = face culling ----
// dx7z stores state 2 in [6000C7A8] (60006270: 0 -> 1 "off", 2 -> 0, 1 ->
// 0x80000000) and culls in software in every triangle loop (60004437..):
// A = (y1-y0)(x2-x0) - (x1-x0)(y2-y0); the triangle is skipped when
// signbit(A) == flag. Mapping verified in the user's race (the first guess,
// 2 -> CULL_BACK, rendered the world inside-out): 2 -> CULL_FRONT,
// 1 -> CULL_BACK, 0 -> CULL_NONE (D3D FrontCounterClockwise = FALSE after the
// wrapper's y flip). Road signs draw both faces coplanar and rely on this.
static ID3D11RasterizerState* g_cullStates[3] = {};
static bool g_honorCull = true;
static void ApplyDepthState();

static ID3D11RasterizerState* CullState(int mode) {
    if (mode < 0 || mode > 2 || !g_honorCull) mode = 0;
    if (mode == 0) return g_dx.rasterState;
    if (!g_cullStates[mode] && g_dx.device) {
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = (mode == 2) ? D3D11_CULL_FRONT : D3D11_CULL_BACK;
        rd.FrontCounterClockwise = FALSE;
        rd.ScissorEnable = TRUE;
        rd.DepthClipEnable = FALSE;
        if (FAILED(g_dx.device->CreateRasterizerState(&rd, &g_cullStates[mode])))
            g_cullStates[mode] = nullptr;
    }
    return g_cullStates[mode] ? g_cullStates[mode] : g_dx.rasterState;
}

// The mode is read from the state map so that State_SetDirect(2, ...) (the R1
// capture restoring the game's backend cull) is honored by State_Reapply too.
static void ApplyCull() {
    if (!g_dx.context) return;
    auto it = g_states.find(THRASH_STATE_CULL_MODE);
    ID3D11RasterizerState* rs = CullState(it != g_states.end() ? it->second : 0);
    if (rs) g_dx.context->RSSetState(rs);
}

// ---- G1: 0x18 grDepthBiasLevel ----
// The race runs the Glide W-buffer (state 4 = 2), a 16-bit float with a 12-bit
// mantissa: a bias level is one mantissa LSB, i.e. a RELATIVE w offset of
// level / 4096. The game sends levels from -1..-10 (decals, shadows) down to
// -10000 for far objects. Applied in the vertex shader as
// z += level / divisor * (1 - z); the rasterizer DepthBias (constant in
// 24-bit units) cannot express that.
static int g_depthBiasDivisor = 4096;
static int g_depthBiasLevel = 0;

static void ApplyDepthBias(int level) {
    if (level < -1000000 || level > 1000000) level = 0;
    g_depthBiasLevel = level;
    g_dx.depthBias = (float)level / (float)(g_depthBiasDivisor > 0 ? g_depthBiasDivisor : 4096);
    DX11_MarkScreenCBDirty(g_dx);
}

// Glide fog table (dx7z guFogGenerateExp @ 600039D0): entries normalised so the
// last one, at w63 = 2^18 / 5 (guFogTableIndexToW), reaches 1.0.
static void UpdateFogScale() {
    const float w63 = 262144.0f / 5.0f;
    const float denom = 1.0f - expf(-g_dx.fogDensity * w63);
    g_dx.fogScale = (denom > 1e-6f) ? 1.0f / denom : 1.0f;
}

void State_ConfigureSampling(int aniso, bool honorLodBias, int depthBiasDivisor) {
    g_aniso = aniso;
    g_honorLodBias = honorLodBias;
    g_depthBiasDivisor = depthBiasDivisor;
}

void State_ConfigurePointFilter(int mode) { g_pointFilterMode = mode; }
void State_SetRaceVideoMode(bool race) { g_raceVideoMode = race; RefreshCutoutPolicy(); ApplySampler(); }
bool State_RaceVideoMode() { return g_raceVideoMode; }

// G1v3: after an extra scene traversal (R1 cubemap capture, R0 probe) the
// wrapper restores the GPU pipeline it captured beforehand, but the game's own
// state cache keeps whatever the traversal set last (it only re-sends changes).
// Any state the traversal changed then stays wrong on the GPU until the game
// happens to change it again: an additive glow as the last capture draw left
// the whole next frame additive (HUD included), a depth state left cars
// see-through. Push the wrapper's tracked copy of the game's state instead.
// AA1: the tracked blend mode, with alpha-to-coverage for alpha-tested
// draws in the plain SrcAlpha/InvSrcAlpha mode on an MSAA target. The game
// uses two kinds of alpha test (setstate 0x24 immediates in the image): 10/255
// on cars/sprites/particles, only to skip fully transparent texels, and
// 0x80..0xA0 for cutouts (the deferred queue at 004666AF: foliage, fences,
// poles). Only the latter get coverage from alpha; the pixel shader then
// sharpens the alpha at the threshold instead of discarding (emissive.y).
// AA1: alpha-test references at or above this get alpha-to-coverage.
static const float kA2CMinAlphaRef = 0.25f;
static bool AlphaToCoverageWanted(int blend) {
    return g_dx.msaaSamples > 1 && g_dx.alphaToCoverage && blend == 0 &&
           g_dx.alphaRef >= kA2CMinAlphaRef && g_dx.blendModesA2C[blend] != nullptr;
}
static bool HardCutoutWanted(int blend) {
    // Deferred world queue uses references 0x80..0xA0. Cars (10/255),
    // additive particles, frontend and depth-free HUD retain their behavior.
    if (!g_raceVideoMode || blend != 0 || g_dx.depthComparison == 7)
        return false;
    // HZ6: NFS4 cutout materials also come through the sorted queue (10/255,
    // no depth write): a keyed bitmap there blended its bilinear edge (a
    // light rim) and wrapped onto the opposite edge. FlushVerts suppresses
    // this for a draw with vertex alpha (a fading object keeps its blend).
    if (!g_dx.depthWrite)
        return !g_hardSuppress && g_dx.alphaRef >= 10.0f/255.0f && Tex_BoundIsKeyed();
    // Only bitmaps classified as binary cutouts by Tex_Update take the hard
    // path (sampler CLAMP + the shader's 1.5-texel inset); a gradient-alpha
    // texture drawn at the world references keeps the plain alpha test.
    // HZ6: keyed = binary alpha with >= 1% holes. NFS4 hedges (Atlantica
    // 0072, 4.6% holes along one edge) missed the 8% cutout rule and kept
    // the WRAP seam along the quad edge and a rim at the 10/255 test.
    if (g_dx.alphaRef >= 128.0f/255.0f && g_dx.alphaRef <= 160.0f/255.0f)
        return Tex_BoundIsKeyed();
    // FakeSort / prim queue 1 is flushed at 00466A90 with leftover 10/255.
    // The 0xA0 pass is a different list (004669C0). Binary-keyed track
    // bitmaps still need the world-cutout discard.
    return g_dx.alphaRef >= 10.0f/255.0f && Tex_BoundIsKeyed();
}
static void ApplyBlend() {
    if (!g_dx.context) return;
    auto it = g_states.find(THRASH_STATE_BLEND_MODE);
    const int blend = it != g_states.end() ? it->second : 0;
    if (blend < 0 || blend > 3) return;
    const bool hard = HardCutoutWanted(blend);
    const bool a2c = !hard && AlphaToCoverageWanted(blend);
    ID3D11BlendState* state = a2c ? g_dx.blendModesA2C[blend] : g_dx.blendModes[blend];
    // MAT14d/MAT16: inside a cubemap capture the alpha channel holds the sun:
    // the additive sun / lens-flare sprite draws (bracketed by the MAT16
    // hooks) add their luminance to it, everything else writes RGB only.
    bool sunMask = false;
    if (g_dx.captureAlphaMask) {
        ID3D11BlendState* rgb = a2c ? g_dx.blendModesA2CRgb[blend] : g_dx.blendModesRgb[blend];
        ID3D11BlendState* clearA = a2c ? g_dx.blendModesA2CClearA[blend] : g_dx.blendModesClearA[blend];
        if (blend == 1 && g_dx.sunSpriteDraw && g_dx.blendSunMask) { state = g_dx.blendSunMask; sunMask = true; }
        else if (blend == 0 && g_dx.captureClearAlpha && clearA) state = clearA;   // CAR2 fix 7: occluders clear the sun mask
        else if (rgb) state = rgb;
    }
    if (sunMask != g_dx.sunMaskDraw) { g_dx.sunMaskDraw = sunMask; DX11_MarkScreenCBDirty(g_dx); }
    if (!state) return;
    DX11_SetBlend(g_dx, state);
    if (hard != g_dx.hardCutoutActive) {
        g_dx.hardCutoutActive = hard;
        DX11_MarkScreenCBDirty(g_dx);
        ApplySampler();
    }
    if (a2c != g_dx.a2cActive) {
        g_dx.a2cActive = a2c;
        DX11_MarkScreenCBDirty(g_dx);
    }
}
void State_ApplyBlend() { ApplyBlend(); }

static void RefreshCutoutPolicy() {
    const int blend = State_Peek(THRASH_STATE_BLEND_MODE);
    if (blend < 0 || blend > 3) return;
    const bool hard = HardCutoutWanted(blend);
    const bool a2c = !hard && AlphaToCoverageWanted(blend);
    // Auxiliary passes may temporarily bind their own blend state. A depth
    // or alpha update must not overwrite it unless our policy actually changes.
    if (hard != g_dx.hardCutoutActive || a2c != g_dx.a2cActive) ApplyBlend();
}

void State_RefreshCutoutPolicy() { RefreshCutoutPolicy(); }

// bar5 tiling cutouts (dx11_draw.cpp FlushVerts): a hard-cutout draw whose
// UVs leave [0,1] needs the game's WRAP state for that one draw, so the
// sampler CLAMP override in ApplySampler is skipped while `skip` is set.
void State_OverrideCutoutClamp(bool skip) {
    if (g_cutoutWrapOverride != skip) {
        g_cutoutWrapOverride = skip;
        ApplySampler();
    }
}

// HZ6: FlushVerts turns the sorted keyed-cutout path off for one draw whose
// vertices carry alpha.
void State_SuppressHardCutout(bool on) {
    if (g_hardSuppress != on) {
        g_hardSuppress = on;
        RefreshCutoutPolicy();
    }
}

void State_Reapply() {
    if (!g_dx.context) return;
    ApplyBlend();
    ApplyDepthState();
    ApplyCull();
    ApplyClip();
    ApplySampler();
    if (g_dx.cbScreen) DX11_UpdateScreenCB(g_dx);
    g_dx.context->VSSetConstantBuffers(0, 1, &g_dx.cbScreen);
    g_dx.context->PSSetConstantBuffers(0, 1, &g_dx.cbScreen);
    Tex_Bind(Tex_Bound());
}

void State_ReleaseCaches() {
    for (auto& e : g_samplerCache) if (e.second) e.second->Release();
    g_samplerCache.clear();
    for (auto*& rs : g_cullStates) { if (rs) rs->Release(); rs = nullptr; }
}

void State_ConfigureCull(bool honor) { g_honorCull = honor; }

void State_Init() {
    g_states.clear();
    g_states[THRASH_STATE_CLEAR_COLOR] = (int)0xFF000000u;
    g_states[THRASH_STATE_SCREEN_WIDTH] = 640;
    g_states[THRASH_STATE_SCREEN_HEIGHT] = 480;
    g_states[THRASH_STATE_ZBUFFER_COMPARE] = 3; // dx7z.dll @ 0x6000C04C: LEQUAL
    g_vertexStride = 32;
    g_indexStride = 4;
    g_dx.clearColorArgb = 0xFF000000u;
    g_samplerKey = SamplerKey{};
    g_clipValid = false;
    g_depthBiasLevel = 0;
    g_dx.depthBias = 0.0f;
    g_dx.fogEnabled = false;
    g_dx.fogDensity = 0.0f;
    g_dx.fogScale = 1.0f;
}

// G3: per-setstate/getstate log lines (11 M per race, fflush each) cost real
// frame time; off unless [display] log_states=1.
static bool g_stateLogEnabled = false;
void State_ConfigureLogging(bool enabled) { g_stateLogEnabled = enabled; }
bool State_LoggingEnabled() { return g_stateLogEnabled; }

static void ApplyDepthState() {
    if (!g_dx.context) return;
    // MB2: while the player's car is being drawn, the same depth state also
    // writes stencil 1 (the motion-blur foreground mask).
    const bool mark = MotionBlur_ForegroundMark();
    auto* state = (mark ? g_dx.depthStatesMark : g_dx.depthStates)[g_dx.depthComparison][g_dx.depthWrite ? 1 : 0];
    if (!state) state = g_dx.depthStates[g_dx.depthComparison][g_dx.depthWrite ? 1 : 0];
    if (state) g_dx.context->OMSetDepthStencilState(state, mark ? 1 : 0);
}
void State_ApplyDepth() { ApplyDepthState(); }

void State_Set(int id, int value) {
    if ((id == THRASH_STATE_ZBUFFER_COMPARE && (value < 0 || value > 7)) ||
        (id == THRASH_STATE_ZBUFFER_MODE && (value < 0 || value > 2))) return;
    g_states[id] = value;

    if (g_stateLogEnabled)
        Log("setstate(0x%X, %d / 0x%X)", id, value, value);

    switch (id) {
    // PORSCHE.EXE 00539380 reads camera+0xB8 and sends it as state 3
    // immediately before clearwindow. dx7z honors that packed D3DCOLOR;
    // retaining black here erased the race sky in auxiliary views.
    case THRASH_STATE_CLEAR_COLOR:
        g_dx.clearColorArgb = (uint32_t)value;
        break;

    case THRASH_STATE_SCREEN_WIDTH:
    case THRASH_STATE_SCREEN_HEIGHT:
        break;

    // dx7z.dll @ 0x600030B9: mode 0 forces ALWAYS/no writes. Modes 1/2
    // restore the saved comparison and enable writes, even after state 0x39=0.
    // The menu uses Z mode 1; mode 2 currently uses the same normalized Z input.
    case THRASH_STATE_ZBUFFER_MODE: {
        g_dx.depthComparison = value ? g_states[THRASH_STATE_ZBUFFER_COMPARE] : 7;
        g_dx.depthWrite = value != 0;
        ApplyDepthState();
        RefreshCutoutPolicy();
        break;
    }
    // dx7z.dll @ 0x60003141: all eight Glide comparisons, including GEQUAL
    // (6) used by menu passes. Reject invalid values without corrupting the cache.
    case THRASH_STATE_ZBUFFER_COMPARE: {
        g_dx.depthComparison = value;
        ApplyDepthState();
        RefreshCutoutPolicy();
        break;
    }
    // dx7z.dll @ 0x60003227 -> grDepthMask. This must not change texture t0.
    case THRASH_STATE_ZBUFFER_WRITE: {
        g_dx.depthWrite = value != 0;
        ApplyDepthState();
        RefreshCutoutPolicy();
        break;
    }
    // Depth-tested transparent texels must be discarded before writing Z.
    // dx7z.dll @ 0x6000374C: 0 disables alpha test, otherwise GEQUAL/ref byte.
    case THRASH_STATE_ALPHA_TEST_REF: {
        g_dx.alphaRef = (float)(value & 0xFF) / 255.0f;
        DX11_MarkScreenCBDirty(g_dx);
        RefreshCutoutPolicy();
        break;
    }

    // dx7z has no setstate case for 0x12/0x13 (jump table 600038E0 -> default);
    // the blend function is state 0x38 below.
    case THRASH_STATE_ALPHA_BLEND_SRC:
    case THRASH_STATE_ALPHA_BLEND_DST:
        break;

    // dx7z 6000366C: grAlphaBlendFunction from a four-entry table. dx7z's
    // getstate (60005CC0) returns the last value set, and the game relies on
    // that: it saves getstate(0x38), draws its glow with 1 and sets the saved
    // value back. Returning anything else left the additive mode stuck for
    // the rest of the frame (washed-out flashes, over-bright loading screen).
    case THRASH_STATE_BLEND_MODE:
        if (value >= 0 && value < 4) ApplyBlend();
        break;

    // dx7z 6000627B + software cull in the draw loops (see CullState).
    case THRASH_STATE_CULL_MODE:
        if (value >= 0 && value <= 2) ApplyCull();
        break;

    // dx7z 6000364B: grDepthBiasLevel(value). Shadows, decals and road marks.
    case THRASH_STATE_DEPTH_BIAS:
        ApplyDepthBias(value);
        break;

    // dx7z 60002D03: grTexFilterMode(tmu, v, v): 0 point, 1 bilinear.
    case THRASH_STATE_TEXTURE_FILTER:
        if (value == 0 || value == 1) { g_samplerKey.filter = value; ApplySampler(); }
        break;

    // dx7z 600036FC: grTexLodBiasValue(tmu, float). The game sharpens car
    // pages with -1.0 / -3.25 and softens others with +0.25.
    case THRASH_STATE_TEXTURE_LOD_BIAS:
        g_samplerKey.lodBiasBits = value;
        ApplySampler();
        break;

    // dx7z 6000347C / 600034FE: grTexClampMode S / T: 0 CLAMP, 1 WRAP, 2 MIRROR.
    case THRASH_STATE_TEXTURE_CLAMP_S:
        if (value >= 0 && value <= 2) { g_samplerKey.clampS = value; ApplySampler(); }
        break;
    case THRASH_STATE_TEXTURE_CLAMP_T:
        if (value >= 0 && value <= 2) { g_samplerKey.clampT = value; ApplySampler(); }
        break;

    // dx7z 6000357F formats the float and passes it on (12 writes per race);
    // not a sampler state. Kept as a no-op.
    case THRASH_STATE_TEXTURE_CLAMP:
    case 0x10041:
    case 0x10042:
        break;

    case THRASH_STATE_VERTEX_STRIDE:
        if (value >= 24 && value <= 128)
            g_vertexStride = value;
        Log("setstate VERTEX_STRIDE = %d", value);
        break;

    case THRASH_STATE_INDEX_FORMAT:
        switch (value) {
        case 1: g_indexStride = 1; break;
        case 2: g_indexStride = 2; break;
        case 3: g_indexStride = 4; break;
        case 4: g_indexStride = 8; break;
        default: g_indexStride = 2; break;
        }
        Log("setstate INDEX_FORMAT = %d -> indexStride=%d", value, g_indexStride);
        break;

    // dx7z 6000326A: 0 grFogMode(GR_FOG_DISABLE), 1 grFogMode(GR_FOG_WITH_TABLE_ON_Q),
    // 2 / 4 / 8 select the table generator (4 = guFogGenerateExp, the only one
    // the game uses); 3, 5..7 are rejected.
    case THRASH_STATE_FOG_MODE_GLIDE:
        if (value == 0 || value == 1) {
            g_dx.fogEnabled = value != 0;
            DX11_MarkScreenCBDirty(g_dx);
        } else if (value != 4 && value != 2 && value != 8) {
            Log("G1 fog: unsupported table mode %d", value);
        }
        break;

    // dx7z 600032C5: density as float, or 1/value for small integers, then the
    // exp fog table is regenerated (state 0x15 mode 4).
    case THRASH_STATE_FOG_DENSITY: {
        float density = 0.0f;
        if (value != 0 && (value & 0xFFFFF000) == 0) density = 1.0f / (float)value;
        else memcpy(&density, &value, sizeof(density));
        // HZ7: a cubed key density of 1.0 (Lost Canyons cave sky, fully
        // fogged) was dropped here as invalid and switched the fog off.
        if (!(density >= 0.0f && density < 64.0f)) density = 0.0f;
        g_dx.fogDensity = density;
        UpdateFogScale();
        DX11_MarkScreenCBDirty(g_dx);
        break;
    }

    // dx7z 60003259: grFogColorValue(ARGB).
    case THRASH_STATE_FOG_COLOR_GLIDE:
        g_dx.fogColorArgb = (uint32_t)value;
        DX11_MarkScreenCBDirty(g_dx);
        break;

    case THRASH_STATE_FOG_MODE:
    case THRASH_STATE_FOG_COLOR:
    case THRASH_STATE_GAMMA:
        break;

    case THRASH_STATE_WINDOW_HANDLE:
        break;
    }
}

void State_SetDirect(int id, int value) {
    g_states[id] = value;
}

int State_Get(int id) {
    auto it = g_states.find(id);
    int val = (it != g_states.end()) ? it->second : 0;
    if (g_stateLogEnabled)
        Log("getstate(0x%X) -> %d / 0x%X", id, val, val);
    return val;
}

int State_Peek(int id) {
    auto it = g_states.find(id);
    return it != g_states.end() ? it->second : 0;
}
