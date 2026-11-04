// nfs5-web WEB2: the THRASH driver on dx11z's D3D11 renderer (native build).
//
// runtime/thrash_hle.c owns the guest-facing side of the driver (the about
// struct, video mode list, window and lock descriptors in guest memory, the
// state the game reads back) and forwards the drawing side here: state, the
// device, textures, primitives, clears, page flips. The renderer core is
// _mod/wrapper's dx11_device / draw / state / texture (renderer/dx11z/, the
// mod's features stubbed in feature_stubs.cpp); this file replaces the
// wrapper's thrash_exports.cpp for that core:
//  - guest addresses become host pointers (g_mem + a); vertex and index
//    arrays have no pointers inside, texture handles are slot numbers;
//  - the game's HWND is the runtime's placeholder, so the swap chain gets a
//    real host window, pumped on every page flip (keys go to host_dik_state
//    for the DirectInput keyboard in pu_com.c).
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dx11z/thrash_api.h"
#include "dx11z/dx11_device.h"
#include "dx11z/dx11_texture.h"
#include "dx11z/dx11_state.h"
#include "dx11z/dx11_draw.h"
#include "dx11z/log.h"

extern "C" uint8_t* g_mem;
extern DX11Device g_dx;

void Tex_UpdateRect(void* handle, int mipLevel, int x, int y, int rw, int rh, int pitch, void* data, int format);

static inline void* G(uint32_t a) { return a ? (void*)(g_mem + a) : nullptr; }
static inline THRASH_Vertex* V(uint32_t a) { return (THRASH_Vertex*)G(a); }

const uint8_t* R_GuestPtrAt(uint32_t guestAddr) {
    const uint32_t p = *(const uint32_t*)(g_mem + guestAddr);
    return p ? g_mem + p : nullptr;
}

// ---------------------------------------------------------------- host window

static HWND g_win;
static DWORD g_winThread;        // the thread that owns the window (its messages)
static int g_currentWindow;
static long g_frames;
static uint8_t g_dik[256];          // DirectInput keyboard state (0x80 = down)
static volatile LONG g_quit;
// DirectInput buffered keyboard data (GetDeviceData): DIK, 0x80/0, time, sequence
struct KeyEvent { uint32_t ofs, data, ts, seq; };
static KeyEvent g_kq[256];
static unsigned g_kq_head, g_kq_tail, g_kq_seq;
// window messages for the guest's own window procedure (the menus read
// WM_KEYDOWN/WM_CHAR, not DirectInput): moved into its queue by kbd_pump
struct WinMsg { uint32_t m, w, l; };
static WinMsg g_wq[256];
static unsigned g_wq_head, g_wq_tail;
static void PushMsg(uint32_t m, uint32_t w, uint32_t l) {
    if (g_wq_tail - g_wq_head >= 256) return;
    g_wq[g_wq_tail++ & 255] = {m, w, l};
}
// the cursor in the game's logical coordinates (GetCursorPos, mouse messages)
static int g_mx = 320, g_my = 240;
static uint32_t MapMouse(LPARAM l);
static void PushKey(unsigned dik, bool down) {
    if (g_kq_tail - g_kq_head >= 256) g_kq_head++;          // full: drop the oldest
    g_kq[g_kq_tail++ & 255] = {dik, down ? 0x80u : 0u, GetTickCount(), ++g_kq_seq};
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE:
        InterlockedExchange(&g_quit, 1);
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        unsigned sc = (unsigned)((l >> 16) & 0xFF) | ((l & (1 << 24)) ? 0x80u : 0u);
        if (sc == 0x45 && w == VK_PAUSE) sc = 0xC5;          // DIK_PAUSE
        if (w == VK_NUMLOCK) sc = 0x45;                       // DIK_NUMLOCK
        const bool down = (m == WM_KEYDOWN || m == WM_SYSKEYDOWN);
        if (!(down && (l & (1 << 30)))) PushKey(sc & 0xFF, down);   // no auto-repeat (DirectInput has none)
        g_dik[sc & 0xFF] = down ? 0x80 : 0;
        if (m == WM_SYSKEYDOWN && w == VK_F4) InterlockedExchange(&g_quit, 1);
        PushMsg(m, (uint32_t)w, (uint32_t)l);
        if (m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) break;    // DefWindowProc: WM_SYSCHAR etc.
        return 0; }
    case WM_CHAR:
        PushMsg(m, (uint32_t)w, (uint32_t)l);
        return 0;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_LBUTTONDBLCLK: {
        const uint32_t lp = MapMouse(l);
        if (m == WM_MOUSEMOVE && g_wq_tail != g_wq_head && g_wq[(g_wq_tail - 1) & 255].m == WM_MOUSEMOVE)
            g_wq[(g_wq_tail - 1) & 255].l = lp;               // coalesce moves
        else PushMsg(m, (uint32_t)w, lp);
        if (m == WM_LBUTTONDOWN) SetCapture(h); else if (m == WM_LBUTTONUP) ReleaseCapture();
        return 0; }
    case WM_MOUSEWHEEL:
        PushMsg(m, (uint32_t)w, MapMouse(l));
        return 0;
    case WM_KILLFOCUS:
        memset(g_dik, 0, sizeof g_dik);
        break;
    case WM_SYSCOMMAND:
        if ((w & 0xFFF0) == SC_KEYMENU) return 0;             // Alt alone: no menu loop
        break;
    }
    return DefWindowProcA(h, m, w, l);
}

