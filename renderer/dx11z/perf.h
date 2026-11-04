#pragma once
#include <cstdint>
#include <intrin.h>

// PERF1: frame profiler ([perf] enable=1, default off).
//
// CPU: time per zone (rdtsc, calibrated against QPC), split by the view the
// work belongs to (main camera, rear-view mirror, R1 cube capture, MAT14 car
// layer, other = HUD / frontend) and by the parts of the game's scene pass
// (sky, track, cars, flush, effects, background). Every zone also records
// how much of its time was spent inside the wrapper's draw entry points
// (THRASH_draw*), so the game's own work (traversal, software transform and
// lighting) and the wrapper's (conversion, D3D11 submission) separate.
//
// GPU: D3D11 timestamp queries around the same zones, read back a few frames
// later without stalling; pipeline statistics (primitives, pixel shader
// invocations) per pass.
//
// Output: one "PERF" block per [perf] interval frames in dx11z.log and one row
// per frame in dx11z_perf.csv (next to the game executable).

enum PerfView { PV_OTHER = 0, PV_MAIN, PV_REAR, PV_CUBE, PV_PLANAR, PV_COUNT };

// Per-view zones: the scene-pass parts, then the per-object / leaf zones.
enum PerfViewZone {
    VZ_SKY = 0, VZ_TRACK, VZ_CARS, VZ_FLUSH, VZ_FX, VZ_BG,   // scene-pass parts
    VZ_API,       // THRASH_draw* (wrapper conversion + submission)
    VZ_CAROBJ,    // one car in the deferred-tree flush (game transform + light + draws)
    VZ_OBJ,       // any other deferred-tree object
    VZ_LIGHT,     // the game's light apply on a car batch
    VZ_COUNT
};
const int VZ_PARTS = VZ_BG + 1;

enum PerfZone {
    PZ_RACE = 0,  // 004689C0 race render (main, rear-view mirror, cube, HUD)
    PZ_MAIN,      // 00468BA0 main view (incl. the MAT14 layer)
    PZ_REAR,      // 00468CA0 rear-view mirror
    PZ_CUBE,      // R1 capture: 6 faces + mips + diagnostics
    PZ_CUBE_MIPS,
    PZ_CUBE_READ, // cube readback diagnostics (stall)
    PZ_PLANAR,    // MAT14 car layer
    PZ_MBLUR,
    PZ_HOOKS,     // THRASH_pageflip housekeeping before Present
    PZ_PRESENT,   // DX11_Present: resolve + Present (vsync / GPU back-pressure)
    PZ_LIGHT_WRAP,// CAR1/CAR2 work in the light hook
    PZ_SMOOTH,    // MAT11 SmoothNormals
    PZ_MATCB,     // Materials_BeginShade + EndShade
    PZ_TEX,       // talloc / tfree / tupdate / tupdaterect
    PZ_STATE,     // setstate
    PZ_LOG,       // Log / LogTransition
    PZ_DIAG,      // CAR2 diag readbacks (car_diag)
    PZ_SIM,       // 00414FF0 simulation ticks (fixed step: physics, AI, sound, network)
    PZ_CARPAIR,   // car-car collision pairs (004980D0 via the PHYS1 hook)
    PZ_SLEEP,     // the render thread in the game's Sleep / SleepEx
    PZ_WAIT,      // the render thread in the game's WaitForSingleObject / WaitForMultipleObjectsEx
    PZ_COUNT
};

// Per-view counters.
enum PerfCount {
    CT_DRAWS = 0, // THRASH_draw* calls
    CT_PRIMS,     // primitives the game passed
    CT_D3D,       // D3D11 Draw calls
    CT_VERTS,     // vertices submitted
    CT_MAT,       // material-shaded draws
    CT_CARMAT,    // ... of them car paint
    CT_CAROBJ,    // cars drawn from the deferred tree
    CT_OBJ,       // other deferred-tree objects
    CT_LIGHT,     // car light batches
    CT_COUNT
};

// Global counters.
enum PerfGlobal {
    CG_TEXUP = 0, // texture uploads (tupdate / tupdaterect)
    CG_TEXKB,     // uploaded kilobytes
    CG_STATE,     // setstate calls
    CG_LOG,       // log lines
    CG_VBWRAP,    // dynamic vertex buffer DISCARD maps
    CG_CBSCREEN,  // screen constant buffer updates
    CG_CUBES,     // R1 captures
    CG_TICKS,     // simulation ticks
    CG_PAIRS,     // car-car collision pair tests
    CG_SLEEPS,    // Sleep / SleepEx calls of the render thread
    CG_WAITS,     // wait calls of the render thread
    CG_COUNT
};

// PERF1c: individual D3D11 calls of the draw path (count + time), to see
// which call blocks when the driver stalls.
enum PerfCall {
    PC_VB_MAP = 0,  // SubmitVerts: Map of the dynamic vertex buffer
    PC_VB_COPY,     // memcpy + Unmap
    PC_DRAW,        // IASet* + Draw
    PC_CB_SCREEN,   // DX11_UpdateScreenCB (Map DISCARD of b0)
    PC_MAT_GET,     // BeginShade: VS/PSGetShader
    PC_MAT_MAP,     // BeginShade: Map DISCARD of the material constant buffer
    PC_MAT_SET,     // BeginShade: shaders + constant buffers
    PC_MAT_SRV,     // BeginShade: shader resources + samplers
    PC_MAT_END,     // EndShade (restore shaders, clear resources)
    PC_SNAPSHOT,    // cube / car layer pipeline snapshot (Get* of the whole pipeline)
    PC_COUNT
};

