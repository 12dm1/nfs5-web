#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>

enum class DisplayMode { Borderless, Windowed, Exclusive };

struct DisplayConfig {
    int renderW       = 0;
    int renderH       = 0;
    DisplayMode mode  = DisplayMode::Borderless;
    bool usedFallback = false;
};

DisplayConfig ResolveDisplayConfig(int iniW, int iniH, const char* iniMode,
                                   int desktopW, int desktopH);

struct DX11Device {
    ID3D11Device*           device          = nullptr;
    ID3D11DeviceContext*    context         = nullptr;
    IDXGISwapChain*         swapChain       = nullptr;
    ID3D11RenderTargetView* rtv             = nullptr;
    ID3D11DepthStencilView* dsv             = nullptr;
    ID3D11Texture2D*        depthBuffer     = nullptr;
    // MB1: read view of the main depth buffer (null when unavailable).
    ID3D11ShaderResourceView* depthSrv      = nullptr;
    ID3D11ShaderResourceView* stencilSrv    = nullptr;   // MB2: G8 view of the same buffer
    // AA1: multisampled colour target. With msaaSamples > 1, `rtv` is the MSAA
    // texture's view and `backbufferRtv` the swap chain's; the scene is
    // resolved into the back buffer before Present / readrect. Depth/stencil
    // and their SRVs carry the same sample count (Texture2DMS views).
    int                     msaaSamples     = 1;
    bool                    alphaToCoverage = false;
    ID3D11Texture2D*        msaaColor       = nullptr;
    ID3D11RenderTargetView* backbufferRtv   = nullptr;

    ID3D11VertexShader*     vertexShader    = nullptr;
    ID3D11PixelShader*      pixelShaderTex  = nullptr;
    ID3D11PixelShader*      pixelShaderFlat = nullptr;
    // PERF3: the pixel shader the normal draw path has bound (Tex_Bind: Tex or
    // Flat), not owned. Passes that switch shaders restore this and
    // vertexShader instead of querying the context (Get*Shader + Release
    // cost ~50 us per pair in some sessions).
    ID3D11PixelShader*      boundPS = nullptr;
    // PERF5: the blend state last bound through DX11_SetBlend, not owned.
    // Never OMGetBlendState: Get* AddRefs the object, and when its refcount
    // qword straddles a cache line (heap placement, random per launch) every
    // AddRef/Release is a split lock, ~50 us each; per car reflection draw
    // that was the whole 15 fps race (2026-10-18, blendAlpha).
    ID3D11BlendState*       boundBlend = nullptr;
    // PERF6: sampler slot 0 last bound through DX11_SetSampler0, not owned.
    // Both helpers skip a bind of what is already bound: in slow launches
    // the NVIDIA driver's worker thread spends ~35 % in lock cmpxchg8b
    // refcount updates per bind (same split-lock lottery, driver objects).
    ID3D11SamplerState*     boundSampler0 = nullptr;
    uint32_t                bindsIssued = 0, bindsSkipped = 0;
    ID3D11VertexShader*     clearVS         = nullptr;
    ID3D11PixelShader*      clearPS         = nullptr;
    // R2 car reflection pass (cubemap sampled with a decoded vertex normal).
    ID3D11VertexShader*     reflectVS       = nullptr;
    ID3D11PixelShader*      reflectPS       = nullptr;
    ID3D11Buffer*           cbReflect       = nullptr;
    // MAT7 track material shading (PBR from material_core.inc).
    ID3D11VertexShader*     materialVS      = nullptr;
    ID3D11PixelShader*      materialPS      = nullptr;
    ID3D11Buffer*           cbMaterial      = nullptr;
    ID3D11InputLayout*      inputLayout     = nullptr;

