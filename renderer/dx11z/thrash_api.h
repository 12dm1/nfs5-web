/*
 * THRASH API — EA's Graphics Abstraction Layer (NFS5 Porsche Unleashed)
 *
 * Reconstructed from PORSCHE.EXE v3.5 decompilation.
 * The EXE loads a driver DLL (dx7z.dll / voodoo2z.dll) at runtime via
 * LoadLibrary + GetProcAddress and stores function pointers at 0x006BD910-0x006BD9B8.
 *
 * Loader function: set3d.c::THRASH_opendll @ 0x00574FA0
 * DLL handle stored at: 0x0069E5E8
 *
 * All functions use __stdcall calling convention (indicated by @N suffix in exports).
 * A replacement DLL must export all 45 functions with exact decorated names.
 */

#ifndef THRASH_API_H
#define THRASH_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define THRASH_API __stdcall

/* ============================================================
 * THRASH State IDs — used with setstate(id, value) / getstate(id)
 *
 * Extracted from 167 setstate + 46 getstate call sites in PORSCHE.EXE.
 * Names are inferred from context (surrounding code, NFS3/NFS4 RE knowledge).
 * ============================================================ */

typedef enum {
    /* Camera/view state */
    THRASH_STATE_CLEAR_COLOR            = 0x03,   /* packed ARGB used by clearwindow */

    /* Glide sampling / fog states, from dx7z.dll's setstate jump table
     * (60002C7C, index table 600038E0) and its GetProcAddress'd Glide
     * imports: */
    THRASH_STATE_CULL_MODE              = 0x02,   /* dx7z software cull: 0 none, 2 -> D3D CULL_FRONT, 1 -> CULL_BACK (verified in-game) */
    THRASH_STATE_TEXTURE_FILTER         = 0x07,   /* grTexFilterMode(tmu, v, v): 0 point, 1 bilinear */
    THRASH_STATE_TEXTURE_LOD_BIAS       = 0x08,   /* grTexLodBiasValue(tmu, float bits) */
    THRASH_STATE_FOG_DENSITY            = 0x0E,   /* float density (or 1/n for n < 0x1000); exp table via guFogGenerateExp */
    THRASH_STATE_FOG_COLOR_GLIDE        = 0x0F,   /* grFogColorValue(ARGB) */
    THRASH_STATE_FOG_MODE_GLIDE         = 0x15,   /* 0 GR_FOG_DISABLE, 1 GR_FOG_WITH_TABLE_ON_Q, 2/4/8 table generator */
    THRASH_STATE_DEPTH_BIAS             = 0x18,   /* grDepthBiasLevel(level) */
    THRASH_STATE_BLEND_MODE             = 0x38,   /* grAlphaBlendFunction: 0 SrcA/InvSrcA, 1 SrcA/One, 2 Zero/InvSrcA, 3 DstColor/Zero */
    THRASH_STATE_TEXTURE_CLAMP_S        = 0x41,   /* grTexClampMode S: 0 clamp, 1 wrap, 2 mirror */
    THRASH_STATE_TEXTURE_CLAMP_T        = 0x42,   /* grTexClampMode T: 0 clamp, 1 wrap, 2 mirror */

    /* Texture/filtering states */
    THRASH_STATE_TEXTURE_FILTER_MODE    = 0x0B,   /* set+get: texture filtering (point/bilinear) */
    THRASH_STATE_MIP_FILTER_MODE        = 0x0C,   /* set+get: mipmap filtering */
    THRASH_STATE_PERSPECTIVE_CORRECT    = 0x0D,   /* set: perspective correction on/off */

    /* Alpha/transparency states */
    THRASH_STATE_ALPHA_BLEND_SRC        = 0x12,   /* no dx7z handler (jump table default) */
    THRASH_STATE_ALPHA_BLEND_DST        = 0x13,   /* no dx7z handler (jump table default) */

    /* Depth buffer: verified through dx7z.dll's setstate jump table. */
    THRASH_STATE_ZBUFFER_MODE           = 0x04,   /* 0: ALWAYS/no writes; 1: Z; 2: W */
    THRASH_STATE_ZBUFFER_COMPARE        = 0x28,   /* Glide comparison enum 0..7 */

    /* Rendering modes */
    THRASH_STATE_RENDER_MODE            = 0x1F,   /* set: render mode (init sets 0x6B) */
    THRASH_STATE_ALPHA_TEST_REF         = 0x24,   /* 0: disabled; otherwise alpha >= low byte */
    THRASH_STATE_DITHER_MODE            = 0x25,   /* set: dithering on/off */
    THRASH_STATE_FOG_MODE               = 0x26,   /* set: fog enable/mode */
    THRASH_STATE_FOG_COLOR              = 0x27,   /* set: fog color (packed RGB) */
    THRASH_STATE_CHROMA_KEY             = 0x29,   /* set+get: chroma key / color key */
    THRASH_STATE_CHROMA_KEY_COLOR       = 0x2A,   /* set: chroma key color value */
    THRASH_STATE_TEXTURE_CLAMP          = 0x2E,   /* set: clamp/wrap in the enum; game often writes float 1.0. Do not treat nonzero as point filter. */
    THRASH_STATE_ZBUFFER_WRITE          = 0x39,   /* grDepthMask; NOT a texture binding */

    /* Resolution/display */
    THRASH_STATE_MAX_TEXTURE_SIZE       = 0x38,   /* get only: max texture dimension (set = blend mode) */
    THRASH_STATE_GAMMA                  = 0x3C,   /* set: gamma correction */
    THRASH_STATE_UNKNOWN_3F             = 0x3F,   /* set: unknown */

    /* Vertex/index format (dx7z globals at 6000C51Ch / 6000C520h) */
    THRASH_STATE_VERTEX_STRIDE          = 0x1E,   /* set: bytes per vertex (32 or 40) */
    THRASH_STATE_INDEX_FORMAT           = 0x46,   /* set: index format (1-4), controls spacing */

    /* Extended states */
    THRASH_STATE_UNKNOWN_6C             = 0x6C,   /* set: unknown */
    THRASH_STATE_SCREEN_WIDTH           = 0x12F,  /* set+get: screen width in pixels */
    THRASH_STATE_SCREEN_HEIGHT          = 0x132,  /* set+get: screen height in pixels */
    THRASH_STATE_UNKNOWN_1B3            = 0x1B3,  /* set: unknown */
    THRASH_STATE_UNKNOWN_1C3            = 0x1C3,  /* set: unknown */
    THRASH_STATE_WINDOW_HANDLE          = 0x10029 /* set: HWND of render window */
} THRASH_StateID;


