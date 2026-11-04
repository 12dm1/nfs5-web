#include "perf.h"
#include "dx11_device.h"
#include "dx11_shaders.h"
#include "dx11_state.h"
#include "materials.h"
#include "log.h"
#include <windows.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <wrl/client.h>

// PERF7: 0 = no NVIDIA driver worker thread (see DX11_Init); [display] driver_threads=1 restores it.
#define DRIVER_THREADS_DEFAULT 0

DX11Device g_dx;
DisplayConfig g_displayConfig;

// P5 keeps DXGI windowed for both supported presentation modes. The configured
// render size is independent from the client size, so no display-mode switch is
// requested from DXGI.
static const UINT kSwapChainFlags = 0;
static const char kDeviceWindowProperty[] = "dx11z.device.window-state";

template<typename T>
static void SafeRelease(T*& ptr) {
    if (ptr) { ptr->Release(); ptr = nullptr; }
}

static bool CompileShaders(DX11Device& dev) {
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* errBlob = nullptr;

    HRESULT hr = D3DCompile(g_shaderSource, strlen(g_shaderSource), "thrash_shaders",
                            nullptr, nullptr, "VSMain", "vs_4_0", flags, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        Log("VS compile failed: %s", errBlob ? (char*)errBlob->GetBufferPointer() : "unknown");
        SafeRelease(errBlob);
        return false;
    }
    SafeRelease(errBlob);

    hr = dev.device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                        nullptr, &dev.vertexShader);
    if (FAILED(hr)) { SafeRelease(vsBlob); return false; }


    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,      0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    1, DXGI_FORMAT_R8G8B8A8_UNORM,      0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };

    hr = dev.device->CreateInputLayout(layout, 5, vsBlob->GetBufferPointer(),
                                       vsBlob->GetBufferSize(), &dev.inputLayout);
    SafeRelease(vsBlob);
    if (FAILED(hr)) return false;

    ID3DBlob* psTexBlob = nullptr;
    hr = D3DCompile(g_shaderSource, strlen(g_shaderSource), "thrash_shaders",
                    nullptr, nullptr, "PSMain", "ps_4_0", flags, 0, &psTexBlob, &errBlob);
    if (FAILED(hr)) {
        Log("PS(tex) compile failed: %s", errBlob ? (char*)errBlob->GetBufferPointer() : "unknown");
        SafeRelease(errBlob);
        return false;
    }
    SafeRelease(errBlob);

    hr = dev.device->CreatePixelShader(psTexBlob->GetBufferPointer(), psTexBlob->GetBufferSize(),
                                       nullptr, &dev.pixelShaderTex);
    SafeRelease(psTexBlob);
    if (FAILED(hr)) return false;

    ID3DBlob* psFlatBlob = nullptr;
    hr = D3DCompile(g_shaderSource, strlen(g_shaderSource), "thrash_shaders",
                    nullptr, nullptr, "PSMainNoTex", "ps_4_0", flags, 0, &psFlatBlob, &errBlob);
    if (FAILED(hr)) {
        Log("PS(flat) compile failed: %s", errBlob ? (char*)errBlob->GetBufferPointer() : "unknown");
        SafeRelease(errBlob);
        return false;
    }
    SafeRelease(errBlob);

    hr = dev.device->CreatePixelShader(psFlatBlob->GetBufferPointer(), psFlatBlob->GetBufferSize(),
                                       nullptr, &dev.pixelShaderFlat);
    SafeRelease(psFlatBlob);
    if (FAILED(hr)) return false;

    ID3DBlob* clearBlob = nullptr;
    for (int stage = 0; stage < 2; ++stage) {
        hr = D3DCompile(g_clearShaderSource, strlen(g_clearShaderSource), "thrash_clear",
            nullptr, nullptr, stage ? "PSClear" : "VSClear", stage ? "ps_4_0" : "vs_4_0",
            flags, 0, &clearBlob, &errBlob);
        if (FAILED(hr)) {
            Log("Clear shader compile failed: %s", errBlob ? (char*)errBlob->GetBufferPointer() : "unknown");
            SafeRelease(errBlob);
            return false;
        }
        SafeRelease(errBlob);
        hr = stage ? dev.device->CreatePixelShader(clearBlob->GetBufferPointer(),
                         clearBlob->GetBufferSize(), nullptr, &dev.clearPS)
                   : dev.device->CreateVertexShader(clearBlob->GetBufferPointer(),
                         clearBlob->GetBufferSize(), nullptr, &dev.clearVS);
        SafeRelease(clearBlob);
        if (FAILED(hr)) return false;
    }

    ID3DBlob* reflectBlob = nullptr;
    for (int stage = 0; stage < 2; ++stage) {
        hr = D3DCompile(g_reflectShaderSource, strlen(g_reflectShaderSource), "thrash_reflect",
            nullptr, nullptr, stage ? "PSReflect" : "VSReflect", stage ? "ps_4_0" : "vs_4_0",
            flags, 0, &reflectBlob, &errBlob);
        if (FAILED(hr)) {
            Log("Reflect shader compile failed: %s", errBlob ? (char*)errBlob->GetBufferPointer() : "unknown");
            SafeRelease(errBlob);
            return false;
        }
        SafeRelease(errBlob);
        hr = stage ? dev.device->CreatePixelShader(reflectBlob->GetBufferPointer(),
                         reflectBlob->GetBufferSize(), nullptr, &dev.reflectPS)
                   : dev.device->CreateVertexShader(reflectBlob->GetBufferPointer(),
                         reflectBlob->GetBufferSize(), nullptr, &dev.reflectVS);
        SafeRelease(reflectBlob);
        if (FAILED(hr)) return false;
    }
    return true;
}

static int g_requestedMsaa = 1;
static bool g_requestedA2C = false;

void DX11_ConfigureMsaa(int samples, bool alphaToCoverage) {
    g_requestedMsaa = (samples == 2 || samples == 4 || samples == 8) ? samples : 1;
    g_requestedA2C = alphaToCoverage;
}

