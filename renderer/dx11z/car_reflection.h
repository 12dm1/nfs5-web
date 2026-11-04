#pragma once
#include <cstdint>
#include "thrash_api.h"

// R2: shade the intended car materials with the R1 environment cubemap.
//
// The car's reflective materials contain an EAGL R3DSpecularTextureLayer
// (vtable 005B39B4, apply 00445520): a separate blended pass whose texture is
// a 64x64 sphere map from the track .env package (its alpha is the designed
// intensity) and whose UV is a function of the camera-space vertex normal.
// The layer already owns the inputs a reflection needs: object-space normals
// from its dependency layer, the current model and view matrices. It writes
// one (u, v) per output vertex into the EAGL vertex array that the finalize
// routines later hand to THRASH_drawtrimesh; those two floats survive the
// vertex remap and CPU clipper unchanged, which the 32-byte THRASH vertex
// never could for a normal.
//
// R2 therefore keeps the game's material selection, texture/band choice and
// blend state, and replaces only what the two floats mean: the hook calls the
// original layer, then rewrites every UV slot with a stereographic encoding
// of the camera-space vertex normal. The wrapper recognizes the following
// drawtrimesh of that vertex array, decodes the normal per pixel, replays the
// game's sphere-map UV to read the designed intensity from the bound .env
// texture, rebuilds the per-pixel view ray from the camera's own projection
// coefficients and samples the cubemap with the world-space reflection
// vector. The tunnel-lamp layer (R3DSkyMapTextureLayer, 005B398C) is left as
// the game's own effect.

struct ReflectConstants {
    float camRight[4];     // world-space camera basis rows (V+E8, +F4, +100)
    float camUp[4];
    float camForward[4];   // camera space z = -forward
    float proj[4];         // sX, sY, oX, oY (C+148, -C+15C, -C+168, -C+16C)
    float viewport[4];     // pixel center X/Y (+0.01 bias), half width/height
    float params[4];       // strength, fresnel F0 (0 = off), 0, 0
};

// Camera-space unit normal -> two floats (stereographic from the -Z pole,
// which is the direction pointing away from the camera and therefore never
// belongs to a visible front-facing vertex; facing normals, z = +1, land at
// the origin). The decoder is the inverse.
void CarReflection_EncodeNormal(const float normalCam[3], float out[2]);
void CarReflection_DecodeNormal(const float in[2], float normalCam[3]);
// Object-space normal through a row-vector model matrix (16 floats) and the
// view's basis rows into camera space, normalized. Returns false when the
// result is degenerate (zero-length input or matrix).
bool CarReflection_NormalToCamera(const float normalObj[3], const float model16[16],
                                  const float right[3], const float up[3],
                                  const float forward[3], float normalCam[3]);
// Camera-space direction -> world direction using the same basis rows.
void CarReflection_CameraToWorld(const float dirCam[3], const float right[3],
                                 const float up[3], const float forward[3],
                                 float dirWorld[3]);
// Reflection of the view ray v about normal n (both camera space, v points
// from the camera to the surface). Standard reflect().
void CarReflection_Reflect(const float v[3], const float n[3], float r[3]);
// Per-pixel view ray in camera space from a game draw pixel and the
// projection/viewport constants (P6 section 3 inverted).
void CarReflection_PixelRay(const ReflectConstants& c, float px, float py, float ray[3]);
// The game's own sphere-map UV for a camera-space normal (00445520,
// including its fast inverse square root); the pixel shader replays it.
void CarReflection_SphereMapUv(const float normalCam[3], float uv[2]);

// [display] reflection_shading=0|1, reflection_strength, reflection_fresnel.
void CarReflection_Configure(bool enabled, float strength, float fresnelF0);
// G11: false skips the R3DSkyMapTextureLayer (tunnel-lamp projection) pass.
void CarReflection_ConfigureTunnelLamps(bool enabled);
bool CarReflection_Enabled();
// Patch the layer vtable entry after byte checks. Requires the camera hooks
// (executable identity) and the R1 cubemap to be configured.
bool CarReflection_Install();
bool CarReflection_Installed();

// Draw-path integration. ShouldUse() is true for exactly the drawtrimesh that
// follows a rewritten layer pass with that vertex array; any other THRASH draw
// clears the pending state through OnDraw().
bool CarReflection_ShouldUse(const void* vertices, const void* indices);
void CarReflection_OnDraw();
// Constants for the pending draw (valid after ShouldUse() returned true).
const ReflectConstants& CarReflection_PendingConstants();

// R3: decal mask. The reflection pass is a second draw over the same corners
// as the material's diffuse pass; its intensity is the .env alpha, not the
// diffuse texture's, so a decal quad (Decal/DecalHood/License use CarExtEnv)
// reflects across its whole rectangle, doubling the body's reflection where
// the sticker is transparent. NoteDiffuseDraw() remembers the corner UVs and
// texture of every ordinary drawtrimesh; DiffuseMask() hands them back for a
// reflective draw whose corner count, arrays and first/last corner positions
// match the previous draw. The pixel shader then multiplies the reflection by
// the diffuse texture's alpha at those UVs.
void CarReflection_ConfigureDecalMask(bool enabled);
bool CarReflection_DecalMaskEnabled();
void CarReflection_NoteDiffuseDraw(int count, const THRASH_Vertex* vertices, const void* indices,
                                   int vertexStride, int indexStride, void* texture);
// Returns count*3 (u, v) pairs in corner order, or null when the previous
// draw is not this geometry's diffuse pass. *texture = its THRASH handle.
const float* CarReflection_DiffuseMask(int count, const THRASH_Vertex* vertices, const void* indices,
                                       int vertexStride, int indexStride, void** texture);
uint32_t CarReflection_MaskedDraws();
uint32_t CarReflection_UnmaskedDraws();
// Called on treset/restore: the cube is stale until the next capture.
void CarReflection_Invalidate();
// Counters for logs/tests.
uint32_t CarReflection_RewrittenBatches();
uint32_t CarReflection_ReflectiveDraws();
uint32_t CarReflection_SkyMapDisarms();

// Hook bodies exposed for the offscreen test (driven on fake layers/EAGL).
// Both layer apply routines are thiscall(this, streamA, streamB) ret 8.
using SkyMapApplyFn = uint32_t (__fastcall*)(uint8_t* layer, void* edx,
                                             uint8_t* streamA, uint8_t* streamB);
extern SkyMapApplyFn g_originalSpecularApply;
extern SkyMapApplyFn g_originalSkyMapApply;
uint32_t __fastcall CarReflection_HookSpecularApply(uint8_t* layer, void* edx,
                                                    uint8_t* streamA, uint8_t* streamB);
uint32_t __fastcall CarReflection_HookSkyMapApply(uint8_t* layer, void* edx,
                                                  uint8_t* streamA, uint8_t* streamB);
// Test seam: override the globals the hook reads (EAGL renderer, sky-map
// parameters, engine renderer). Null restores the executable addresses.
void CarReflection_SetTestGlobals(uint8_t* eagl, uint8_t* skyParams, uint8_t* engine);
// Test seam: treat the cube as captured (the hook otherwise passes through
// until SceneProbe reports a race capture).
void CarReflection_ForceCubeReady(bool ready);
// MB1: basis/projection/viewport constants of a game view (V+E8.., V+0C camera,
// EAGL viewport) plus its world position (V+10C). Same reader as the R2 pass.
bool CarReflection_ViewConstants(const uint8_t* view, ReflectConstants* out, float position[3]);