/* ============================================================
 * THRASH_about return structure
 *
 * The loader checks: magic == 0x33444658 ("3DFX") or 0x33444632 ("3D2")
 * to determine if it's a valid THRASH driver.
 * ============================================================ */

/* About struct is 0xD0 (208) bytes per original dx7z.dll.
 * Key fields found via RE of dx7z.dll's THRASH_about: */
typedef struct {
    uint32_t data[52];      /* 208 bytes = 52 dwords, accessed by offset */
} THRASH_AboutInfo;

/* PORSCHE.EXE::FUN_00572a00 walks entries 1..about[0x3C/4], inclusive,
 * with a 0x28-byte stride. Entry 0 is a reserved, unavailable sentinel.
 * FUN_00572990 checks available and buffer counts as well as width/height/bpp. */
typedef struct {
    uint32_t width, height, bpp, format;
    uint32_t available, colorBuffers, buffersWithDepth;
    uint32_t reserved[3];
} THRASH_VideoMode;


/* ============================================================
 * Vertex structure (inferred from draw function signatures)
 *
 * Used by drawtri(@12 = 3 ptrs), drawquad(@16 = 4 ptrs),
 * drawline(@8 = 2 ptrs), etc.
 * ============================================================ */

typedef struct {
    float x, y, z;          /* screen-space position (after transform) */
    float rhw;              /* 1/w for perspective correction */
    uint32_t color;         /* ARGB diffuse color (D3DTLVERTEX order) */
    uint32_t specular;      /* specular / fog factor */
    float u, v;             /* texture coordinates */
} THRASH_Vertex;


/* ============================================================
 * Texture handle (opaque, returned by talloc, consumed by settexture)
 * ============================================================ */

typedef void* THRASH_Texture;

/* lockwindow returns a descriptor, NOT a pointer to the pixels themselves.
 * Verified in PORSCHE.EXE::FUN_00534360 and dx7z.dll @ 0x60003C41.
 * This is a 28-byte structure in the game's 32-bit ABI. */
