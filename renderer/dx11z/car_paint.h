#pragma once
#include <cstdint>

struct ID3D11ShaderResourceView;

// CP1: the paint shader (CAR2, the 959 look) on EVERY car without per-car
// packs. The game tints a car's page while it composes it (00444130, called
// from the page builder 00443F60): a grey source image texel whose palette
// alpha names a colour band becomes band colour x source red. A hook on both
// calls records per page texel the band and the source red; when the page is
// uploaded the record is checked texel by texel against the uploaded data
// (racing numbers, the licence plate and anything else drawn later fall out)
// and becomes a paint map (R8G8: R = body / stripe paint, G = keep x R, keep =
// source red / the band's panel level, the peak of its red histogram on the
// page). Interior colour bands (14, 15) are not paint.
struct CarPaintConfig {
    bool enabled = false;       // [car_paint] enable
    bool log = false;           // [car_paint] log: page matches and misses
    unsigned files = 1;         // [car_paint] files: bit mask of the builder's file index (bit 0 = exterior)
};

void CarPaint_Configure(const CarPaintConfig& cfg);
bool CarPaint_Enabled();
// Patches the two blit calls in the page builder (after the image identity check).
bool CarPaint_Install();
// Tex_Update hook: raw page data as the game uploads it (before conversion).
void CarPaint_OnUpload(int slot, const void* data, int w, int h, int fmt, const char* name);
// Diagnostics: a partial update of a slot (Tex_UpdateRect).
void CarPaint_OnUpdateRect(int slot, int x, int y, int w, int h);
// The slot's texture is gone (Tex_Free / Tex_Reset).
void CarPaint_OnFree(int slot);
void CarPaint_OnResetAll();
// The paint map of a texture slot, null when it has none.
ID3D11ShaderResourceView* CarPaint_SlotSrv(int slot);
// CP3 diagnostics: one car draw (the slot it textures with, shaded by the
// material path or not, corners with / without a car normal) and the frame
// tick that logs the per-slot status line.
void CarPaint_NoteCarDraw(int slot, int tris, bool shaded, int withNormal, int withoutNormal);
void CarPaint_OnFrame(uint32_t frame);
// Diagnostics: pages recorded / matched / missed so far.
void CarPaint_Stats(uint32_t* recorded, uint32_t* matched, uint32_t* missed);

// Pure part, shared with the tests: band of a palette alpha (-1 = untinted)
// and the raw texel the blit writes for a tinted palette entry.
int      CarPaint_BandOfAlpha(uint8_t alpha);
uint32_t CarPaint_TintedTexel(uint32_t palette, uint32_t bandColour, int fmt);