static bool CreateRenderTarget(DX11Device& dev) {
    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = dev.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (FAILED(hr)) return false;
    D3D11_TEXTURE2D_DESC bbDesc = {};
    backBuffer->GetDesc(&bbDesc);

    hr = dev.device->CreateRenderTargetView(backBuffer, nullptr, &dev.backbufferRtv);
    backBuffer->Release();
    if (FAILED(hr)) return false;

    // AA1: MSAA colour target in front of the swap chain.
    dev.msaaSamples = 1;
    dev.alphaToCoverage = false;
    if (g_requestedMsaa > 1) {
        UINT quality = 0;
        if (SUCCEEDED(dev.device->CheckMultisampleQualityLevels(bbDesc.Format, (UINT)g_requestedMsaa, &quality)) && quality > 0) {
            D3D11_TEXTURE2D_DESC cd = {};
            cd.Width = dev.width;
            cd.Height = dev.height;
            cd.MipLevels = 1;
            cd.ArraySize = 1;
            cd.Format = bbDesc.Format;
            cd.SampleDesc.Count = (UINT)g_requestedMsaa;
            cd.Usage = D3D11_USAGE_DEFAULT;
            cd.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (SUCCEEDED(dev.device->CreateTexture2D(&cd, nullptr, &dev.msaaColor)) &&
                SUCCEEDED(dev.device->CreateRenderTargetView(dev.msaaColor, nullptr, &dev.rtv))) {
                dev.msaaSamples = g_requestedMsaa;
                dev.alphaToCoverage = g_requestedA2C;
                Log("AA1: MSAA x%d colour target %dx%d (alpha-to-coverage %d)", dev.msaaSamples,
                    dev.width, dev.height, dev.alphaToCoverage);
            } else {
                Log("AA1: MSAA x%d target creation failed, single-sample", g_requestedMsaa);
                SafeRelease(dev.rtv);
                SafeRelease(dev.msaaColor);
            }
        } else {
            Log("AA1: MSAA x%d not supported for the swap-chain format, single-sample", g_requestedMsaa);
        }
    }
    if (!dev.rtv) {
        dev.rtv = dev.backbufferRtv;
        dev.rtv->AddRef();
    }

    // MB1: typeless storage so the post-process can read the depth of the
    // finished scene (R24 view) while the DSV keeps its D24S8 format.
    const bool ms = dev.msaaSamples > 1;
    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = dev.width;
    depthDesc.Height = dev.height;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    depthDesc.SampleDesc.Count = (UINT)dev.msaaSamples;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    hr = dev.device->CreateTexture2D(&depthDesc, nullptr, &dev.depthBuffer);
    if (FAILED(hr)) return false;

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = ms ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
    hr = dev.device->CreateDepthStencilView(dev.depthBuffer, &dsvDesc, &dev.dsv);
    if (FAILED(hr)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srvDesc.ViewDimension = ms ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    hr = dev.device->CreateShaderResourceView(dev.depthBuffer, &srvDesc, &dev.depthSrv);
    if (FAILED(hr)) {
        Log("MB1: depth SRV creation failed 0x%08X (motion blur unavailable)", hr);
        dev.depthSrv = nullptr;
    }
    srvDesc.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    hr = dev.device->CreateShaderResourceView(dev.depthBuffer, &srvDesc, &dev.stencilSrv);
    if (FAILED(hr)) {
        Log("MB2: stencil SRV creation failed 0x%08X (foreground mask unavailable)", hr);
        dev.stencilSrv = nullptr;
    }
    return true;
}

static bool CreateStates(DX11Device& dev) {
    // M1: [display] texture_aniso = 2..16 uses anisotropic filtering for the
    // linear sampler (textures now carry mip chains); 0/1 = trilinear.
    int aniso = ReadDisplayIniInt("texture_aniso", 16);
    if (aniso > D3D11_MAX_MAXANISOTROPY) aniso = D3D11_MAX_MAXANISOTROPY;
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = (aniso >= 2) ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.MaxAnisotropy = (aniso >= 2) ? (UINT)aniso : 1;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MinLOD = 0.0f;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev.device->CreateSamplerState(&sd, &dev.samplerLinear);
    Log("M1: linear sampler %s (texture_aniso=%d)",
        aniso >= 2 ? "anisotropic" : "trilinear", aniso);

    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.MaxAnisotropy = 1;
    dev.device->CreateSamplerState(&sd, &dev.samplerPoint);

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev.device->CreateBlendState(&bd, &dev.blendAlpha);

    bd.RenderTarget[0].BlendEnable = FALSE;
    dev.device->CreateBlendState(&bd, &dev.blendOpaque);

    // G1: the four grAlphaBlendFunction pairs dx7z selects from state 0x38
    // (jump table 600039BC): rgb (src, dst); alpha always (ONE, ZERO).
    static const D3D11_BLEND kBlendPairs[4][2] = {
        {D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA},
        {D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE},
        {D3D11_BLEND_ZERO,      D3D11_BLEND_INV_SRC_ALPHA},
        {D3D11_BLEND_DEST_COLOR, D3D11_BLEND_ZERO},
    };
    for (int i = 0; i < 4; i++) {
        D3D11_BLEND_DESC md = {};
        md.RenderTarget[0].BlendEnable = TRUE;
        md.RenderTarget[0].SrcBlend = kBlendPairs[i][0];
        md.RenderTarget[0].DestBlend = kBlendPairs[i][1];
        md.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        md.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        md.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        md.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        md.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(dev.device->CreateBlendState(&md, &dev.blendModes[i]))) return false;
        md.AlphaToCoverageEnable = TRUE;
        if (FAILED(dev.device->CreateBlendState(&md, &dev.blendModesA2C[i]))) return false;
        // MAT14d: capture variants, RGB only (the alpha holds the sun mask).
        md.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        md.AlphaToCoverageEnable = FALSE;
        if (FAILED(dev.device->CreateBlendState(&md, &dev.blendModesRgb[i]))) return false;
        md.AlphaToCoverageEnable = TRUE;
        if (FAILED(dev.device->CreateBlendState(&md, &dev.blendModesA2CRgb[i]))) return false;
        // CAR2 fix 7: the same, attenuating the alpha (the sun mask) by the
        // draw's coverage: alpha := alpha * (1 - srcAlpha). Opaque geometry
        // clears it, a thin cloud layer only dims it.
        md.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        md.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        md.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        md.AlphaToCoverageEnable = FALSE;
        if (FAILED(dev.device->CreateBlendState(&md, &dev.blendModesClearA[i]))) return false;
        md.AlphaToCoverageEnable = TRUE;
        if (FAILED(dev.device->CreateBlendState(&md, &dev.blendModesA2CClearA[i]))) return false;
    }
    {
        // MAT16: the sun / lens-flare sprites (additive, mode 1) inside a
        // capture. The pixel shader premultiplies their colour and puts its
        // peak into the alpha (ScreenConstants emissive.w), so the cube's
        // alpha accumulates the sun's luminance next to the colour.
        D3D11_BLEND_DESC sm = {};
        sm.RenderTarget[0].BlendEnable = TRUE;
        sm.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        sm.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        sm.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        sm.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        sm.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        sm.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        sm.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(dev.device->CreateBlendState(&sm, &dev.blendSunMask))) return false;
        D3D11_BLEND_DESC op = {};
        op.RenderTarget[0].BlendEnable = FALSE;
        op.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        op.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        op.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        op.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        op.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        op.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        op.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        if (FAILED(dev.device->CreateBlendState(&op, &dev.blendOpaqueRgb))) return false;
    }

    bd.RenderTarget[0].RenderTargetWriteMask = 0;
    if (FAILED(dev.device->CreateBlendState(&bd, &dev.blendNoColor))) return false;

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = TRUE;
    dev.device->CreateRasterizerState(&rd, &dev.rasterState);

    // Glide comparison 0..7 maps to D3D11 comparison 1..8. Keep depth enabled
    // even for ALWAYS/write-only: DepthEnable=FALSE would also suppress writes.
    for (int compare = 0; compare < 8; ++compare) {
        for (int write = 0; write < 2; ++write) {
            D3D11_DEPTH_STENCIL_DESC dsd = {};
            dsd.DepthEnable = TRUE;
            dsd.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            dsd.DepthFunc = (D3D11_COMPARISON_FUNC)(compare + 1);
            HRESULT hr = dev.device->CreateDepthStencilState(&dsd, &dev.depthStates[compare][write]);
            if (FAILED(hr)) {
                Log("CreateDepthStencilState(%d, %d) failed: 0x%08X", compare, write, hr);
                return false;
            }
            dsd.StencilEnable = TRUE;
            dsd.StencilReadMask = 0xFF;
            dsd.StencilWriteMask = 0xFF;
            dsd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
            dsd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
            dsd.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
            dsd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
            dsd.BackFace = dsd.FrontFace;
            hr = dev.device->CreateDepthStencilState(&dsd, &dev.depthStatesMark[compare][write]);
            if (FAILED(hr)) {
                Log("CreateDepthStencilState mark (%d, %d) failed: 0x%08X", compare, write, hr);
                return false;
            }
        }
    }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 80;   // ScreenConstants: five float4 rows (G1 fog, G12 emissive curve)
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev.device->CreateBuffer(&cbd, nullptr, &dev.cbScreen);
    cbd.ByteWidth = 96;   // six float4 rows (ReflectConstants)
    dev.device->CreateBuffer(&cbd, nullptr, &dev.cbReflect);

    return true;
}

static DisplayMode ParseDisplayMode(const char* s) {
    if (!s || !*s) return DisplayMode::Borderless;
    if (_stricmp(s, "windowed") == 0) return DisplayMode::Windowed;
    if (_stricmp(s, "exclusive") == 0) return DisplayMode::Exclusive;
    return DisplayMode::Borderless;
}

static const char* DisplayModeName(DisplayMode m) {
    switch (m) {
    case DisplayMode::Windowed:  return "windowed";
    case DisplayMode::Exclusive: return "exclusive";
    default:                     return "borderless";
    }
}

POINT DX11_MapClientPointToLogical(int x, int y, int clientW, int clientH,
                                   int logicalW, int logicalH) {
    POINT result = {x, y};
    if (clientW <= 0 || clientH <= 0 || logicalW <= 0 || logicalH <= 0)
        return result;

    result.x = (LONG)((long long)x * logicalW / clientW);
    result.y = (LONG)((long long)y * logicalH / clientH);
    result.x = std::max(0L, std::min((LONG)logicalW - 1, result.x));
    result.y = std::max(0L, std::min((LONG)logicalH - 1, result.y));
    return result;
}

static bool IsClientMouseMessage(UINT message) {
    switch (message) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
        return true;
    default:
        return false;
    }
}