    ID3D11Buffer*           cbScreen        = nullptr;
    ID3D11SamplerState*     samplerLinear   = nullptr;
    ID3D11SamplerState*     samplerPoint    = nullptr;
    ID3D11BlendState*       blendAlpha      = nullptr;
    ID3D11BlendState*       blendOpaque     = nullptr;
    ID3D11BlendState*       blendNoColor    = nullptr;
    // G1: THRASH state 0x38 -> dx7z grAlphaBlendFunction: 0 SrcA/InvSrcA,
    // 1 SrcA/ONE (additive), 2 ZERO/InvSrcA, 3 DstColor/ZERO (modulate).
    ID3D11BlendState*       blendModes[4]   = {};
    // AA1: the same four with alpha-to-coverage (alpha-tested foliage edges).
    ID3D11BlendState*       blendModesA2C[4] = {};
    // MAT14d/MAT16: cubemap capture variants. The cube's alpha holds the
    // sun: every draw writes RGB only, except the additive sun / lens-flare
    // sprite draws (sunSpriteDraw, set around FUN_00460290 / FUN_00460460 by
    // the MAT16 hooks in thrash_exports.cpp), which add their premultiplied
    // colour and their peak luminance to the alpha. Selected while
    // captureAlphaMask is set (scene_probe); sunMaskDraw mirrors the choice
    // into ScreenConstants emissive.w for the pixel shader.
    ID3D11BlendState*       blendModesRgb[4] = {};
    ID3D11BlendState*       blendModesA2CRgb[4] = {};
    ID3D11BlendState*       blendSunMask = nullptr;
    ID3D11BlendState*       blendOpaqueRgb = nullptr;
    // CAR2 fix 7: capture variants of mode 0 (opaque / cutout occluders)
    // that also clear the alpha they cover, used during the track and car
    // phases (captureClearAlpha) so the sun mask ends where geometry
    // stands in front of the sun. sunMaskHooks: the MAT16 sprite hooks are
    // installed (the mask is meaningful).
    ID3D11BlendState*       blendModesClearA[4] = {};
    ID3D11BlendState*       blendModesA2CClearA[4] = {};
    bool                    captureClearAlpha = false;   // sky, track, cars phases
    bool                    captureOpaqueGain = false;   // CAR2 fix 9: track, cars phases
    bool                    sunMaskHooks = false;
    bool                    captureAlphaMask = false;
    bool                    sunSpriteDraw = false;
    bool                    sunMaskDraw = false;
    ID3D11RasterizerState*  rasterState     = nullptr;
    // [Glide comparison 0..7][write enabled]. ALWAYS + no writes is the 2D default.
    ID3D11DepthStencilState* depthStates[8][2] = {};
    // MB2: same states with a stencil write (ref 1, ALWAYS/REPLACE) that marks
    // the player's car pixels for the motion-blur foreground mask.
    ID3D11DepthStencilState* depthStatesMark[8][2] = {};
    int     depthComparison = 7;
    bool    depthWrite      = false;

    HWND    hwnd        = nullptr;
    WNDPROC originalWndProc = nullptr;
    DisplayMode presentationMode = DisplayMode::Borderless;
    int     presentationW = 0;
    int     presentationH = 0;
    bool    windowSubclassed = false;
    bool    active      = true;
    bool    minimized   = false;
    // GPU backbuffer / viewport size (the resolved INI render size).
    int     width       = 640;
    int     height      = 480;
    // Optional size of the currently bound auxiliary render target. This is
    // local to an offscreen pass (mirror/reflection capture) and must never be
    // reported as, or copied into, the configured display dimensions above.
    int     auxiliaryTargetW = 0;
    int     auxiliaryTargetH = 0;
    // Game vertex / LFB coordinate space. Menu is always 640x480.
    int     logicalW    = 640;
    int     logicalH    = 480;
    // Last in-game video setting from THRASH_setvideomode (race HUD space).
    int     gameVideoW  = 640;
    int     gameVideoH  = 480;
    float   alphaRef    = 0.0f;
    // G1: Glide depth-bias level (state 0x18) as a relative W offset, level / depthBiasDivisor.
    float   depthBias   = 0.0f;
    // Camera clear color from THRASH state 3, packed as D3DCOLOR/ARGB.
    uint32_t clearColorArgb = 0xFF000000u;
    // G1 fog (dx7z states 0x15 grFogMode / 0x0E density / 0x0F grFogColorValue).
    bool     fogEnabled  = false;
    uint32_t fogColorArgb = 0;
    float    fogDensity  = 0.0f;
    float    fogScale    = 1.0f;
    bool    initialized = false;
    // G3: ScreenConstants (alphaRef, fog, depth bias) change thousands of
    // times per frame through setstate; they are uploaded once per draw.
    bool    screenCbDirty = false;
    // G12: emissive gain for the current draw (1 = none); > 1 only while an
    // HDR cubemap face is the target and the draw is additive.
    float   emissive = 1.0f;
    float   emissiveThreshold = 0.6f;
    float   emissivePower = 2.0f;
    // AA1: alpha-to-coverage blend state currently bound (shader sharpens
    // alpha at the test threshold instead of discarding).
    bool    a2cActive = false;
    bool    hardCutoutActive = false; // World cutouts: discard, opaque survivors, clamp sampler.
};