typedef struct {
    void* pixels;          /* +0x00: linear framebuffer address */
    uint32_t pitch;        /* +0x04: bytes per row */
    uint32_t format;       /* +0x08: THRASH format (4 = RGB565) */
    uint32_t width;        /* +0x0C */
    uint32_t height;       /* +0x10 */
    uint32_t buffer;       /* +0x14: backend buffer selector */
    uint32_t access;       /* +0x18: bit 0 = read, bit 1 = write */
} THRASH_LockedWindow;


/* ============================================================
 * API Function Declarations
 *
 * 45 exported functions, all __stdcall.
 * Grouped by purpose.
 *
 * DAT_xxxxxxxx = address of function pointer in PORSCHE.EXE
 * @N = stack bytes consumed (stdcall parameter size)
 * ============================================================ */

/* --- Identification & lifecycle --- */

/* Returns pointer to THRASH_AboutInfo. Magic must be 0x33444658 or 0x33444632. */
/* DAT_006BD934 */
THRASH_AboutInfo* THRASH_API THRASH_about(void);

/* Initialize the driver. Called after about() succeeds. */
/* DAT_006BD95C */
int THRASH_API THRASH_init(void);

/* Query driver capabilities/identity */
/* DAT_006BD990 */
int THRASH_API THRASH_is(void);

/* Shut down and release resources */
/* DAT_006BD938 */
int THRASH_API THRASH_restore(void);


/* --- Display setup --- */

/* Select which display adapter to use */
/* DAT_006BD9B8 */
int THRASH_API THRASH_selectdisplay(int display_index);

/* Record the in-game video setting from the about() list.
 * arg0 = index into about()->modeList (not pixels).
 * arg1/arg2 are Glide buffer/refresh selectors; the game always passes 2 and 1 or 2.
 * This does not change the DXGI swap chain. Output size comes from dx11z.ini
 * or the exclusive-fullscreen desktop mode. Menu drawing stays 640x480.
 * DAT_006BD958 */
int THRASH_API THRASH_setvideomode(int mode_index, int buffers, int extra);


/* --- Render state --- */

/* Set a render state value */
/* DAT_006BD97C  (aliases: DAT_006BD918, DAT_006BD920) */
int THRASH_API THRASH_setstate(int state_id, int value);

/* Get a render state value */
/* DAT_006BD984  (aliases: DAT_006BD9A0, DAT_006BD998) */
int THRASH_API THRASH_getstate(int state_id);


/* --- Window management --- */

/* Set render target window (HWND or internal handle) */
/* DAT_006BD9B0 */
int THRASH_API THRASH_window(int window_handle);

/* Create a render window/surface: x, y, width, height */
/* (not stored in function pointer table — may be in DLL only) */
int THRASH_API THRASH_createwindow(int x, int y, int width, int height);

/* Destroy render window */
int THRASH_API THRASH_destroywindow(int window_handle);

/* Get texture from window (render-to-texture) */
int THRASH_API THRASH_getwindowtexture(int window_handle);

/* Clear the render window */
/* DAT_006BD91C */
void THRASH_API THRASH_clearwindow(void);

/* Flush pending draw commands */
/* DAT_006BD970 */
void THRASH_API THRASH_flushwindow(void);

/* Lock the window surface; the returned descriptor contains the pixel pointer. */
/* DAT_006BD964 */
THRASH_LockedWindow* THRASH_API THRASH_lockwindow(void);

/* Unlock the window surface */
/* DAT_006BD92C */
void THRASH_API THRASH_unlockwindow(THRASH_LockedWindow* surface);

/* Present / swap buffers */
/* DAT_006BD948 */
void THRASH_API THRASH_pageflip(void);

/* Set clipping rectangle: x1, y1, x2, y2 */
/* DAT_006BD9B4 */
void THRASH_API THRASH_clip(int x1, int y1, int x2, int y2);

/* Wait for sync (vsync or frame completion) */
/* DAT_006BD978 */
void THRASH_API THRASH_sync(int flags);

/* Called during idle */
void THRASH_API THRASH_idle(void);


/* --- Texture management --- */

/* Allocate a texture: width, height, format, flags, miplevels */
/* DAT_006BD968 */
THRASH_Texture THRASH_API THRASH_talloc(int width, int height, int format, int flags, int mip_levels);

/* Free a texture */
/* DAT_006BD94C */
void THRASH_API THRASH_tfree(THRASH_Texture texture);

/* Upload texture data: texture, pixel_data, palette (0 unless the texture class
 * is palettised — DTEX_select FUN_005554b0 passes flags&8 ? dtex+0x1C : 0).
 * Only level 0 is ever uploaded by the game. */