static LPARAM MapMouseLParam(const DX11Device& dev, LPARAM value) {
    RECT client = {};
    if (!dev.hwnd || !GetClientRect(dev.hwnd, &client))
        return value;
    const int clientW = client.right - client.left;
    const int clientH = client.bottom - client.top;
    const int x = (short)LOWORD(value);
    const int y = (short)HIWORD(value);
    POINT mapped = DX11_MapClientPointToLogical(
        x, y, clientW, clientH, dev.logicalW, dev.logicalH);
    return MAKELPARAM((short)mapped.x, (short)mapped.y);
}

static LRESULT CALLBACK DX11_WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* dev = (DX11Device*)GetPropA(hwnd, kDeviceWindowProperty);
    if (!dev || !dev->originalWndProc)
        return DefWindowProcA(hwnd, message, wParam, lParam);

    if (message == WM_ACTIVATEAPP) {
        const bool active = wParam != FALSE;
        if (dev->active != active)
            Log("window focus: %s", active ? "active" : "inactive");
        dev->active = active;
        if (!active) {
            ReleaseCapture();
            ClipCursor(nullptr);
        }
    } else if (message == WM_KILLFOCUS || message == WM_CANCELMODE) {
        ReleaseCapture();
        ClipCursor(nullptr);
    } else if (message == WM_SIZE) {
        const bool minimized = wParam == SIZE_MINIMIZED;
        if (dev->minimized != minimized)
            Log("window state: %s", minimized ? "minimized" : "restored");
        dev->minimized = minimized;
        if (!minimized) {
            const int width = LOWORD(lParam);
            const int height = HIWORD(lParam);
            if (width > 0 && height > 0) {
                dev->presentationW = width;
                dev->presentationH = height;
            }
        }
        // The configured render target deliberately remains independent from
        // WM_SIZE. DXGI presents that fixed target into the current client area.
    }

    if (IsClientMouseMessage(message))
        lParam = MapMouseLParam(*dev, lParam);

    WNDPROC original = dev->originalWndProc;
    LRESULT result = CallWindowProcA(original, hwnd, message, wParam, lParam);
    if (message == WM_NCDESTROY) {
        RemovePropA(hwnd, kDeviceWindowProperty);
        dev->originalWndProc = nullptr;
        dev->windowSubclassed = false;
        dev->hwnd = nullptr;
    }
    return result;
}

static bool AttachWindow(DX11Device& dev, HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd))
        return false;
    if (dev.windowSubclassed && dev.hwnd == hwnd)
        return true;
    if (dev.windowSubclassed)
        DX11_DetachWindow(dev);

    dev.hwnd = hwnd;
    if (!SetPropA(hwnd, kDeviceWindowProperty, &dev)) {
        Log("SetProp(window state) failed: %lu", GetLastError());
        return false;
    }

    SetLastError(0);
    LONG_PTR original = SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)DX11_WindowProc);
    if (!original && GetLastError() != 0) {
        Log("Window subclass failed: %lu", GetLastError());
        RemovePropA(hwnd, kDeviceWindowProperty);
        return false;
    }
    dev.originalWndProc = (WNDPROC)original;
    dev.windowSubclassed = true;
    return true;
}

static bool DisplayIniPath(char* path, size_t size);

bool DX11_ConfigureWindow(DX11Device& dev, HWND hwnd, const DisplayConfig& cfg) {
    if (!AttachWindow(dev, hwnd))
        return false;

    MONITORINFO monitor = {};
    monitor.cbSize = sizeof(monitor);
    HMONITOR selected = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoA(selected, &monitor)) {
        Log("GetMonitorInfo failed: %lu", GetLastError());
        return false;
    }

    DisplayMode effective = cfg.mode;
    if (effective == DisplayMode::Exclusive) {
        effective = DisplayMode::Borderless;
        Log("display: exclusive is deferred; using borderless presentation");
    }
    dev.presentationMode = effective;

    LONG_PTR oldStyle = GetWindowLongPtrA(hwnd, GWL_STYLE);
    LONG_PTR oldExStyle = GetWindowLongPtrA(hwnd, GWL_EXSTYLE);
    LONG_PTR visible = oldStyle & WS_VISIBLE;
    LONG_PTR style = 0;
    LONG_PTR exStyle = (oldExStyle & ~(LONG_PTR)(WS_EX_TOPMOST | WS_EX_TOOLWINDOW)) |
                       WS_EX_APPWINDOW;
    RECT target = {};

    if (effective == DisplayMode::Windowed) {
        style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | visible;
        target = {0, 0, cfg.renderW, cfg.renderH};
        if (!AdjustWindowRectEx(&target, (DWORD)style, GetMenu(hwnd) != nullptr,
                                (DWORD)exStyle)) {
            Log("AdjustWindowRectEx failed: %lu", GetLastError());
            return false;
        }
        const int outerW = target.right - target.left;
        const int outerH = target.bottom - target.top;
        const int workW = monitor.rcWork.right - monitor.rcWork.left;
        const int workH = monitor.rcWork.bottom - monitor.rcWork.top;
        target.left = monitor.rcWork.left + std::max(0, (workW - outerW) / 2);
        target.top = monitor.rcWork.top + std::max(0, (workH - outerH) / 2);
        target.right = target.left + outerW;
        target.bottom = target.top + outerH;
    } else {
        style = WS_POPUP | visible;
        target = monitor.rcMonitor;
    }

    SetLastError(0);
    LONG_PTR previousStyle = SetWindowLongPtrA(hwnd, GWL_STYLE, style);
    if (!previousStyle && GetLastError() != 0) {
        Log("SetWindowLongPtr(style) failed: %lu", GetLastError());
        return false;
    }
    SetLastError(0);
    LONG_PTR previousExStyle = SetWindowLongPtrA(hwnd, GWL_EXSTYLE, exStyle);
    if (!previousExStyle && GetLastError() != 0) {
        Log("SetWindowLongPtr(exstyle) failed: %lu", GetLastError());
        return false;
    }

    const int outerW = target.right - target.left;
    const int outerH = target.bottom - target.top;
    if (!SetWindowPos(hwnd, HWND_NOTOPMOST, target.left, target.top, outerW, outerH,
                      SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
        Log("SetWindowPos(%s) failed: %lu", DisplayModeName(effective), GetLastError());
        return false;
    }

    RECT client = {};
    if (GetClientRect(hwnd, &client)) {
        dev.presentationW = client.right - client.left;
        dev.presentationH = client.bottom - client.top;
    }
    // [display] start_minimized=1 (race_test): a windowed game goes to the taskbar without
    // taking the focus; nothing is presented while it is minimized, the race runs on.
    char ini[MAX_PATH] = {};
    if (effective == DisplayMode::Windowed && DisplayIniPath(ini, sizeof(ini)) &&
        GetPrivateProfileIntA("display", "start_minimized", 0, ini) != 0) {
        ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
        Log("display: start_minimized -> window minimized");
    }
    dev.minimized = IsIconic(hwnd) != FALSE;
    dev.active = GetForegroundWindow() == hwnd;
    Log("window configured: requested=%s effective=%s client=%dx%d render=%dx%d active=%d",
        DisplayModeName(cfg.mode), DisplayModeName(effective),
        dev.presentationW, dev.presentationH, cfg.renderW, cfg.renderH, dev.active);
    return true;
}

void DX11_DetachWindow(DX11Device& dev) {
    HWND hwnd = dev.hwnd;
    if (dev.windowSubclassed && hwnd && IsWindow(hwnd)) {
        WNDPROC current = (WNDPROC)GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
        if (current == DX11_WindowProc && dev.originalWndProc)
            SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)dev.originalWndProc);
        RemovePropA(hwnd, kDeviceWindowProperty);
    }
    dev.originalWndProc = nullptr;
    dev.windowSubclassed = false;
}