static void PumpHostWindow() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (g_quit) {
        fprintf(stderr, "[dx11] window closed after %ld frames\n", g_frames);
        fflush(stderr);
        ExitProcess(0);
    }
}

// client pixels -> the game's logical screen (640x480 in the frontend);
// the picture fills the client area (DX11 output stretched to it)
static uint32_t MapMouse(LPARAM l) {
    RECT rc; GetClientRect(g_win, &rc);
    const int cw = rc.right > 0 ? rc.right : 1, ch = rc.bottom > 0 ? rc.bottom : 1;
    const int lw = g_dx.logicalW > 0 ? g_dx.logicalW : 640, lh = g_dx.logicalH > 0 ? g_dx.logicalH : 480;
    int x = (short)LOWORD(l) * lw / cw, y = (short)HIWORD(l) * lh / ch;
    x = x < 0 ? 0 : x >= lw ? lw - 1 : x; y = y < 0 ? 0 : y >= lh ? lh - 1 : y;
    g_mx = x; g_my = y;
    return (uint32_t)(x & 0xFFFF) | ((uint32_t)y << 16);
}

static HWND CreateHostWindow(int w, int h) {
    WNDCLASSA wc = {};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "nfs5web";
    RegisterClassA(&wc);
    RECT r = {0, 0, w, h};
    const DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowExA(0, "nfs5web", "Porsche Unleashed (nfs5-web)", style,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOWNORMAL);
        UpdateWindow(hwnd);
    }
    return hwnd;
}

// The renderer's INI: $DX11Z_INI, else out/dx11z.ini next to the program
// (written with windowed defaults the first time).
static void PrepareIni() {
    char path[MAX_PATH];
    if (GetEnvironmentVariableA("DX11Z_INI", path, MAX_PATH)) return;
    if (!GetModuleFileNameA(nullptr, path, MAX_PATH)) return;
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    strcpy(slash + 1, "dx11z.ini");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = fopen(path, "w")) {
            fputs("; nfs5-web native renderer (dx11z core). Written once with defaults.\n"
                  "[display]\nmode=windowed\nwidth=1280\nheight=960\nvsync=1\nmsaa=1\n", f);
            fclose(f);
        }
    }
    SetEnvironmentVariableA("DX11Z_INI", path);
}

// ---------------------------------------------------------------- driver side

