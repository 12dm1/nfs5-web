#pragma once
#include <cstdint>

void State_Init();
void State_Set(int id, int value);
void State_SetDirect(int id, int value);
int  State_Get(int id);
// Internal state read without THRASH_getstate diagnostic logging.
int  State_Peek(int id);
// G1: anisotropy for bilinear samplers, whether to honor the game's LOD bias
// (state 0x08), and the divisor turning a Glide depth-bias level into a
// relative W offset (4096 = one 12-bit W-buffer mantissa LSB).
void State_ConfigureSampling(int aniso, bool honorLodBias, int depthBiasDivisor);
void State_ReleaseCaches();
void State_ConfigureLogging(bool enabled);
bool State_LoggingEnabled();
// G1v3: honor THRASH state 2 (dx7z software face culling); false = CULL_NONE.
void State_ConfigureCull(bool honor);
// G7: point sampling (state 7 = 0): 0 never, 1 always, 2 only in the race.
void State_ConfigurePointFilter(int mode);
void State_SetRaceVideoMode(bool race);   // set by THRASH_setvideomode (32-bit = race)
bool State_RaceVideoMode();
// Push every tracked game state (blend, depth, sampler, constants, texture)
// to the GPU again; used after an extra scene traversal restored the pipeline.
void State_Reapply();
// MB2: re-send the current depth state (the stencil-mark variant follows
// MotionBlur_ForegroundMark()).
void State_ApplyDepth();
// AA1: re-send the tracked blend mode (alpha-to-coverage variant when the
// draw is alpha-tested on an MSAA target).
void State_ApplyBlend();
// Re-evaluate A2C / hard world-cutout after a texture bind.
void State_RefreshCutoutPolicy();
// Tiling cutout textures (UVs > 1): skip the sampler CLAMP override for one draw.
// HZ6: no hard cutout for the next draws (sorted draw with vertex alpha).
void State_SuppressHardCutout(bool on);
void State_OverrideCutoutClamp(bool skip);
// G5: the game's clip rectangle (THRASH_clip) in logical coordinates; sets the scissor.
void State_SetClip(int x1, int y1, int x2, int y2);

extern int g_vertexStride;
extern int g_indexStride;
