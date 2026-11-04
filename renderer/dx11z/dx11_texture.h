#pragma once
#include <d3d11.h>
#include <cstdint>

struct TexSlot {
    ID3D11Texture2D*          tex    = nullptr;
    ID3D11ShaderResourceView* srv    = nullptr;
    int width  = 0;
    int height = 0;
    int format = 0;
    int mipLevels = 0;   // set by Tex_Alloc (keeps the slot array zero-initialised)
    bool used  = false;
    // Binary colour-key (foliage, fences). Game FakeSort still submits
    // leftover alphaRef 10/255; the wrapper treats these as world cutouts.
    bool cutout = false;
    // HZ6: binary alpha with any hole at all (cutout is the stricter >= 8%
    // holes). Enough for draws the game itself queues as cutouts (0x80..0xA0):
    // NFS4 hedges have 4-8% holes along one edge.
    bool keyed = false;
};

// M1: mips=true creates every texture with a full mip chain that is regenerated
// on the GPU after each upload; false keeps a single level.
void  Tex_Configure(bool mips);
void  Tex_Init();
void  Tex_Shutdown();
void  Tex_Reset();
void* Tex_Alloc(int width, int height, int format, int flags, int mipLevels);
void  Tex_Free(void* handle);
// palette: the game's third tupdate argument (palette pointer, 0 for the
// advertised formats); only level 0 is uploaded.
void  Tex_Update(void* handle, int palette, void* data);
// HZ4: a texture the wrapper owns (an animation frame), ARGB8888 (BGRA in
// memory), processed like a game upload; freed with Tex_Free / Tex_Reset.
void* Tex_CreateInternal(int w, int h, const void* bgra);
// HITCH1: the next Tex_Update calls carry an image the wrapper generated itself
// (NE1 text sheets) whose clear texels already hold a usable colour, so the
// alpha dilation (40-150 ms on a 1024x1024 sheet) and the material hash
// (byte-wise FNV, ~100 ms on 4 MB) are skipped.
extern "C" void Tex_SetGeneratedUpload(int on);
extern bool g_generatedUpload;
// HITCH1: true while the texture being updated is a streaming one (the same
// slot updated less than 100 ms before); per-update logs stay quiet then.
bool Tex_NoteUpdate(int slot);
extern bool g_texStreamingUpdate;
void  Tex_UpdateRect(void* handle, int mipLevel, int x, int y, int w, int h,
                     int pitch, void* data, int fmt);
void  Tex_Bind(void* handle);
// Handle passed to the last Tex_Bind (diagnostics).
void* Tex_Bound();
// True when the bound texture is a binary colour-key cutout.
bool  Tex_BoundIsCutout();
// HZ6: true when the bound texture has binary alpha with any hole.
bool  Tex_BoundIsKeyed();
// R3: the SRV of a THRASH texture handle (null for 0/unknown/freed).
struct ID3D11ShaderResourceView;
ID3D11ShaderResourceView* Tex_SrvForHandle(void* handle);
// MAT7: slot index of the currently bound texture (-1 when none).
int Tex_BoundSlot();
// Diagnostics: the slot record (null when the slot is free).
const TexSlot* Tex_SlotAt(int slot);
// PERF1: live game textures and their GPU size (32-bit texels, mip chain included).
void Tex_Stats(int* count, double* megabytes);