extern "C" {

// THRASH_init: the device on a new host window; the game's own size states.
int r_init(int w, int h) {
    PrepareIni();
    char dir[MAX_PATH];
    if (!GetEnvironmentVariableA("DX11Z_OUT", dir, MAX_PATH)) {
        if (GetModuleFileNameA(nullptr, dir, MAX_PATH)) {
            if (char* s = strrchr(dir, '\\')) *s = 0;
            SetEnvironmentVariableA("DX11Z_OUT", dir);
        }
    }
    LogInit();
    State_Init();
    if (w <= 0) w = 640;
    if (h <= 0) h = 480;
    int iw = 0, ih = 0; char mode[32] = {};
    ReadDisplayIni(&iw, &ih, mode, sizeof mode);
    if (!g_win) { g_win = CreateHostWindow(iw > 0 ? iw : 1280, ih > 0 ? ih : 960); g_winThread = GetCurrentThreadId(); }
    if (!g_win) { fprintf(stderr, "[dx11] CreateWindow failed (%lu)\n", GetLastError()); return 0; }
    DX11_ConfigureMsaa(ReadDisplayIniInt("msaa", 1), ReadDisplayIniInt("alpha_to_coverage", 1) != 0);
    if (!DX11_Init(g_dx, g_win, w, h)) { fprintf(stderr, "[dx11] DX11_Init failed (see dx11z.log)\n"); return 0; }
    Tex_Init();
    Draw_Init();
    // the states dx7z.dll sets itself (wrapper THRASH_init)
    State_SetDirect(0x38, 0);
    State_SetDirect(0x1E, 32);
    State_SetDirect(0x44, 0);
    State_SetDirect(0x46, 3);
    State_SetDirect(0x43, 0x3F800000);
    State_SetDirect(0x2E, 0x3F800000);
    State_SetDirect(0x17, 0x3F800000);
    State_SetDirect(0x3B, 0);
    State_SetDirect(0x3E, 1);
    State_SetDirect(THRASH_STATE_SCREEN_WIDTH, w);
    State_SetDirect(THRASH_STATE_SCREEN_HEIGHT, h);
    DX11_SetPresentInterval(ReadDisplayIniInt("vsync", 1));
    fprintf(stderr, "[dx11] init: GPU %dx%d, logical %dx%d\n", g_dx.width, g_dx.height, g_dx.logicalW, g_dx.logicalH);
    PumpHostWindow();
    return 1;
}

void r_restore(void) {
    Draw_Shutdown();
    Tex_Shutdown();
    DX11_Shutdown(g_dx);
}

void r_setvideomode(int w, int h, int bpp) {
    State_SetRaceVideoMode(bpp == 32);
    State_SetDirect(THRASH_STATE_SCREEN_WIDTH, w);
    State_SetDirect(THRASH_STATE_SCREEN_HEIGHT, h);
    g_dx.gameVideoW = w;
    g_dx.gameVideoH = h;
    if (g_dx.initialized) DX11_SetLogicalSize(g_dx, w, h);
}

void r_setstate(int id, int value) { State_Set(id, value); }
int  r_getstate(int id) { return State_Get(id); }
void r_window(int idx) { if (idx >= 0 && idx <= 7) g_currentWindow = idx; }
void r_clearwindow(void) { DX11_Clear(g_dx, g_currentWindow == 3); }
void r_flushwindow(void) { if (g_dx.context) g_dx.context->Flush(); }

void r_clip(int x1, int y1, int x2, int y2) { if (g_dx.context) State_SetClip(x1, y1, x2, y2); }

// R_SHOT=<frame>[,<frame>...]: the frame read back before it is presented,
// as text statistics on stderr (no image needed to tell a picture from a
// black or flat screen) and as shot_<frame>.bmp in DX11Z_OUT.
static void ShotIfWanted() {
    static int s_init, s_n; static long s_at[16];
    if (!s_init) {
        s_init = 1;
        if (const char* e = getenv("R_SHOT"))
            for (const char* p = e; *p && s_n < 16; ) { s_at[s_n++] = strtol(p, (char**)&p, 10); while (*p == ',') p++; }
    }
    bool want = false;
    for (int i = 0; i < s_n; i++) if (s_at[i] == g_frames) want = true;
    if (!want) return;
    const int w = g_dx.logicalW, h = g_dx.logicalH;
    if (w <= 0 || h <= 0) return;
    uint16_t* px = new uint16_t[(size_t)w * h];
    if (!DX11_ReadRect565(g_dx, 0, 0, w, h, px)) { fprintf(stderr, "[shot] frame %ld: readback failed\n", g_frames); delete[] px; return; }
    // statistics: non-black share, distinct colours, mean brightness per 3x3 cell
    static uint8_t seen[65536 / 8]; memset(seen, 0, sizeof seen);
    long nonblack = 0, distinct = 0; double cell[9] = {}; long cnt[9] = {};
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint16_t c = px[(size_t)y * w + x];
        if (c) nonblack++;
        if (!(seen[c >> 3] & (1 << (c & 7)))) { seen[c >> 3] |= (uint8_t)(1 << (c & 7)); distinct++; }
        double l = (((c >> 11) & 31) * 255 / 31 * 0.299) + (((c >> 5) & 63) * 255 / 63 * 0.587) + ((c & 31) * 255 / 31 * 0.114);
        int k = (y * 3 / h) * 3 + (x * 3 / w); cell[k] += l; cnt[k]++;
    }
    fprintf(stderr, "[shot] frame %ld %dx%d: %.1f%% not black, %ld colours, brightness by thirds:", g_frames, w, h,
            100.0 * nonblack / ((double)w * h), distinct);
    for (int k = 0; k < 9; k++) fprintf(stderr, "%s%.0f", k % 3 ? " " : (k ? " | " : " "), cnt[k] ? cell[k] / cnt[k] : 0.0);
    fprintf(stderr, "\n");
    char dir[MAX_PATH] = ".", path[MAX_PATH];
    GetEnvironmentVariableA("DX11Z_OUT", dir, MAX_PATH);
    snprintf(path, sizeof path, "%s\\shot_%ld.bmp", dir, g_frames);
    if (FILE* f = fopen(path, "wb")) {
        const int stride = (w * 3 + 3) & ~3;
        BITMAPFILEHEADER fh = {}; BITMAPINFOHEADER ih = {};
        fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + stride * h;
        ih.biSize = sizeof ih; ih.biWidth = w; ih.biHeight = -h; ih.biPlanes = 1; ih.biBitCount = 24;
        fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
        uint8_t* row = new uint8_t[stride]();
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint16_t c = px[(size_t)y * w + x];
                row[x * 3 + 0] = (uint8_t)((c & 31) * 255 / 31);
                row[x * 3 + 1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
                row[x * 3 + 2] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
            }
            fwrite(row, stride, 1, f);
        }
        delete[] row;
        fclose(f);
        fprintf(stderr, "[shot] -> %s\n", path);
    }
    delete[] px;
}