void ReadDisplayIni(int* outW, int* outH, char* outMode, int modeSize);
// PERF1: swap interval of DX11_Present (1 = vsync, 0 = unthrottled).
void DX11_SetPresentInterval(int interval);
// One integer key from [display]; defaultValue when the INI or key is absent.
int ReadDisplayIniInt(const char* key, int defaultValue);
// One float key from [display]; defaultValue when absent or not numeric.
float ReadDisplayIniFloat(const char* key, float defaultValue);
uint32_t ReadDisplayIniHex(const char* key, uint32_t defaultValue);
// N1: keys of other dx11z.ini sections; ReadIniString returns false when absent/empty.
int ReadIniInt(const char* section, const char* key, int defaultValue);
bool ReadIniString(const char* section, const char* key, char* out, int size);

bool DX11_Init(DX11Device& dev, HWND hwnd, int width, int height);
// Applies the requested window style without activating or raising the game.
// Exclusive currently resolves to borderless until a separate tested DXGI
// exclusive lifecycle is implemented.
bool DX11_ConfigureWindow(DX11Device& dev, HWND hwnd, const DisplayConfig& cfg);
void DX11_DetachWindow(DX11Device& dev);
POINT DX11_MapClientPointToLogical(int x, int y, int clientW, int clientH,
                                   int logicalW, int logicalH);
// Query before window configuration: automatic INI dimensions come from the
// selected monitor's desktop mode, not from a resized client area.
bool DX11_GetDesktopMode(HWND hwnd, DXGI_MODE_DESC& mode);
// Initialize the pipeline on an existing device/targets (also used by offscreen tests).
bool DX11_InitPipeline(DX11Device& dev);
void DX11_Shutdown(DX11Device& dev);
void DX11_Present(DX11Device& dev);
void DX11_Clear(DX11Device& dev, bool depthOnly = false);
void DX11_ResizeIfNeeded(DX11Device& dev, int w, int h);
void DX11_SetLogicalSize(DX11Device& dev, int w, int h);
int  DX11_TargetWidth(const DX11Device& dev);
int  DX11_TargetHeight(const DX11Device& dev);
void DX11_SetAuxiliaryTargetSize(DX11Device& dev, int w, int h);
void DX11_ApplyGpuViewport(DX11Device& dev);
void DX11_UpdateScreenCB(DX11Device& dev);
// Mark the screen constants stale; DX11_FlushScreenCB uploads them if so.
inline void DX11_MarkScreenCBDirty(DX11Device& dev) { dev.screenCbDirty = true; }
// PERF5: every blend bind goes through here (see boundBlend). The wrapper
// never uses the blend factor, so the default restores exactly.
inline void DX11_SetBlend(DX11Device& dev, ID3D11BlendState* state,
                          const FLOAT* factors = nullptr, UINT mask = 0xFFFFFFFF) {
    if (state == dev.boundBlend && state) { ++dev.bindsSkipped; return; }
    dev.context->OMSetBlendState(state, factors, mask);
    dev.boundBlend = state;
    ++dev.bindsIssued;
}
// PERF6: every slot-0 sampler bind goes through here (see boundSampler0).
inline void DX11_SetSampler0(DX11Device& dev, ID3D11SamplerState* sampler) {
    if (sampler == dev.boundSampler0 && sampler) { ++dev.bindsSkipped; return; }
    dev.context->PSSetSamplers(0, 1, &sampler);
    dev.boundSampler0 = sampler;
    ++dev.bindsIssued;
}
void DX11_FlushScreenCB(DX11Device& dev);
// G7: read a rectangle of the back buffer (logical coordinates) as RGB565.
bool DX11_ReadRect565(DX11Device& dev, int x, int y, int w, int h, uint16_t* out);
// AA1: resolve the MSAA scene into the swap-chain back buffer (no-op without
// MSAA). Present does it; readrect does it on demand.
void DX11_ResolveToBackbuffer(DX11Device& dev);
// AA1: request MSAA before DX11_Init (validated against the device).
void DX11_ConfigureMsaa(int samples, bool alphaToCoverage);

extern DX11Device g_dx;
extern DisplayConfig g_displayConfig;