DisplayConfig ResolveDisplayConfig(int iniW, int iniH, const char* iniMode,
                                   int desktopW, int desktopH) {
    DisplayConfig cfg;
    cfg.mode = ParseDisplayMode(iniMode);

    const bool wZero = (iniW <= 0);
    const bool hZero = (iniH <= 0);

    if (wZero && hZero) {
        cfg.renderW = desktopW;
        cfg.renderH = desktopH;
        cfg.usedFallback = true;
        Log("display: width/height absent or zero, using desktop %dx%d", desktopW, desktopH);
    } else if (wZero != hZero) {
        cfg.renderW = desktopW;
        cfg.renderH = desktopH;
        cfg.usedFallback = true;
        Log("display: partial size (%dx%d) rejected, using desktop %dx%d",
            iniW, iniH, desktopW, desktopH);
    } else if (iniW < 320 || iniH < 200) {
        cfg.renderW = desktopW;
        cfg.renderH = desktopH;
        cfg.usedFallback = true;
        Log("display: size %dx%d below minimum (320x200), using desktop %dx%d",
            iniW, iniH, desktopW, desktopH);
    } else if (iniW > 16384 || iniH > 16384) {
        cfg.renderW = desktopW;
        cfg.renderH = desktopH;
        cfg.usedFallback = true;
        Log("display: size %dx%d exceeds 16384 limit, using desktop %dx%d",
            iniW, iniH, desktopW, desktopH);
    } else {
        cfg.renderW = iniW;
        cfg.renderH = iniH;
        cfg.usedFallback = false;
    }

    if (cfg.renderW < 320) cfg.renderW = 640;
    if (cfg.renderH < 200) cfg.renderH = 480;

    Log("display config: render=%dx%d mode=%s%s",
        cfg.renderW, cfg.renderH, DisplayModeName(cfg.mode),
        cfg.usedFallback ? " (fallback)" : "");
    return cfg;
}

// A UTF-8 BOM in front of the first section header makes that section invisible
// to GetPrivateProfile*: it reads as "<BOM>[display]", so every [display] key
// silently falls back to its default while the later sections still work.
// PowerShell writes the BOM by default, so an edited dx11z.ini keeps acquiring
// one. Strip it once per process, before the first read.
static void StripIniBom(const char* path) {
    static bool done = false;
    if (done) return;
    done = true;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return;
    unsigned char bom[3] = {};
    const size_t got = fread(bom, 1, 3, f);
    if (got != 3 || bom[0] != 0xEF || bom[1] != 0xBB || bom[2] != 0xBF) { fclose(f); return; }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    if (size <= 3 || size > (1 << 20)) { fclose(f); return; }
    char* body = (char*)malloc((size_t)size - 3);
    if (!body) { fclose(f); return; }
    fseek(f, 3, SEEK_SET);
    const size_t read = fread(body, 1, (size_t)size - 3, f);
    fclose(f);
    if (read == (size_t)size - 3) {
        FILE* out = nullptr;
        if (fopen_s(&out, path, "wb") == 0 && out) {
            fwrite(body, 1, read, out);
            fclose(out);
            Log("dx11z.ini started with a UTF-8 BOM, which hides its first section "
                "from the INI reader; removed it (%s)", path);
        }
    }
    free(body);
}

static bool DisplayIniPath(char* path, size_t size) {
    if (!WrapperIniPath(path, size))
        return false;
    StripIniBom(path);
    return true;
}

// N1: any section of dx11z.ini (the network redirect lives in [network]).
int ReadIniInt(const char* section, const char* key, int defaultValue) {
    char path[MAX_PATH] = {};
    if (!DisplayIniPath(path, sizeof(path)))
        return defaultValue;
    return (int)GetPrivateProfileIntA(section, key, defaultValue, path);
}

bool ReadIniString(const char* section, const char* key, char* out, int size) {
    char path[MAX_PATH] = {};
    if (!out || size <= 0) return false;
    out[0] = 0;
    if (!DisplayIniPath(path, sizeof(path)))
        return false;
    GetPrivateProfileStringA(section, key, "", out, (DWORD)size, path);
    return out[0] != 0;
}

int ReadDisplayIniInt(const char* key, int defaultValue) {
    char path[MAX_PATH] = {};
    if (!DisplayIniPath(path, sizeof(path)))
        return defaultValue;
    return (int)GetPrivateProfileIntA("display", key, defaultValue, path);
}

float ReadDisplayIniFloat(const char* key, float defaultValue) {
    char path[MAX_PATH] = {};
    char text[64] = {};
    if (!DisplayIniPath(path, sizeof(path)))
        return defaultValue;
    GetPrivateProfileStringA("display", key, "", text, sizeof(text), path);
    if (!text[0]) return defaultValue;
    char* end = nullptr;
    const double value = strtod(text, &end);
    if (end == text) return defaultValue;
    return (float)value;
}

uint32_t ReadDisplayIniHex(const char* key, uint32_t defaultValue) {
    char path[MAX_PATH] = {};
    char text[64] = {};
    if (!DisplayIniPath(path, sizeof(path)))
        return defaultValue;
    GetPrivateProfileStringA("display", key, "", text, sizeof(text), path);
    if (!text[0]) return defaultValue;
    char* end = nullptr;
    const unsigned long value = strtoul(text, &end, 0);   // "0x..." or decimal
    if (end == text) return defaultValue;
    return (uint32_t)value;
}

void ReadDisplayIni(int* outW, int* outH, char* outMode, int modeSize) {
    *outW = 0;
    *outH = 0;
    if (outMode && modeSize > 0) outMode[0] = 0;
    char path[MAX_PATH] = {};
    if (!DisplayIniPath(path, sizeof(path)))
        return;
    *outW = (int)GetPrivateProfileIntA("display", "width", 0, path);
    *outH = (int)GetPrivateProfileIntA("display", "height", 0, path);
    if (outMode && modeSize > 0)
        GetPrivateProfileStringA("display", "mode", "borderless", outMode, modeSize, path);
    Log("dx11z.ini width=%d height=%d mode=%s (%s)", *outW, *outH,
        outMode ? outMode : "(null)", path);
}

static bool SyncGpuSizeFromBackbuffer(DX11Device& dev) {
    if (!dev.swapChain)
        return false;
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(dev.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer)))
        return false;
    D3D11_TEXTURE2D_DESC td = {};
    backBuffer->GetDesc(&td);
    backBuffer->Release();
    if (td.Width == 0 || td.Height == 0)
        return false;
    dev.width = (int)td.Width;
    dev.height = (int)td.Height;
    return true;
}

int DX11_TargetWidth(const DX11Device& dev) {
    return dev.auxiliaryTargetW > 0 ? dev.auxiliaryTargetW : dev.width;
}

int DX11_TargetHeight(const DX11Device& dev) {
    return dev.auxiliaryTargetH > 0 ? dev.auxiliaryTargetH : dev.height;
}

void DX11_SetAuxiliaryTargetSize(DX11Device& dev, int w, int h) {
    // Passing zero closes the auxiliary scope. The configured display size and
    // logical game coordinate space deliberately remain untouched.
    dev.auxiliaryTargetW = w > 0 ? w : 0;
    dev.auxiliaryTargetH = h > 0 ? h : 0;
}

void DX11_ApplyGpuViewport(DX11Device& dev) {
    if (!dev.context)
        return;
    int w = DX11_TargetWidth(dev) > 0 ? DX11_TargetWidth(dev) : 1;
    int h = DX11_TargetHeight(dev) > 0 ? DX11_TargetHeight(dev) : 1;
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)w;
    vp.Height = (float)h;
    vp.MaxDepth = 1.0f;
    dev.context->RSSetViewports(1, &vp);
    D3D11_RECT scissor = {0, 0, (LONG)w, (LONG)h};
    dev.context->RSSetScissorRects(1, &scissor);
}

void DX11_SetLogicalSize(DX11Device& dev, int w, int h) {
    if (w <= 0 || h <= 0)
        return;
    if (w != dev.logicalW || h != dev.logicalH)
        Log("logical size %dx%d -> %dx%d (GPU %dx%d)",
            dev.logicalW, dev.logicalH, w, h, dev.width, dev.height);
    dev.logicalW = w;
    dev.logicalH = h;
    if (dev.context) {
        DX11_ApplyGpuViewport(dev);
        DX11_UpdateScreenCB(dev);
    }
}

bool DX11_GetDesktopMode(HWND hwnd, DXGI_MODE_DESC& mode) {
    MONITORINFOEXW monitor = {};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &monitor)) return false;
    DEVMODEW desktop = {};
    desktop.dmSize = sizeof(desktop);
    if (!EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &desktop) ||
        !desktop.dmPelsWidth || !desktop.dmPelsHeight) return false;
    mode = {};
    mode.Width = desktop.dmPelsWidth;
    mode.Height = desktop.dmPelsHeight;
    mode.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    return true;
}

// PERF8: the slow-launch counter is the device's GPU fence value, read with
// lock cmpxchg8b on every Map. Its address is fixed at device creation; when
// it straddles a cache line each Map costs tens of us instead of ~0.1.
static const double kSlowMapUs = 1.0;
static const int kMaxDeviceRetries = 3;