// R_KEYS=<frame>:<DIK>[,...]: scripted key presses for autotests (held 4 frames),
// e.g. R_KEYS=700:0x1C,900:0xD0 = Enter at frame 700, Down at 900.
static void ScriptedKeys() {
    static int s_init, s_n; static long s_f[64]; static unsigned s_k[64];
    if (!s_init) {
        s_init = 1;
        if (const char* e = getenv("R_KEYS"))
            for (const char* p = e; *p && s_n < 64; ) {
                char* q; long f = strtol(p, &q, 10); if (*q != ':') break;
                unsigned k = (unsigned)strtoul(q + 1, &q, 0);
                s_f[s_n] = f; s_k[s_n++] = k & 0xFF; p = q; while (*p == ',') p++;
            }
    }
    for (int i = 0; i < s_n; i++) {
        if (g_frames == s_f[i] || g_frames == s_f[i] + 4) {
            const bool dn = g_frames == s_f[i];
            UINT vk = MapVirtualKeyA(s_k[i] & 0x7F, MAPVK_VSC_TO_VK_EX);
            if (s_k[i] & 0x80) switch (s_k[i]) { case 0xC8: vk = VK_UP; break; case 0xD0: vk = VK_DOWN; break;
                case 0xCB: vk = VK_LEFT; break; case 0xCD: vk = VK_RIGHT; break; }
            uint32_t lp = 1u | ((s_k[i] & 0x7Fu) << 16) | ((s_k[i] & 0x80u) ? 1u << 24 : 0u) | (dn ? 0u : 0xC0000000u);
            PushMsg(dn ? WM_KEYDOWN : WM_KEYUP, vk, lp);
            const UINT ch = MapVirtualKeyA(vk, MAPVK_VK_TO_CHAR) & 0x7FFF;
            if (dn && ch) PushMsg(WM_CHAR, ch, lp);
        }
        if (g_frames == s_f[i]) { PushKey(s_k[i], true); g_dik[s_k[i]] = 0x80;
                                   fprintf(stderr, "[keys] frame %ld: DIK 0x%02X down\n", g_frames, s_k[i]); }
        if (g_frames == s_f[i] + 4) { PushKey(s_k[i], false); g_dik[s_k[i]] = 0; }
    }
}

