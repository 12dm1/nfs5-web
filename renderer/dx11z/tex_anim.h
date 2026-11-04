#pragma once
#include <cstdint>

// HZ4: NFS4 animated textures on converted tracks.
//
// NFS4 animates a texture in one of two ways (nfshs.exe FUN_00434690, on
// the polygon's anim byte and a 1/64 s clock):
//   frames  frame = (clock / (anim >> 3)) % (anim & 7), the frames being
//           consecutive textures of tr0.qfs (Aquatica bubbles, water);
//   scroll  texflags 0x2000: v += (clock & 0x3FF) / 1024 * 15 (Dolphin Cove,
//           Snowy).
// PU has neither. The converter writes GameData/Track/<track>.tan: per base
// texture tag its frame count, delay, scroll speed and the BGRA of frames
// 1..n-1. When the game uploads a base texture (DTEX name = FSH tag), the
// frames are created as extra texture slots processed like a game upload;
// Tex_Bind swaps in the current frame and FlushVerts shifts v for scrolls.
//
// [content] texture_animation = 1 (default) / 0.

void  TexAnim_Configure(bool enabled);
// THRASH_treset: the table is per track, the frame slots are gone.
void  TexAnim_OnReset();
// After the game uploaded `slot` (name = its DTEX/FSH tag).
void  TexAnim_OnUpload(int slot, const char* name, int w, int h);
// Before the game frees `slot`.
void  TexAnim_OnFree(int slot);
// Handle to bind instead of `handle` (the current frame), or `handle`.
void* TexAnim_Substitute(void* handle);
// Texture-space v shift for a scrolling texture bound in `slot`, else 0.
float TexAnim_VOffset(int slot);