static double ProbeMapCost(ID3D11Device* device, ID3D11DeviceContext* context) {
    D3D11_BUFFER_DESC bd = {4096, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER,
                            D3D11_CPU_ACCESS_WRITE, 0, 0};
    Microsoft::WRL::ComPtr<ID3D11Buffer> vb;
    if (FAILED(device->CreateBuffer(&bd, nullptr, &vb))) return 0.0;
    auto maps = [&](int n) {
        for (int i = 0; i < n; ++i) {
            D3D11_MAPPED_SUBRESOURCE m;
            D3D11_MAP type = (i & 1) ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD;
            if (SUCCEEDED(context->Map(vb.Get(), 0, type, 0, &m))) {
                *(UINT*)m.pData = (UINT)i;
                context->Unmap(vb.Get(), 0);
            }
        }
    };
    maps(32);   // warm-up: first renames allocate
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    double best = 1e9;
    for (int round = 0; round < 3; ++round) {
        const int kMaps = 200;
        QueryPerformanceCounter(&a);
        maps(kMaps);
        QueryPerformanceCounter(&b);
        best = std::min(best, (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart / kMaps);
    }
    context->ClearState();
    context->Flush();
    return best;
}

bool DX11_Init(DX11Device& dev, HWND hwnd, int width, int height) {
    dev.hwnd = hwnd;
    // Parameters are the game coordinate space (menu 640x480), not the GPU size.
    dev.logicalW = width;
    dev.logicalH = height;
    dev.gameVideoW = width;
    dev.gameVideoH = height;

    DisplayConfig cfg = g_displayConfig;
    if (cfg.renderW <= 0 || cfg.renderH <= 0) {
        int iniW = 0, iniH = 0;
        char iniMode[32] = {};
        ReadDisplayIni(&iniW, &iniH, iniMode, sizeof(iniMode));

        DXGI_MODE_DESC desktopMode = {};
        if (!DX11_GetDesktopMode(hwnd, desktopMode)) {
            desktopMode.Width = 640;
            desktopMode.Height = 480;
            Log("Cannot query desktop mode, using 640x480 fallback");
        }

        cfg = ResolveDisplayConfig(
            iniW, iniH, iniMode, (int)desktopMode.Width, (int)desktopMode.Height);
        g_displayConfig = cfg;
    }
    int gpuW = cfg.renderW;
    int gpuH = cfg.renderH;

    if (!DX11_ConfigureWindow(dev, hwnd, cfg)) {
        Log("DX11 window configuration failed");
        DX11_DetachWindow(dev);
        return false;
    }

    Log("DX11 output %dx%d mode=%s logical %dx%d", gpuW, gpuH,
        DisplayModeName(cfg.mode), width, height);
    dev.width = gpuW;
    dev.height = gpuH;

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = gpuW;
    scd.BufferDesc.Height = gpuH;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    // Let DXGI choose the output refresh rate; do not force a 60 Hz mode.
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    scd.Flags = kSwapChainFlags;

    D3D_FEATURE_LEVEL featureLevel;
    D3D_FEATURE_LEVEL requestedLevels[] = {D3D_FEATURE_LEVEL_11_0};
    UINT createFlags = 0;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    // PERF7: the NVIDIA driver's worker thread syncs with this thread through
    // one 64-bit counter; at device creation it lands on a split cache line
    // in some launches (lock cmpxchg8b = bus lock, the race drops to ~50 fps).
    // 0 = ask the driver for no internal threads (no worker, no shared counter).
    const int driverThreads = ReadDisplayIniInt("driver_threads", DRIVER_THREADS_DEFAULT);
    if (!driverThreads) createFlags |= D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;

    // PERF8: a slow device keeps its memory until a good one exists, so the
    // retry cannot be handed the same fence-counter address again.
    // [display] slow_device_retry: 0 = off, 1 = retry slow devices, 2 = test (device #1 counts as slow).
    const int slowRetry = ReadDisplayIniInt("slow_device_retry", 1);
    ID3D11Device* slowDevices[kMaxDeviceRetries] = {};
    ID3D11DeviceContext* slowContexts[kMaxDeviceRetries] = {};
    HRESULT hr = S_OK;
    for (int attempt = 0;; ++attempt) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
            requestedLevels, 1, D3D11_SDK_VERSION,
            &scd, &dev.swapChain, &dev.device, &featureLevel, &dev.context);
        if (FAILED(hr)) break;
        const double mapUs = ProbeMapCost(dev.device, dev.context);
        const bool slow = mapUs > kSlowMapUs || (slowRetry == 2 && attempt == 0);
        Log("PERF8 map probe: %.3f us/map (device #%d)%s", mapUs, attempt + 1,
            slow ? "  ** SLOW LAUNCH (split fence counter) **" : "");
        if (!slow || !slowRetry || attempt == kMaxDeviceRetries) break;
        dev.swapChain->Release();
        dev.swapChain = nullptr;
        slowDevices[attempt] = dev.device;
        slowContexts[attempt] = dev.context;
        dev.device = nullptr;
        dev.context = nullptr;
    }
    for (int i = 0; i < kMaxDeviceRetries; ++i) {
        if (slowContexts[i]) slowContexts[i]->Release();
        if (slowDevices[i]) slowDevices[i]->Release();
    }

    if (FAILED(hr)) {
        Log("D3D11CreateDeviceAndSwapChain failed: 0x%08X", hr);
        DX11_DetachWindow(dev);
        return false;
    }
    Log("DX11 device created (feature level 0x%X, driver_threads=%d)", featureLevel, driverThreads);

    HRESULT hrBuf = dev.swapChain->ResizeBuffers(
        0, gpuW, gpuH, DXGI_FORMAT_UNKNOWN, kSwapChainFlags);
    if (FAILED(hrBuf))
        Log("ResizeBuffers(%dx%d) failed: 0x%08X", gpuW, gpuH, hrBuf);

    if (!SyncGpuSizeFromBackbuffer(dev)) {
        Log("SyncGpuSizeFromBackbuffer failed, keeping %dx%d", gpuW, gpuH);
        dev.width = gpuW;
        dev.height = gpuH;
    }

    if (!CreateRenderTarget(dev)) {
        Log("CreateRenderTarget failed");
        DX11_Shutdown(dev);
        return false;
    }
    if (!DX11_InitPipeline(dev)) {
        DX11_Shutdown(dev);
        return false;
    }

    dev.initialized = true;
    Log("DX11 init complete GPU %dx%d logical %dx%d",
        dev.width, dev.height, dev.logicalW, dev.logicalH);
    return true;
}

