#pragma once
#include <cstdint>
#include <d3d11.h>

// MAT2: texture identity (DTEX callback hook + FNV-1a hash).
// MAT5: FSHN binary pack loader, HD albedo replacement, hot reload.
//
// [materials] in dx11z.ini:
//   enable     = 1 (default) — load .fshn packs and replace textures
//   hot_reload = 1 (default) — re-read .fshn on mtime change (~0.5s tick)
//   log        = 0           — per-tupdate hash lines

struct MaterialsConfig {
    bool enable     = true;
    bool hot_reload = true;
    bool log        = false;
    bool debug_all  = false;   // shade ALL textures with default material
    bool smooth_normals = true; // MAT11: weld per-draw vertex normals for material draws
    float sun_reflection = 1.0f; // MAT14d: sun in road reflections (cube alpha mask + highlight), 1 = full
    int   debug_view = 0;        // MAT19: 1 env, 2 sun mask / car cover, 3 roughness, 4 reflection added
    bool  dump_pages = false;    // CAR0: write every unique uploaded page as PNG to _mod/texdump/<track>/
    bool  sun_override = false;  // CAR2: [materials] sun_dir=x,y,z replaces the pack's sun (world space)
    char  ini_path[260] = {};    // CAR2: dx11z.ini path for the live [car_tune] section (built on first use)
    float sun_dir[3] = {0.0f, 1.0f, 0.0f};
    char shader_dir[260] = {};
};

void Materials_Configure(const MaterialsConfig& cfg);

// Lazy-installs the DTEX callback hook on first call.
bool Materials_Install();

// Called from Tex_Update AFTER ConvertToRGBA, BEFORE cutout processing.
// Returns the material index (>=0) if the slot matched a pack entry, -1 otherwise.
int Materials_OnTexUpdate(int slot, const uint32_t* rgba, int w, int h, int fmt);

// CAR0: DTEX name of the page last uploaded into the slot ("CAR0", "tbl1", "?"...).
const char* Materials_SlotName(int slot);

// Called from THRASH_treset to load/unload the pack for the current track.
void Materials_OnReset();

// Called from THRASH_pageflip to check hot-reload mtime.
void Materials_OnPageflip(uint32_t frame);

// Query: does this slot have an HD albedo override?  If so, return its SRV.
ID3D11ShaderResourceView* Materials_HdAlbedoSrv(int slot);

// Query: does this slot have a material-forced cutout flag?
// Returns: 0 = no override, 1 = force cutout, -1 = force non-cutout.
int Materials_CutoutOverride(int slot);

// FNV-1a 64-bit over raw bytes (exposed for tests).
uint64_t Materials_FNV1a64(const void* data, size_t size);

// Diagnostic: hash of a slot's last upload (0 if never uploaded).
uint64_t Materials_SlotHash(int slot);

// MAT7: material shading for track draws.
// Compile the material VS/PS into g_dx (called from CompileShaders).
bool Materials_CompileShaders();
// Release compiled shaders (called from DX11_Shutdown).
void Materials_ReleaseShaders();
// True when the currently bound texture has a non-neutral material that should
// be drawn with the material pixel shader. False during SceneProbe_Active().
bool Materials_ShouldShade();
// MAT11: the frame's view projection constants (matProj of the material CB)
// after Materials_BeginShade; false when the engine view could not be read.
bool Materials_ViewProj(float proj[4]);
bool Materials_SmoothNormalsEnabled();
// MAT11b: camera basis and position of the frame (world space), for stitching
// smooth normals across draw calls; false when the view could not be read.
bool Materials_ViewBasis(float right[3], float up[3], float fwd[3], float pos[3]);
// MAT13: a pack is loaded and at least one material reflects; reflection
// strength of the material bound by Materials_BeginShade (0 outside a shade).
bool Materials_PlanarWanted();
float Materials_BoundReflection();
uint32_t Materials_CurrentFrame();
// MAT14: re-read the view constants now (the main view has just begun and
// the car mirror pass runs before the first material draw of the frame).
void Materials_RefreshView();
// MAT14b: the game's lens-flare readback centre (logical pixels) = the sun on
// screen; logged against the pack's sun direction a few times (self-check).
void Materials_NoteSunProbe(int x, int y, int w, int h);
// Set up material VS/PS/cbuffer/SRV before FlushVerts; restore after.
void Materials_BeginShade();
void Materials_EndShade();
// CAR1: whether the last Materials_BeginShade bound a car material (flag bit5).
bool Materials_LastShadeWasCar();

// CAR1 test hook (depth_smoke): distance of a page to a fingerprint blob
// through the same code the slot match uses; tints[8][4] and alpha[8] as the
// slot would get them. Returns false for a bad blob.
bool Materials_TestFingerprint(const uint8_t* fpBlob, size_t fpSize, const uint32_t* page, int w, int h,
                               const uint8_t* zones, float* distOut, float tints[8][4], float alpha[8]);
// Sun direction and colour from the loaded pack header (world space).
void Materials_SunDir(float out[3]);
// CAR2 diag: the world-space sun the car shader last used (pack / game car
// light / [materials] sun_dir), and the track name the pack loader saw.
bool Materials_ShaderSunWorld(float out[3]);
const char* Materials_TrackName();
// PERF1: textures of the loaded material packs and their GPU size.
void Materials_BlobStats(int* count, double* megabytes);
void Materials_SunColour(float out[3]);
// MAT19 / CP1: [materials] debug_view (8 = NaN / black paint detector).
int Materials_DebugView();