// GPU zones (timestamp pairs).
enum PerfGpuZone {
    GZ_FRAME = 0, GZ_MAIN, GZ_PLANAR, GZ_REAR, GZ_CUBE, GZ_CUBE_MIPS, GZ_MBLUR, GZ_RESOLVE,
    GZ_MAIN_PART0,                        // + VZ_SKY..VZ_BG
    GZ_CUBE_PART0 = GZ_MAIN_PART0 + VZ_PARTS,
    GZ_COUNT = GZ_CUBE_PART0 + VZ_PARTS
};

extern bool     g_perfOn;
extern int      g_perfView;
extern uint64_t g_perfApiTicks;     // running total of VZ_API ticks (all views)
extern uint64_t g_perfLightTicks;   // running total of VZ_LIGHT ticks

void Perf_Configure(bool enabled, int intervalFrames, bool csv);
// [perf] sample=1: a background thread samples the render thread's EIP about
// once a millisecond (modules / hot addresses per zone in the PERF block).
void Perf_ConfigureSampler(bool on);
bool Perf_Enabled();
// Call-site hooks: race render, main pass (chained with MB1), scene-pass parts.
// Install after every other hook of THRASH_init.
bool Perf_Install();

void Perf_AddZone(int zone, uint64_t ticks, uint64_t apiTicks);
void Perf_AddView(int view, int vz, uint64_t ticks, uint64_t apiTicks);
void Perf_Count(int view, int counter, uint32_t n);
void Perf_Global(int counter, uint32_t n);
// The parts of a simulation tick and of the race loop around it (perf.cpp kSim*).
const int SIM_PARTS = 20;

// GPU timestamps (no-ops when off or without a device).
void Perf_GpuMark(int gz, bool end);
void Perf_ReleaseGpu();

// THRASH_pageflip: FrameEnd right before Present (after the resolve),
// FrameStart right after it (closes the CPU frame, resolves old GPU frames,
// logs, starts the next frame).
void Perf_FrameEnd();
void Perf_FrameStart();

inline uint64_t Perf_Now() { return __rdtsc(); }
void Perf_AddCall(int call, uint64_t ticks);

// One D3D11 call (or a short group of them).
struct PerfCallScope {
    int call; uint64_t t0;
    explicit PerfCallScope(int c) : call(c), t0(g_perfOn ? __rdtsc() : 0) {}
    ~PerfCallScope() { if (t0) Perf_AddCall(call, __rdtsc() - t0); }
};

// A zone of the whole frame.
struct PerfScope {
    int zone; uint64_t t0, a0;
    explicit PerfScope(int z) : zone(z), t0(0), a0(0) {
        if (g_perfOn) { t0 = __rdtsc(); a0 = g_perfApiTicks; }
    }
    ~PerfScope() { if (t0) Perf_AddZone(zone, __rdtsc() - t0, g_perfApiTicks - a0); }
};

// A zone of the current view.
struct PerfViewScope {
    int view, vz; uint64_t t0, a0;
    explicit PerfViewScope(int z) : view(g_perfView), vz(z), t0(0), a0(0) {
        if (g_perfOn) { t0 = __rdtsc(); a0 = g_perfApiTicks; }
    }
    ~PerfViewScope() { if (t0) Perf_AddView(view, vz, __rdtsc() - t0, g_perfApiTicks - a0); }
};

// A scene-pass part: CPU per view, GPU timestamps for the main view and the cube.
struct PerfPartScope {
    int view, part; uint64_t t0, a0;
    explicit PerfPartScope(int p) : view(g_perfView), part(p), t0(0), a0(0) {
        if (!g_perfOn) return;
        if (view == PV_MAIN) Perf_GpuMark(GZ_MAIN_PART0 + part, false);
        else if (view == PV_CUBE) Perf_GpuMark(GZ_CUBE_PART0 + part, false);
        t0 = __rdtsc(); a0 = g_perfApiTicks;
    }
    ~PerfPartScope() {
        if (!t0) return;
        Perf_AddView(view, part, __rdtsc() - t0, g_perfApiTicks - a0);
        if (view == PV_MAIN) Perf_GpuMark(GZ_MAIN_PART0 + part, true);
        else if (view == PV_CUBE) Perf_GpuMark(GZ_CUBE_PART0 + part, true);
    }
};

// A draw entry point: time (the leaf zone the others subtract) and counts.
struct PerfDrawScope {
    uint64_t t0;
    explicit PerfDrawScope(uint32_t prims) : t0(0) {
        if (!g_perfOn) return;
        Perf_Count(g_perfView, CT_DRAWS, 1);
        Perf_Count(g_perfView, CT_PRIMS, prims);
        t0 = __rdtsc();
    }
    ~PerfDrawScope() {
        if (!t0) return;
        const uint64_t d = __rdtsc() - t0;
        g_perfApiTicks += d;
        Perf_AddView(g_perfView, VZ_API, d, d);
    }
};

// Switches the view for a pass and restores the outer one.
struct PerfViewSwitch {
    int saved;
    explicit PerfViewSwitch(int v) : saved(g_perfView) { g_perfView = v; }
    ~PerfViewSwitch() { g_perfView = saved; }
};

// GPU zone pair.
struct PerfGpuScope {
    int gz;
    explicit PerfGpuScope(int z) : gz(z) { if (g_perfOn) Perf_GpuMark(gz, false); }
    ~PerfGpuScope() { if (g_perfOn) Perf_GpuMark(gz, true); }
};