bool DX11_InitPipeline(DX11Device& dev) {
    if (!CompileShaders(dev))     { Log("CompileShaders failed"); return false; }
    if (!Materials_CompileShaders()) Log("MAT7: material shader compile failed (non-fatal)");
    if (!CreateStates(dev))       { Log("CreateStates failed"); return false; }

    // PERF6 probe: cheap binds cost ~0.1 us each; in a slow launch a driver
    // counter touched by every call sits on a split cache line and each one
    // costs tens of us. Logged so a slow launch can be recognised afterwards.
    {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        const int kRounds = 1000;
        for (int i = 0; i < kRounds; ++i) {
            ID3D11SamplerState* s = (i & 1) ? nullptr : dev.samplerLinear;
            dev.context->PSSetSamplers(0, 1, &s);
            dev.context->OMSetBlendState((i & 1) ? dev.blendOpaque : dev.blendAlpha, nullptr, 0xFFFFFFFF);
            dev.context->IASetPrimitiveTopology((i & 1) ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
                                                        : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        }
        dev.context->Flush();
        QueryPerformanceCounter(&b);
        const double us = (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart;
        Log("PERF6 driver probe: %d binds in %.0f us = %.3f us/bind%s", kRounds * 3, us,
            us / (kRounds * 3), us / (kRounds * 3) > 2.0 ? "  ** SLOW LAUNCH (split-lock lottery) **" : "");
    }

    dev.context->OMSetRenderTargets(1, &dev.rtv, dev.dsv);
    DX11_ApplyGpuViewport(dev);

    dev.context->IASetInputLayout(dev.inputLayout);
    dev.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dev.context->VSSetShader(dev.vertexShader, nullptr, 0);
    dev.context->PSSetShader(dev.pixelShaderTex, nullptr, 0);
    dev.boundPS = dev.pixelShaderTex;
    dev.context->RSSetState(dev.rasterState);
    // No occlusion/writes until the game sets depth mode 0x04. The old
    // 0x19/0x2F mapping never enabled depth during menu rendering.
    dev.depthComparison = 7;
    dev.depthWrite = false;
    dev.context->OMSetDepthStencilState(dev.depthStates[7][0], 0);
    // PERF6: a new context has nothing bound; never trust stale tracking.
    dev.boundBlend = nullptr;
    dev.boundSampler0 = nullptr;
    DX11_SetSampler0(dev, dev.samplerLinear);
    dev.context->VSSetConstantBuffers(0, 1, &dev.cbScreen);
    dev.context->PSSetConstantBuffers(0, 1, &dev.cbScreen);

    DX11_SetBlend(dev, dev.blendAlpha);

    DX11_UpdateScreenCB(dev);

    return true;
}

void DX11_FlushScreenCB(DX11Device& dev) {
    if (dev.screenCbDirty) DX11_UpdateScreenCB(dev);
}

void DX11_UpdateScreenCB(DX11Device& dev) {
    dev.screenCbDirty = false;
    if (g_perfOn) Perf_Global(CG_CBSCREEN, 1);
    PerfCallScope perf(PC_CB_SCREEN);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(dev.context->Map(dev.cbScreen, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        float* data = (float*)mapped.pData;
        data[0] = (float)dev.logicalW;
        data[1] = (float)dev.logicalH;
        // World cutouts that still arrive with leftover 10/255 must discard
        // at the stock 0x80 threshold; do not write that back into game state.
        float alphaRef = dev.alphaRef;
        if (dev.hardCutoutActive && alphaRef < 128.0f / 255.0f)
            alphaRef = 128.0f / 255.0f;
        data[2] = alphaRef;
        data[3] = dev.depthBias;
        const uint32_t color = dev.clearColorArgb;
        data[4] = (float)((color >> 16) & 0xFF) / 255.0f;
        data[5] = (float)((color >> 8) & 0xFF) / 255.0f;
        data[6] = (float)(color & 0xFF) / 255.0f;
        data[7] = (float)((color >> 24) & 0xFF) / 255.0f;
        const uint32_t fog = dev.fogColorArgb;
        data[8]  = (float)((fog >> 16) & 0xFF) / 255.0f;
        data[9]  = (float)((fog >> 8) & 0xFF) / 255.0f;
        data[10] = (float)(fog & 0xFF) / 255.0f;
        data[11] = (dev.fogEnabled && dev.fogDensity > 0.0f) ? 1.0f : 0.0f;
        data[12] = dev.fogDensity;
        data[13] = dev.fogScale;
        data[14] = dev.emissive;
        data[15] = dev.emissiveThreshold;
        data[16] = dev.emissivePower;
        data[17] = dev.a2cActive ? 1.0f : 0.0f;
        data[18] = dev.hardCutoutActive ? 1.0f : 0.0f;
        data[19] = dev.sunMaskDraw ? 1.0f : 0.0f;   // MAT16: sun sprite inside a capture
        dev.context->Unmap(dev.cbScreen, 0);
    }
}

static int g_presentCount = 0;

// G7: THRASH_readrect. A small staging copy of the requested region of the
// swap-chain back buffer (the main race target), converted to RGB565 as the
// Glide LFB read would return it. Logical coordinates map through the same
// scale as the clip rectangle. GPU/CPU sync per call; the game calls it once
// per 200 ms for the lens flare.
static ID3D11Texture2D* g_readStaging = nullptr;
static int g_readStagingW = 0, g_readStagingH = 0;

bool DX11_ReadRect565(DX11Device& dev, int x, int y, int w, int h, uint16_t* out) {
    if (!dev.context || !dev.rtv || !out || w <= 0 || h <= 0) return false;
    const int lw = dev.logicalW > 0 ? dev.logicalW : 1;
    const int lh = dev.logicalH > 0 ? dev.logicalH : 1;
    // The current target: the back buffer for the main frame, a cube face
    // during the R1 capture (the game's flare code runs inside the captured
    // passes too and takes its one-time reference there).
    const int gw = DX11_TargetWidth(dev), gh = DX11_TargetHeight(dev);
    // Region on the GPU target, clamped; one GPU pixel per logical pixel.
    int gx0 = (int)((long long)x * gw / lw), gy0 = (int)((long long)y * gh / lh);
    int gx1 = (int)((long long)(x + w) * gw / lw), gy1 = (int)((long long)(y + h) * gh / lh);
    if (gx1 <= gx0) gx1 = gx0 + 1;
    if (gy1 <= gy0) gy1 = gy0 + 1;
    if (gx0 < 0 || gy0 < 0 || gx1 > gw || gy1 > gh) {
        memset(out, 0, (size_t)w * h * 2);
        return false;
    }
    const int rw = gx1 - gx0, rh = gy1 - gy0;
    ID3D11Resource* res = nullptr;
    dev.rtv->GetResource(&res);
    ID3D11Texture2D* backBuf = nullptr;
    if (res) { res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&backBuf); res->Release(); }
    if (!backBuf) return false;
    D3D11_TEXTURE2D_DESC srcDesc = {};
    backBuf->GetDesc(&srcDesc);
    // AA1: the main MSAA target is read through its resolve in the back buffer.
    bool resolved = false;
    if (srcDesc.SampleDesc.Count > 1 && backBuf == dev.msaaColor && dev.backbufferRtv) {
        DX11_ResolveToBackbuffer(dev);
        resolved = true;
        backBuf->Release();
        backBuf = nullptr;
        ID3D11Resource* bbRes = nullptr;
        dev.backbufferRtv->GetResource(&bbRes);
        if (bbRes) { bbRes->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&backBuf); bbRes->Release(); }
        if (!backBuf) return false;
        backBuf->GetDesc(&srcDesc);
    }
    const bool half = srcDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if ((srcDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && !half) || srcDesc.SampleDesc.Count != 1 ||
        (UINT)gx1 > srcDesc.Width || (UINT)gy1 > srcDesc.Height) {
        backBuf->Release();
        return false;
    }
    if (g_readStaging) {
        D3D11_TEXTURE2D_DESC have = {};
        g_readStaging->GetDesc(&have);
        if (have.Format != srcDesc.Format) { g_readStaging->Release(); g_readStaging = nullptr; }
    }
    if (!g_readStaging || g_readStagingW < rw || g_readStagingH < rh) {
        if (g_readStaging) { g_readStaging->Release(); g_readStaging = nullptr; }
        D3D11_TEXTURE2D_DESC td = srcDesc;
        td.Width = (UINT)(rw > 64 ? rw : 64);
        td.Height = (UINT)(rh > 64 ? rh : 64);
        td.MipLevels = 1; td.ArraySize = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        if (FAILED(dev.device->CreateTexture2D(&td, nullptr, &g_readStaging)) || !g_readStaging) {
            backBuf->Release();
            return false;
        }
        g_readStagingW = (int)td.Width;
        g_readStagingH = (int)td.Height;
    }
    // A cube face is one slice of a texture array: copy that subresource.
    UINT srcSub = 0;
    if (!resolved) {
        D3D11_RENDER_TARGET_VIEW_DESC rvd = {};
        dev.rtv->GetDesc(&rvd);
        if (rvd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY)
            srcSub = D3D11CalcSubresource(rvd.Texture2DArray.MipSlice, rvd.Texture2DArray.FirstArraySlice, srcDesc.MipLevels);
        else if (rvd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D)
            srcSub = D3D11CalcSubresource(rvd.Texture2D.MipSlice, 0, srcDesc.MipLevels);
    }
    D3D11_BOX box = {(UINT)gx0, (UINT)gy0, 0, (UINT)gx1, (UINT)gy1, 1};
    dev.context->CopySubresourceRegion(g_readStaging, 0, 0, 0, 0, backBuf, srcSub, &box);
    backBuf->Release();
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(dev.context->Map(g_readStaging, 0, D3D11_MAP_READ, 0, &mapped))) return false;
    for (int j = 0; j < h; j++) {
        const int sy = (int)((long long)j * rh / h);
        const uint8_t* row = (const uint8_t*)mapped.pData + sy * mapped.RowPitch;
        for (int i = 0; i < w; i++) {
            const int sx = (int)((long long)i * rw / w);
            uint8_t rgb[3];
            if (half) {
                const uint16_t* hp = (const uint16_t*)(row + sx * 8);
                for (int c = 0; c < 3; c++) {
                    // half -> saturated byte (readrect is a 565 compare; HDR
                    // overshoot saturates like the 8-bit target would)
                    const uint16_t hv = hp[c];
                    const int e = (hv >> 10) & 0x1F, m = hv & 0x3FF;
                    float v = (hv & 0x8000) ? 0.0f : (e == 0 ? (float)m / 16777216.0f
                              : (e == 31 ? 1.0f : (1.0f + (float)m / 1024.0f) * (float)std::ldexp(1.0, e - 15)));
                    rgb[c] = (uint8_t)(v >= 1.0f ? 255 : (int)(v * 255.0f + 0.5f));
                }
            } else {
                const uint8_t* px = row + sx * 4;   // R8G8B8A8
                rgb[0] = px[0]; rgb[1] = px[1]; rgb[2] = px[2];
            }
            out[j * w + i] = (uint16_t)(((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
        }
    }
    dev.context->Unmap(g_readStaging, 0);
    return true;
}

// A null path only logs a pixel digest, without writing an image to disk.
static void ReadBackbuffer(DX11Device& dev, const char* path) {
    ID3D11Texture2D* backBuf = nullptr;
    dev.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuf);
    if (!backBuf) return;

    D3D11_TEXTURE2D_DESC td;
    backBuf->GetDesc(&td);

    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    dev.device->CreateTexture2D(&sd, nullptr, &staging);
    if (!staging) { backBuf->Release(); return; }

    dev.context->CopyResource(staging, backBuf);
    backBuf->Release();

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(dev.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        int w = (int)td.Width, h = (int)td.Height;
        if (!path) {
            uint32_t hash = 2166136261u;
            unsigned nonBlack = 0;
            for (int y = 0; y < h; y++) {
                const uint8_t* row = (const uint8_t*)mapped.pData + y * mapped.RowPitch;
                for (int x = 0; x < w; x++) {
                    const uint8_t* rgb = row + x * 4;
                    if (rgb[0] || rgb[1] || rgb[2]) ++nonBlack;
                    for (int channel = 0; channel < 3; channel++)
                        hash = (hash ^ rgb[channel]) * 16777619u;
                }
            }
            Log("Startup backbuffer #%d %dx%d RGB-FNV1a=%08X nonBlack=%u",
                g_presentCount, w, h, hash, nonBlack);
        }
        FILE* f = path ? fopen(path, "wb") : nullptr;
        if (f) {
            int rowBytes = w * 3;
            int padRow = (rowBytes + 3) & ~3;
            int imgSize = padRow * h;
            uint8_t bmpH[54] = {};
            *(uint16_t*)&bmpH[0] = 0x4D42;
            *(uint32_t*)&bmpH[2] = 54 + imgSize;
            *(uint32_t*)&bmpH[10] = 54;
            *(uint32_t*)&bmpH[14] = 40;
            *(int32_t*)&bmpH[18] = w;
            *(int32_t*)&bmpH[22] = -h;
            *(uint16_t*)&bmpH[26] = 1;
            *(uint16_t*)&bmpH[28] = 24;
            *(uint32_t*)&bmpH[34] = imgSize;
            fwrite(bmpH, 1, 54, f);
            for (int y = 0; y < h; y++) {
                uint8_t* row = (uint8_t*)mapped.pData + y * mapped.RowPitch;
                for (int x = 0; x < w; x++) {
                    uint8_t r = row[x*4+0], g = row[x*4+1], b = row[x*4+2];
                    fwrite(&b, 1, 1, f);
                    fwrite(&g, 1, 1, f);
                    fwrite(&r, 1, 1, f);
                }
                uint8_t pad[3] = {};
                int padBytes = padRow - rowBytes;
                if (padBytes > 0) fwrite(pad, 1, padBytes, f);
            }
            fclose(f);
            Log("Dumped backbuffer to %s (%dx%d)", path, w, h);
        }
        dev.context->Unmap(staging, 0);
    }
    staging->Release();
}

// CP1 diagnostics: pixels the material shader's debug view 8 marked
// (exact magenta = NaN / Inf shading, exact cyan = car paint shaded black).
static void CountDebugPixels(DX11Device& dev) {
    ID3D11Texture2D* backBuf = nullptr;
    dev.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuf);
    if (!backBuf) return;
    D3D11_TEXTURE2D_DESC sd;
    backBuf->GetDesc(&sd);
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    dev.device->CreateTexture2D(&sd, nullptr, &staging);
    if (!staging) { backBuf->Release(); return; }
    dev.context->CopyResource(staging, backBuf);
    backBuf->Release();
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(dev.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        unsigned count[2] = {0, 0};
        int x0[2] = {1 << 30, 1 << 30}, x1[2] = {-1, -1}, y0[2] = {1 << 30, 1 << 30}, y1[2] = {-1, -1};
        for (int y = 0; y < (int)sd.Height; y++) {
            const uint8_t* row = (const uint8_t*)mapped.pData + y * mapped.RowPitch;
            for (int x = 0; x < (int)sd.Width; x++) {
                const uint8_t* c = row + x * 4;
                const int k = (c[0] == 255 && c[1] == 0 && c[2] == 255) ? 0 : (c[0] == 0 && c[1] == 255 && c[2] == 255) ? 1 : -1;
                if (k < 0) continue;
                ++count[k];
                x0[k] = x < x0[k] ? x : x0[k]; x1[k] = x > x1[k] ? x : x1[k];
                y0[k] = y < y0[k] ? y : y0[k]; y1[k] = y > y1[k] ? y : y1[k];
            }
        }
        if (count[0] || count[1])
            Log("CP1 debug #%d: %u NaN px (x %d..%d, y %d..%d), %u black paint px (x %d..%d, y %d..%d)",
                g_presentCount, count[0], x0[0], x1[0], y0[0], y1[0], count[1], x0[1], x1[1], y0[1], y1[1]);
        dev.context->Unmap(staging, 0);
    }
    staging->Release();
}

void DX11_ResolveToBackbuffer(DX11Device& dev) {
    if (!dev.context || dev.msaaSamples <= 1 || !dev.msaaColor || !dev.backbufferRtv) return;
    ID3D11Resource* bb = nullptr;
    dev.backbufferRtv->GetResource(&bb);
    if (!bb) return;
    dev.context->ResolveSubresource(bb, 0, dev.msaaColor, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    bb->Release();
}

// PERF1: [display] vsync=0 presents without waiting for the display (the
// frame time then shows the real cost instead of the 16.7 ms steps).
static UINT g_presentInterval = 1;
void DX11_SetPresentInterval(int interval) { g_presentInterval = interval > 0 ? 1u : 0u; }

void DX11_Present(DX11Device& dev) {
    if (dev.swapChain && !dev.minimized) {
        g_presentCount++;
        Perf_GpuMark(GZ_RESOLVE, false);
        DX11_ResolveToBackbuffer(dev);
        Perf_GpuMark(GZ_RESOLVE, true);
        // Opt-in readback of the six software-rendered startup frames. Used to
        // compare the GPU result against loadscrn.fsh independently of shaders.
        char trace[2] = {};
        if (g_presentCount <= 6 &&
            GetEnvironmentVariableA("DX11_TRACE_STARTUP", trace, sizeof(trace)) == 1 &&
            trace[0] == '1') {
            ReadBackbuffer(dev, nullptr);
        }
        // Frame 100 as a BMP (MAC1 check that a picture arrives). Opt-in since
        // HITCH1: the readback + unbuffered write stalled the menu for ~300 ms.
        char frame100[2] = {};
        if (g_presentCount == 100 &&
            GetEnvironmentVariableA("DX11_FRAME100", frame100, sizeof(frame100)) == 1 && frame100[0] == '1') {
            ReadBackbuffer(dev, "dx11z_frame100.bmp");
        }
        if (Materials_DebugView() == 8 && (g_presentCount % 30) == 0) CountDebugPixels(dev);
        Perf_FrameEnd();
        HRESULT hr = dev.swapChain->Present(g_presentInterval, 0);
        if (g_presentCount <= 5 || FAILED(hr)) {
            RECT wr = {};
            GetWindowRect(dev.hwnd, &wr);
            BOOL visible = IsWindowVisible(dev.hwnd);
            Log("Present #%d hr=0x%08X visible=%d rect=(%d,%d,%d,%d)",
                g_presentCount, hr, visible, wr.left, wr.top, wr.right, wr.bottom);
        }
    }
}

void DX11_Clear(DX11Device& dev, bool depthOnly) {
    if (!dev.context) return;
    const int targetW = DX11_TargetWidth(dev);
    const int targetH = DX11_TargetHeight(dev);
    D3D11_RECT clip = {0, 0, targetW, targetH};
    UINT count = 1;
    dev.context->RSGetScissorRects(&count, &clip);
    clip.left = std::max(0L, clip.left);
    clip.top = std::max(0L, clip.top);
    clip.right = std::min((LONG)targetW, clip.right);
    clip.bottom = std::min((LONG)targetH, clip.bottom);
    if (clip.left >= clip.right || clip.top >= clip.bottom) return;

    if (clip.left != 0 || clip.top != 0 || clip.right != targetW || clip.bottom != targetH) {
        using Microsoft::WRL::ComPtr;
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11InputLayout> layout;
        ComPtr<ID3D11DepthStencilState> depth;
        ComPtr<ID3D11RasterizerState> raster;
        ComPtr<ID3D11Buffer> psConstantBuffer;
        D3D11_PRIMITIVE_TOPOLOGY topology;
        UINT stencil;
        D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        UINT viewportCount = ARRAYSIZE(viewports);
        auto* ctx = dev.context;
        ctx->VSGetShader(&vs, nullptr, nullptr);
        ctx->PSGetShader(&ps, nullptr, nullptr);
        ctx->IAGetInputLayout(&layout);
        ctx->IAGetPrimitiveTopology(&topology);
        ID3D11BlendState* blend = dev.boundBlend;   // PERF5: no OMGetBlendState
        ctx->OMGetDepthStencilState(&depth, &stencil);
        ctx->RSGetState(&raster);
        ctx->RSGetViewports(&viewportCount, viewports);
        ctx->PSGetConstantBuffers(0, 1, &psConstantBuffer);

        D3D11_VIEWPORT full = {0, 0, (float)targetW, (float)targetH, 0, 1};
        ctx->RSSetViewports(1, &full);
        ctx->RSSetState(dev.rasterState);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(dev.clearVS, nullptr, 0);
        ctx->PSSetShader(dev.clearPS, nullptr, 0);
        // The clear shader is also used for camera sub-rectangles (mirror).
        // Refresh b0 after state 3 and bind it explicitly before the draw.
        DX11_UpdateScreenCB(dev);
        ctx->PSSetConstantBuffers(0, 1, &dev.cbScreen);
        // MAT14d: a clear inside a cubemap capture keeps the alpha mask.
        ID3D11BlendState* clearBlend = depthOnly ? dev.blendNoColor
            : (dev.captureAlphaMask && dev.blendOpaqueRgb ? dev.blendOpaqueRgb : dev.blendOpaque);
        DX11_SetBlend(dev, clearBlend);
        ctx->OMSetDepthStencilState(dev.depthStates[7][1], 0);
        ctx->Draw(3, 0);

        ctx->RSSetViewports(viewportCount, viewports);
        ctx->RSSetState(raster.Get());
        ctx->IASetInputLayout(layout.Get());
        ctx->IASetPrimitiveTopology(topology);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11Buffer* restoredConstantBuffer = psConstantBuffer.Get();
        ctx->PSSetConstantBuffers(0, 1, &restoredConstantBuffer);
        DX11_SetBlend(dev, blend);
        ctx->OMSetDepthStencilState(depth.Get(), stencil);
        return;
    }
    const uint32_t color = dev.clearColorArgb;
    float clearColor[4] = {
        (float)((color >> 16) & 0xFF) / 255.0f,
        (float)((color >> 8) & 0xFF) / 255.0f,
        (float)(color & 0xFF) / 255.0f,
        (float)((color >> 24) & 0xFF) / 255.0f
    };
    // MAT19: inside a cubemap capture the alpha is the MAT16 sun mask. The
    // game clears every face at the start of its traversal with alpha 0xFF,
    // which marked the whole cube as sun and dimmed all reflections to
    // sun_reflection; clear it to "no sun" instead.
    if (dev.captureAlphaMask) clearColor[3] = 0.0f;
    if (!depthOnly && dev.rtv) dev.context->ClearRenderTargetView(dev.rtv, clearColor);
    if (dev.dsv) dev.context->ClearDepthStencilView(dev.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
}

void DX11_ResizeIfNeeded(DX11Device& dev, int w, int h) {
    if (w <= 0 || h <= 0) return;

    // GPU-only. Game coordinate space is DX11_SetLogicalSize / THRASH_setvideomode.
    if (!dev.swapChain || !dev.context) {
        dev.width = w;
        dev.height = h;
        return;
    }

    if (w == dev.width && h == dev.height) {
        DX11_ApplyGpuViewport(dev);
        return;
    }

    BOOL fullscreen = FALSE;
    dev.swapChain->GetFullscreenState(&fullscreen, nullptr);

    dev.context->OMSetRenderTargets(0, nullptr, nullptr);
    SafeRelease(dev.rtv);
    SafeRelease(dev.backbufferRtv);
    SafeRelease(dev.msaaColor);
    SafeRelease(dev.dsv);
    SafeRelease(dev.depthSrv);
    SafeRelease(dev.stencilSrv);
    SafeRelease(dev.depthBuffer);

    if (fullscreen) {
        DXGI_MODE_DESC mode = {};
        mode.Width = (UINT)w;
        mode.Height = (UINT)h;
        mode.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        HRESULT hrTarget = dev.swapChain->ResizeTarget(&mode);
        if (FAILED(hrTarget))
            Log("ResizeTarget(%dx%d) failed: 0x%08X", w, h, hrTarget);
    }

    HRESULT hr = dev.swapChain->ResizeBuffers(
        0, w, h, DXGI_FORMAT_UNKNOWN, kSwapChainFlags);
    if (FAILED(hr)) {
        Log("ResizeBuffers(%dx%d) failed: 0x%08X", w, h, hr);
        CreateRenderTarget(dev);
        if (dev.rtv)
            dev.context->OMSetRenderTargets(1, &dev.rtv, dev.dsv);
        return;
    }

    Log("DX11 GPU resize %dx%d -> %dx%d (logical %dx%d)",
        dev.width, dev.height, w, h, dev.logicalW, dev.logicalH);
    dev.width = w;
    dev.height = h;
    if (!CreateRenderTarget(dev)) {
        Log("CreateRenderTarget after GPU resize failed (%dx%d)", w, h);
        return;
    }

    dev.context->OMSetRenderTargets(1, &dev.rtv, dev.dsv);
    DX11_ApplyGpuViewport(dev);
}

void DX11_Shutdown(DX11Device& dev) {
    if (dev.swapChain) dev.swapChain->SetFullscreenState(FALSE, nullptr);
    for (auto& comparisons : dev.depthStates)
        for (auto& state : comparisons) SafeRelease(state);
    for (auto& comparisons : dev.depthStatesMark)
        for (auto& state : comparisons) SafeRelease(state);
    SafeRelease(dev.rasterState);
    SafeRelease(dev.blendOpaque);
    SafeRelease(dev.blendNoColor);
    SafeRelease(dev.blendAlpha);
    State_ReleaseCaches();
    SafeRelease(g_readStaging);
    g_readStagingW = g_readStagingH = 0;
    for (auto*& b : dev.blendModes) SafeRelease(b);
    SafeRelease(dev.samplerPoint);
    SafeRelease(dev.samplerLinear);
    SafeRelease(dev.cbScreen);
    SafeRelease(dev.cbReflect);
    SafeRelease(dev.inputLayout);
    SafeRelease(dev.pixelShaderFlat);
    SafeRelease(dev.pixelShaderTex);
    SafeRelease(dev.vertexShader);
    SafeRelease(dev.clearPS);
    SafeRelease(dev.clearVS);
    SafeRelease(dev.reflectPS);
    SafeRelease(dev.reflectVS);
    Materials_ReleaseShaders();
    SafeRelease(dev.dsv);
    SafeRelease(dev.depthSrv);
    SafeRelease(dev.stencilSrv);
    SafeRelease(dev.depthBuffer);
    SafeRelease(dev.rtv);
    SafeRelease(dev.backbufferRtv);
    SafeRelease(dev.msaaColor);
    for (auto*& b : dev.blendModesA2C) SafeRelease(b);
    for (auto*& b : dev.blendModesRgb) SafeRelease(b);
    for (auto*& b : dev.blendModesA2CRgb) SafeRelease(b);
    for (auto*& b : dev.blendModesClearA) SafeRelease(b);
    for (auto*& b : dev.blendModesA2CClearA) SafeRelease(b);
    SafeRelease(dev.blendSunMask);
    SafeRelease(dev.blendOpaqueRgb);
    SafeRelease(dev.swapChain);
    SafeRelease(dev.context);
    SafeRelease(dev.device);
    dev.initialized = false;
    DX11_DetachWindow(dev);
    Log("DX11 shutdown");
}