// R_CLICK=<frame>:<x>:<y>[,...]: scripted left clicks in logical coordinates
static void ScriptedClicks() {
    static int s_init, s_n; static long s_f[64]; static int s_x[64], s_y[64];
    if (!s_init) {
        s_init = 1;
        if (const char* e = getenv("R_CLICK"))
            for (const char* p = e; *p && s_n < 64; ) {
                char* q; long f = strtol(p, &q, 10); if (*q != ':') break;
                int x = (int)strtol(q + 1, &q, 10); if (*q != ':') break;
                int y = (int)strtol(q + 1, &q, 10);
                s_f[s_n] = f; s_x[s_n] = x; s_y[s_n++] = y; p = q; while (*p == ',') p++;
            }
    }
    for (int i = 0; i < s_n; i++) {
        const uint32_t lp = (uint32_t)(s_x[i] & 0xFFFF) | ((uint32_t)s_y[i] << 16);
        if (g_frames == s_f[i] - 10) { g_mx = s_x[i]; g_my = s_y[i]; PushMsg(WM_MOUSEMOVE, 0, lp); }
        if (getenv("R_MOVEONLY")) continue;
        if (g_frames == s_f[i]) { PushMsg(WM_LBUTTONDOWN, MK_LBUTTON, lp);
            fprintf(stderr, "[click] frame %ld: %d,%d\n", g_frames, s_x[i], s_y[i]); }
        if (g_frames == s_f[i] + 20) PushMsg(WM_LBUTTONUP, 0, lp);
    }
}

void r_pageflip(void) {
    g_frames++;
    ScriptedClicks();
    ScriptedKeys();
    ShotIfWanted();
    DX11_Present(g_dx);
    PumpHostWindow();
}

// the software frame (loading screens): RGB565 in guest memory, drawn as one
// opaque full-screen quad (wrapper THRASH_unlockwindow)
void r_unlockwindow(uint32_t pixels, int w, int h, int pitch) {
    if (!pixels || !g_dx.context || w <= 0 || h <= 0) return;
    const uint8_t* src = (const uint8_t*)G(pixels);
    uint32_t* rgba = new uint32_t[(size_t)w * h];
    for (int y = 0; y < h; y++) {
        const uint16_t* row = (const uint16_t*)(src + (size_t)y * pitch);
        for (int x = 0; x < w; x++) {
            uint16_t c = row[x];
            uint32_t r = ((c >> 11) & 0x1F) * 255 / 31, g = ((c >> 5) & 0x3F) * 255 / 63, b = (c & 0x1F) * 255 / 31;
            rgba[(size_t)y * w + x] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    }
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {rgba, (UINT)(w * 4), 0};
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(g_dx.device->CreateTexture2D(&desc, &init, &tex)) && tex) {
        ID3D11ShaderResourceView* srv = nullptr;
        if (SUCCEEDED(g_dx.device->CreateShaderResourceView(tex, nullptr, &srv)) && srv) {
            ID3D11ShaderResourceView* prevSrv = nullptr;
            ID3D11PixelShader* prevPs = nullptr;
            ID3D11DepthStencilState* prevDepth = nullptr;
            UINT prevStencil = 0;
            g_dx.context->PSGetShaderResources(0, 1, &prevSrv);
            g_dx.context->PSGetShader(&prevPs, nullptr, nullptr);
            ID3D11BlendState* prevBlend = g_dx.boundBlend;
            g_dx.context->OMGetDepthStencilState(&prevDepth, &prevStencil);
            ID3D11SamplerState* prevSampler = g_dx.boundSampler0;
            const bool prevFog = g_dx.fogEnabled;
            if (prevFog) { g_dx.fogEnabled = false; DX11_UpdateScreenCB(g_dx); }
            DX11_SetBlend(g_dx, g_dx.blendOpaque);
            g_dx.context->OMSetDepthStencilState(g_dx.depthStates[7][0], 0);
            DX11_SetSampler0(g_dx, g_dx.samplerLinear);
            DX11_ApplyGpuViewport(g_dx);
            g_dx.context->PSSetShaderResources(0, 1, &srv);
            g_dx.context->PSSetShader(g_dx.pixelShaderTex, nullptr, 0);
            float lw = (float)g_dx.logicalW, lh = (float)g_dx.logicalH;
            THRASH_Vertex v0 = {0, 0, 0, 1, 0xFFFFFFFF, 0, 0, 0};
            THRASH_Vertex v1 = {lw, 0, 0, 1, 0xFFFFFFFF, 0, 1, 0};
            THRASH_Vertex v2 = {lw, lh, 0, 1, 0xFFFFFFFF, 0, 1, 1};
            THRASH_Vertex v3 = {0, lh, 0, 1, 0xFFFFFFFF, 0, 0, 1};
            Draw_Quad(&v0, &v1, &v2, &v3);
            g_dx.context->PSSetShaderResources(0, 1, &prevSrv);
            g_dx.context->PSSetShader(prevPs, nullptr, 0);
            DX11_SetBlend(g_dx, prevBlend);
            g_dx.context->OMSetDepthStencilState(prevDepth, prevStencil);
            DX11_SetSampler0(g_dx, prevSampler);
            if (prevFog) { g_dx.fogEnabled = true; DX11_UpdateScreenCB(g_dx); }
            if (prevSrv) prevSrv->Release();
            if (prevPs) prevPs->Release();
            if (prevDepth) prevDepth->Release();
            srv->Release();
        }
        tex->Release();
    }
    delete[] rgba;
}