/* DAT_006BD940 — verified arg order from decompiled call site in FUN_004dc610 */
void THRASH_API THRASH_tupdate(THRASH_Texture texture, void* data, int palette);

/* Upload partial texture data */
void THRASH_API THRASH_tupdaterect(THRASH_Texture texture, int mip, int x, int y, int w, int h,
                                    int pitch, void* data, int format);

/* Reset all textures (free texture memory) */
/* DAT_006BD988 */
void THRASH_API THRASH_treset(void);

/* Bind a texture for rendering */
/* DAT_006BD954 */
void THRASH_API THRASH_settexture(THRASH_Texture texture);


/* --- Drawing primitives --- */

/* All draw functions take pointers to THRASH_Vertex (or arrays).
 * The @N suffix indicates stack size:
 *   @4  = 1 pointer (point)
 *   @8  = 2 pointers (line, sprite) or (count, array)
 *   @12 = 3 pointers (tri) or (mesh: count, indices, verts)
 *   @16 = 4 pointers (quad)
 */

/* DAT_006BD928 */
void THRASH_API THRASH_drawtri(THRASH_Vertex* v0, THRASH_Vertex* v1, THRASH_Vertex* v2);

/* DAT_006BD9AC — game passes (count, vertices, indices) */
void THRASH_API THRASH_drawtrimesh(int count, THRASH_Vertex* vertices, void* indices);

/* DAT_006BD98C */
void THRASH_API THRASH_drawtrifan(int count, THRASH_Vertex* vertices, void* indices);

/* DAT_006BD99C — @12 variant */
void THRASH_API THRASH_drawtristrip(int count, THRASH_Vertex* vertices, void* indices);

/* DAT_006BD9A8 */
void THRASH_API THRASH_drawquad(THRASH_Vertex* v0, THRASH_Vertex* v1,
                                 THRASH_Vertex* v2, THRASH_Vertex* v3);

/* DAT_006BD944 */
void THRASH_API THRASH_drawquadmesh(int count, THRASH_Vertex* vertices, void* indices);

/* DAT_006BD950 */
void THRASH_API THRASH_drawline(THRASH_Vertex* v0, THRASH_Vertex* v1);

/* DAT_006BD93C */
void THRASH_API THRASH_drawlinemesh(int count, THRASH_Vertex* vertices, void* indices);

/* DAT_006BD994 — @12 variant */
void THRASH_API THRASH_drawlinestrip(int count, THRASH_Vertex* vertices, void* indices);

/* DAT_006BD924 */
void THRASH_API THRASH_drawpoint(THRASH_Vertex* v);

/* DAT_006BD960 */
void THRASH_API THRASH_drawpointmesh(int count, THRASH_Vertex* vertices, void* indices);

/* DAT_006BD974 */
void THRASH_API THRASH_drawsprite(THRASH_Vertex* v0, THRASH_Vertex* v1);

/* DAT_006BD910 */
void THRASH_API THRASH_drawspritemesh(int count, THRASH_Vertex* vertices, void* indices);


/* --- Pixel buffer operations --- */

/* Read rectangle from framebuffer: x, y, w, h, dest_buffer; returns nonzero
 * on success. The lens flare (FUN_00460290 / FUN_00460460) draws the sun,
 * reads the 4x4 pixels under it as a reference and re-reads them every 200 ms
 * to test occlusion; the pixels are in the window format (RGB565, w*2 pitch). */
/* DAT_006BD96C */
int THRASH_API THRASH_readrect(int x, int y, int w, int h, void* buffer);

/* Write rectangle to framebuffer: x, y, w, h, src_buffer */
/* DAT_006BD930 */
void THRASH_API THRASH_writerect(int x, int y, int w, int h, void* buffer);


/* ============================================================
 * Overloaded variants (8-byte versions — count + vertex array)
 *
 * Some draw functions have @8 variants that take (count, vertex_array)
 * instead of (count, indices, vertex_array).
 * These are NOT loaded by the EXE's loader but exist in the DLLs.
 * ============================================================ */

/* These are in DLL exports but not loaded by PORSCHE.EXE:
 *   _THRASH_drawlinestrip@8
 *   _THRASH_drawpointstrip@8
 *   _THRASH_drawtrifan@8
 *   _THRASH_drawtristrip@8
 */


#ifdef __cplusplus
}
#endif

#endif /* THRASH_API_H */
