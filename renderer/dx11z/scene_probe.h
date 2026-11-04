#pragma once
#include <cstdint>

// R0 diagnostic: render the race scene once more per N frames from an
// independent game view into an offscreen target, using the game's own view
// construction, pose setters, traversal and flush. Proves that a second camera
// (looking behind the main camera, or in a fixed world direction) receives
// scene geometry from the same simulation snapshot without touching the
// presented frame. R0 is off unless reflection_probe is 1 or 2; R1 uses the
// same verified hook when reflection_cubemap is enabled.

enum class ProbeMode { Off = 0, Backward = 1, FixedWorld = 2 };

enum class CubemapFace {
    PositiveX = 0, NegativeX = 1, PositiveY = 2,
    NegativeY = 3, PositiveZ = 4, NegativeZ = 5
};

void SceneProbe_Configure(ProbeMode mode, int intervalFrames);
ProbeMode SceneProbe_Mode();
// R1 dynamic environment capture. The cubemap uses an independent square
// target and captures all six faces from one simulation snapshot. The player
// car is excluded to avoid self-reflection; other cars and track geometry stay.
void SceneProbe_ConfigureCubemap(bool enabled, int size, int intervalFrames,
                                 bool dumpFirstCapture);
bool SceneProbe_CubemapEnabled();
// G12: capture into a float16 cube (emissive draws keep values above 1).
void SceneProbe_ConfigureCubemapHdr(bool hdr);
bool SceneProbe_CubemapHdr();
int  SceneProbe_CubemapSize();
// Diagnostic: hardware write watch (Dr0) on one stack address while the six
// cubemap faces render; hits log EIP and return-address candidates. 0 = only
// arm after the snapshot check has located a corrupted dword itself.
void SceneProbe_ConfigureStackWatch(uint32_t address);
// Patch the mirror-pass call site 00468AA1 inside the race frame renderer for
// whichever of the R0 diagnostic or R1 cubemap capture is enabled.
// Requires CameraHooks_Installed() (same executable identity). All-or-nothing.
bool SceneProbe_Install();
bool SceneProbe_Installed();
// True while the probe pass is running: draws and clears go to the probe target.
bool SceneProbe_Active();
// Draw path counters (triangles per draw); also feeds the per-frame totals.
void SceneProbe_CountTriangles(int triangles);
// MAT14d/MAT16 diagnostic: a draw inside a capture; additive ones are counted
// by capture phase (sky / track / cars / background), sun / lens-flare sprite
// draws (MAT16 hooks) separately.
void SceneProbe_NoteCaptureDraw(bool additive);
// Called from THRASH_pageflip: closes the per-frame totals and logs the
// frames around a recent probe (N-2..N+2) so a one-frame loss is visible.
void SceneProbe_OnPageflip(uint32_t frame);
// Release GPU resources (driver restore/shutdown).
void SceneProbe_ReleaseTargets();
// CAR2 diag: the current R1 cube at mip 3 (32x32 per face) in a world
// direction: out = {r, g, b, a, 3x3 max peak, 3x3 max alpha}; false without
// a cube. Stalls the GPU (a staging readback), diagnostics only.
bool SceneProbe_CubeSample(const float dir[3], float out[6]);
// All six faces at mip 3: out = {mean peak, max peak, mean alpha, max alpha,
// world dir of the max-alpha texel (3), world dir of the max-peak texel (3)}.
bool SceneProbe_CubeStats(float out[10]);

// MAT14 car mirror layer (car_mirror.h). Right after the main pass has begun
// its view (call site 00468C77) the game's car pass and deferred-tree flush
// run once more into an offscreen layer; the draw path mirrors every car in
// the plane of the road under it. The material shader samples the layer by
// screen position on up-facing reflective surfaces.
void SceneProbe_ConfigurePlanar(bool enabled, int resolutionDivisor, float blur, float distortion);
bool SceneProbe_PlanarEnabled();
// True while the engine's active view is the main view of the current frame
// (not the rear-view mirror, not a capture): road samples are taken then.
bool SceneProbe_MainViewActive();
struct ID3D11ShaderResourceView;
struct ID3D11SamplerState;
struct PlanarReflectionInfo {
    ID3D11ShaderResourceView* srv;        // the layer: black, alpha 0 outside the cars
    ID3D11ShaderResourceView* depthSrv;   // its depth (R24 of D24S8)
    ID3D11SamplerState* sampler;          // linear, clamp
    float mipScale;       // max mip * blur
    float distortion;
};
// True when this frame's layer exists and the engine's active view is the
// view it was drawn for (the main view, not the rear-view mirror).
bool SceneProbe_PlanarInfo(PlanarReflectionInfo* out);

// Borrowed SRV for the most recently captured TextureCube. R2 may sample it
// only outside SceneProbe_Active(); the capture unbinds shader inputs before
// selecting a face as an RTV, preventing read/write feedback.
struct ID3D11ShaderResourceView;
ID3D11ShaderResourceView* SceneProbe_CubemapSrv();
// FE1: the menu's showroom cube. After the menu ends its 3D view, the cube is
// captured from the car's spot with only the location models the screen drew
// (FEData/Models: dealers, garage, victory room, factory, track). In the menu
// SceneProbe_CubemapSrv() returns only such a capture (null otherwise).
void SceneProbe_ConfigureFrontend(bool enabled, float height, int intervalFrames);
bool SceneProbe_CubeIsFrontend();
// Number of completed six-face captures since the DLL was loaded. R2 keys
// its "cube is fresh for this race" state on this counter.
uint32_t SceneProbe_CubemapCaptures();
// True between the first and last face of a capture (crash report context).
bool SceneProbe_CaptureInProgress();
// Resource construction entry used by the offscreen WARP contract test.
bool SceneProbe_EnsureCubemapTargets();

// Result of the last probe pass, for tests/logs.
struct SceneProbeResult {
    uint32_t frame;
    int      variant;      // 0 minimal pass, 1 lifecycle only, 2 game scene pass, 3 persistent view
    int      draws;
    int      triangles;
    int      nonBlackPixels;
    int      totalPixels;
    uint32_t hash;
    float    position[3];
    float    forward[3];
};
bool SceneProbe_LastResult(SceneProbeResult* out);
// Rotation rows are camera basis vectors in world space (Y up); this turns the
// camera 180 degrees about world Y and keeps the matrix a proper rotation.
void SceneProbe_BackwardRotation(float rot9[9]);

struct SceneCubemapFaceResult {
    int      draws;
    int      triangles;
    int      nonBlackPixels;
    int      totalPixels;
    uint32_t hash;
    float    projScaleX;    // C+148 after begin-view: 1.0 for an exact 90-degree face
    float    projScaleY;    // -C+15C: 1.0 for a square face
    float    right[3];
    float    up[3];
    float    forward[3];
};

struct SceneCubemapResult {
    uint32_t frame;
    int      size;
    int      totalDraws;
    int      totalTriangles;
    float    submitMilliseconds;
    float    readbackMilliseconds;
    float    position[3];
    SceneCubemapFaceResult faces[6];
};
bool SceneProbe_LastCubemapResult(SceneCubemapResult* out);

// Exact Direct3D cubemap orientation in the engine's row convention
// (right, up, forward; Y-up world). Used both for capture and fixtures.
void SceneProbe_CubemapFaceRotation(CubemapFace face, float rot9[9]);