int r_readrect(int x, int y, int w, int h, uint32_t buf) {
    if (!buf || w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    return DX11_ReadRect565(g_dx, x, y, w, h, (uint16_t*)G(buf)) ? 1 : 0;
}

// textures: the core's handles are slot + 1, a plain 32-bit value
uint32_t r_talloc(int w, int h, int fmt, int flags, int mips) {
    return (uint32_t)(uintptr_t)Tex_Alloc(w, h, fmt, flags, mips);
}
void r_tfree(uint32_t t) { Tex_Free((void*)(uintptr_t)t); }
void r_tupdate(uint32_t t, uint32_t data, int palette) {
    Tex_NoteUpdate((int)t - 1);
    Tex_Update((void*)(uintptr_t)t, palette, G(data));
}
void r_tupdaterect(uint32_t t, int mip, int x, int y, int w, int h, int pitch, uint32_t data, int fmt) {
    Tex_UpdateRect((void*)(uintptr_t)t, mip, x, y, w, h, pitch, G(data), fmt);
}
void r_treset(void) { Tex_Reset(); }
void r_settexture(uint32_t t) { Tex_Bind((void*)(uintptr_t)t); }

// primitives: a = the guest stack arguments of the THRASH call
void r_draw(const char* n, const uint32_t* a) {
    const int vS = g_vertexStride, iS = g_indexStride;
    const int count = (int)a[0];
    auto VI = [&](const uint8_t* vb, const uint8_t* ib, int i) {
        return (THRASH_Vertex*)(vb + *(const uint16_t*)(ib + i * iS) * vS);
    };
    if (!strcmp(n, "drawtri@12")) { Draw_Tri(V(a[0]), V(a[1]), V(a[2])); return; }
    if (!strcmp(n, "drawquad@16")) { Draw_Quad(V(a[0]), V(a[1]), V(a[2]), V(a[3])); return; }
    if (!strcmp(n, "drawline@8")) { Draw_Line(V(a[0]), V(a[1])); return; }
    if (!strcmp(n, "drawpoint@4")) { Draw_Point(V(a[0])); return; }
    if (!strcmp(n, "drawsprite@8")) { Draw_Sprite(V(a[0]), V(a[1])); return; }
    const uint8_t* vb = (const uint8_t*)G(a[1]);
    if (!vb || count <= 0) return;
    if (!strcmp(n, "drawtrifan@8")) {
        for (int i = 1; i < count - 1; i++)
            Draw_Tri((THRASH_Vertex*)vb, (THRASH_Vertex*)(vb + i * vS), (THRASH_Vertex*)(vb + (i + 1) * vS));
        return; }
    if (!strcmp(n, "drawtristrip@8")) {
        for (int i = 0; i < count - 2; i++) {
            THRASH_Vertex* p0 = (THRASH_Vertex*)(vb + i * vS);
            THRASH_Vertex* p1 = (THRASH_Vertex*)(vb + (i + 1) * vS);
            THRASH_Vertex* p2 = (THRASH_Vertex*)(vb + (i + 2) * vS);
            if (i & 1) Draw_Tri(p1, p0, p2); else Draw_Tri(p0, p1, p2);
        }
        return; }
    if (!strcmp(n, "drawlinestrip@8")) {
        for (int i = 0; i < count - 1; i++) Draw_Line((THRASH_Vertex*)(vb + i * vS), (THRASH_Vertex*)(vb + (i + 1) * vS));
        return; }
    if (!strcmp(n, "drawpointstrip@8")) {
        for (int i = 0; i < count; i++) Draw_Point((THRASH_Vertex*)(vb + i * vS));
        return; }
    const uint8_t* ib = (const uint8_t*)G(a[2]);
    if (!ib) return;
    THRASH_Vertex* verts = (THRASH_Vertex*)vb;
    if (!strcmp(n, "drawtrimesh@12")) { Draw_TriMesh(count, (void*)ib, verts); return; }
    if (!strcmp(n, "drawtrifan@12")) { Draw_TriFan(count, (void*)ib, verts); return; }
    if (!strcmp(n, "drawtristrip@12")) { Draw_TriStrip(count, (void*)ib, verts); return; }
    if (!strcmp(n, "drawquadmesh@12")) {
        for (int i = 0; i < count; i++)
            Draw_Quad(VI(vb, ib, i * 4), VI(vb, ib, i * 4 + 1), VI(vb, ib, i * 4 + 2), VI(vb, ib, i * 4 + 3));
        return; }
    if (!strcmp(n, "drawlinemesh@12")) {
        for (int i = 0; i < count; i++) Draw_Line(VI(vb, ib, i * 2), VI(vb, ib, i * 2 + 1));
        return; }
    if (!strcmp(n, "drawlinestrip@12")) {
        for (int i = 0; i < count - 1; i++) Draw_Line(VI(vb, ib, i), VI(vb, ib, i + 1));
        return; }
    if (!strcmp(n, "drawpointmesh@12")) {
        for (int i = 0; i < count; i++) Draw_Point(VI(vb, ib, i));
        return; }
    if (!strcmp(n, "drawspritemesh@12")) {
        for (int i = 0; i < count; i++) Draw_Sprite(VI(vb, ib, i * 2), VI(vb, ib, i * 2 + 1));
        return; }
    static int unknown;
    if (unknown++ < 8) fprintf(stderr, "[dx11] unhandled %s\n", n);
}

const uint8_t* host_dik_state(void) { return g_dik; }
void metal_get_cursor(int* x, int* y) { *x = g_mx; *y = g_my; }
// called by the runtime's PeekMessage/GetMessage (GIL held): host messages
// in, the guest window's key messages out into its queue
void win32_post_message(uint32_t m, uint32_t wp, uint32_t lp);
int win32_msg_room(void);
void kbd_pump(void) {
    if (!g_win) return;
    if (GetCurrentThreadId() == g_winThread) PumpHostWindow();
    while (g_wq_head != g_wq_tail && win32_msg_room() > 8) {
        const WinMsg& e = g_wq[g_wq_head++ & 255];
        win32_post_message(e.m, e.w, e.l);
        if (getenv("KBDLOG")) fprintf(stderr, "[kbd] -> guest queue msg 0x%x wp 0x%x lp 0x%x (frame %ld)\n", e.m, e.w, e.l, g_frames);
    }
}
// peek >= 0: the peek-th pending event without removing it; -1: take the oldest
int host_key_event(int peek, uint32_t* ofs, uint32_t* data, uint32_t* ts, uint32_t* seq) {
    unsigned i = g_kq_head + (peek >= 0 ? (unsigned)peek : 0u);
    if (i >= g_kq_tail) return 0;
    const KeyEvent& e = g_kq[i & 255];
    *ofs = e.ofs; *data = e.data; *ts = e.ts; *seq = e.seq;
    if (peek < 0) g_kq_head++;
    return 1;
}
long r_frames(void) { return g_frames; }

}  // extern "C"
