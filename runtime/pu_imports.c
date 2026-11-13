/* nfs5-web: PU imports and driver entry points that nfsu2-web's win32.c does
 * not serve.  win32_call asks pu_call() before its generic layers; a 1 means
 * handled (*r = the return value; win32_call pops the stdcall arguments). */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct { uint32_t iat; const char*dll; const char*name; int nargs; } ImpEnt;
int thrash_call(CPU *c, const char *name, uint32_t *r);
uint32_t pu_directinputcreate(uint32_t ver, uint32_t pp);
uint32_t pu_directsoundcreate(uint32_t pp);
uint32_t pu_directsoundcapturecreate(uint32_t pp);
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
uint32_t com_scratch(uint32_t n);

#define ARG(i) MR32(c->esp + 4u * (uint32_t)(i))
#include "pu_imports2.inc"

/* ---- the driver picture (PC mod CF1 driver_picture.cpp): what the community's gimme.dll stub writes, an SHPI file with
 * one RGB565 (0x78) entry from entry `index` of the source (8-bit 0x7B with a 0x29/0x2D/0x24/0x2A palette, or 0x78).
 * The Internet mode sends it to the other players (file sharing) and their race loading screen draws it. ---- */
int fileio_resolve_save(const char *win, char *out, size_t outsz);
int fileio_resolve_save_w(const char *win, char *out, size_t outsz);
static uint32_t dp_u32(const uint8_t *p){ uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t dp_u16(const uint8_t *p){ uint16_t v; memcpy(&v, p, 2); return v; }
static uint16_t dp_565(unsigned r, unsigned g, unsigned b){ return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }
static int dp_colour(const uint8_t *pal, uint8_t type, unsigned i, uint16_t *out){
    switch (type){
    case 0x29: *out = dp_u16(pal + i * 2); return 1;
    case 0x2D: { const unsigned v = dp_u16(pal + i * 2), g5 = (v >> 5) & 31;
                 *out = (uint16_t)((((v >> 10) & 31) << 11) | (((g5 << 1) | (g5 >> 4)) << 5) | (v & 31)); return 1; }
    case 0x24: *out = dp_565(pal[i * 3 + 2], pal[i * 3 + 1], pal[i * 3]); return 1;
    case 0x2A: *out = dp_565(pal[i * 4 + 2], pal[i * 4 + 1], pal[i * 4]); return 1;
    default: return 0;
    }
}
static int dp_make(const uint8_t *d, size_t n, int index, const char *destination, const char **why){
    if (n < 16 || memcmp(d, "SHPI", 4)){ *why = "not an SHPI file"; return 0; }
    const uint32_t count = dp_u32(d + 8);
    if (dp_u32(d + 4) != n || index < 0 || (uint32_t)index >= count || 16 + (size_t)count * 8 + 8 > n){ *why = "bad header or index"; return 0; }
    const size_t dir = 16 + (size_t)index * 8;
    const uint32_t offset = dp_u32(d + dir + 4);
    if ((size_t)offset + 16 > n){ *why = "entry outside the file"; return 0; }
    const uint32_t head = dp_u32(d + offset);
    const uint8_t type = (uint8_t)head;
    const uint32_t next = head >> 8;
    const unsigned width = dp_u16(d + offset + 4), height = dp_u16(d + offset + 6);
    const size_t pixels = (size_t)width * height;
    if (!pixels || pixels > 4096u * 4096u){ *why = "bad size"; return 0; }
    const uint32_t total = (uint32_t)(0x30 + pixels * 2);
    uint8_t *out = calloc(1, total);
    if (!out){ *why = "no memory"; return 0; }
    uint16_t *rgb = (uint16_t *)(out + 48);            /* 48 is 2-aligned */
    int ok = 0;
    if (type == 0x7B){
        const size_t pal = (size_t)offset + (next ? next : 16 + pixels);     /* the palette follows the pixels */
        if ((size_t)offset + 16 + pixels > n){ *why = "pixels outside the file"; goto done; }
        if (pal + 16 > n){ *why = "no palette"; goto done; }
        const uint8_t pt = d[pal];
        const unsigned colours = dp_u16(d + pal + 4), es = pt == 0x24 ? 3 : pt == 0x2A ? 4 : 2;
        if (!colours || colours > 256 || pal + 16 + colours * es > n){ *why = "bad palette"; goto done; }
        uint16_t lut[256] = { 0 };
        for (unsigned i = 0; i < colours; i++) if (!dp_colour(d + pal + 16, pt, i, &lut[i])){ *why = "unknown palette format"; goto done; }
        for (size_t i = 0; i < pixels; i++) rgb[i] = lut[d[offset + 16 + i]];
    } else if (type == 0x78){
        if ((size_t)offset + 16 + pixels * 2 > n){ *why = "pixels outside the file"; goto done; }
        memcpy(rgb, d + offset + 16, pixels * 2);
    } else { *why = "unsupported pixel format"; goto done; }
    {   /* header 16 + directory 8 + the 8 bytes after the directory + entry header 16 */
        const uint32_t one = 1, entry = 0x20, oh = 0x78;
        const uint16_t w = (uint16_t)width, h = (uint16_t)height;
        memcpy(out, "SHPI", 4); memcpy(out + 4, &total, 4); memcpy(out + 8, &one, 4);
        memcpy(out + 12, d + 12, 4);                      /* directory id */
        memcpy(out + 16, d + dir, 4);                     /* the source entry's tag */
        memcpy(out + 20, &entry, 4);
        memcpy(out + 24, d + 16 + (size_t)count * 8, 8);
        memcpy(out + 32, &oh, 4); memcpy(out + 36, &w, 2); memcpy(out + 38, &h, 2);
        char path[1024];
        path[0] = 0; fileio_resolve_save_w(destination, path, sizeof path);   /* 0 for a file that does not exist yet: the path is still right */
        FILE *o = path[0] ? fopen(path, "wb") : 0;
        if (!o){ *why = "destination not writable"; goto done; }
        ok = fwrite(out, 1, total, o) == total;
        if (fclose(o) || !ok){ ok = 0; *why = "write failed"; }
    }
done:
    free(out);
    return ok;
}
static int driver_picture_convert(const char *source, const char *destination, int index, const char **why){
    char path[1024];
    FILE *f = fileio_resolve_save(source, path, sizeof path) ? fopen(path, "rb") : 0;
    if (!f){ *why = "source not readable"; return 0; }
    fseek(f, 0, SEEK_END); const long size = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = size > 0 && size <= 16L * 1024 * 1024 ? malloc((size_t)size) : 0;
    const int got = d && fread(d, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    const int ok = got ? dp_make(d, (size_t)size, index, destination, why) : (*why = "source not readable", 0);
    free(d);
    return ok;
}

int pu_call(CPU *c, const ImpEnt *e, uint32_t *r){
    const char *n = e->name, *d = e->dll;
    if (n[0] == '_' && thrash_call(c, n, r)) return 1;
    if (!strcasecmp(d, "DINPUT.dll") && !strcmp(n, "DirectInputCreateA")){ *r = pu_directinputcreate(ARG(1), ARG(2)); return 1; }
    if (!strcasecmp(d, "DSOUND.dll")){
        if (!strcmp(n, "#1")){ *r = pu_directsoundcreate(ARG(1)); return 1; }          /* DirectSoundCreate */
        if (!strcmp(n, "#6")){ *r = pu_directsoundcapturecreate(ARG(1)); return 1; }   /* DirectSoundCaptureCreate */
    }
    if (!strcasecmp(d, "DDRAW.dll")){
        if (!strcmp(n, "DirectDrawEnumerateA")){
            /* one display: (lpGUID NULL, description, driver name, context) */
            static uint32_t desc, name;
            if (!desc){ desc = com_scratch(32); name = com_scratch(16);
                        strcpy((char *)(g_mem + desc), "Primary Display Driver"); strcpy((char *)(g_mem + name), "display"); }
            uint32_t args[4] = { 0, desc, name, ARG(1) };
            call_guest(c, ARG(0), 4, args);
            *r = 0; return 1; }
        if (!strcmp(n, "DirectDrawCreate")){
            fprintf(stderr, "[pu] DirectDrawCreate: no DirectDraw (THRASH draws) -> DDERR_GENERIC\n");
            if (ARG(1)) MR32(ARG(1)) = 0;
            *r = 0x80004005u; return 1; }
    }
    if (!strcasecmp(d, "GDI32.dll") && !strcmp(n, "GetDeviceCaps")){
        /* the desktop the game starts on: 32-bit colour (PU refuses < 8 bpp) */
        unsigned w, h; void metal_game_size(unsigned*, unsigned*); metal_game_size(&w, &h);
        switch (ARG(1)){
            case 8: *r = w; break;   case 10: *r = h; break;          /* HORZRES, VERTRES */
            case 12: *r = 32; break; case 14: *r = 1; break;          /* BITSPIXEL, PLANES */
            case 24: *r = 0xFFFFFFFFu; break;                          /* NUMCOLORS: > 256 */
            case 88: case 90: *r = 96; break;                         /* LOGPIXELSX/Y */
            case 116: *r = 60; break;                                 /* VREFRESH */
            default: *r = 0; }
        return 1; }
    if (!strcasecmp(d, "gimme.dll") && !strcmp(n, "ConvertFile")){   /* driver picture -> FSH (cdecl: the caller pops) */
        const char *src = (const char *)(g_mem + MR32(c->esp)), *dst = (const char *)(g_mem + MR32(c->esp + 4));
        const char *why = "";
        *r = (uint32_t)driver_picture_convert(src, dst, (int)MR32(c->esp + 8), &why);
        fprintf(stderr, "[pu] driver picture %s #%d -> %s: %s\n", src, (int)MR32(c->esp + 8), dst, *r ? "written" : why);
        return 1; }
    if (pu_call_misc(c, d, n, r)) return 1;
    if (!strcasecmp(d, "mrbupd.dll")){ *r = 0; return 1; }   /* EA's auto-update DLL: nothing to update */
    if (!strcasecmp(d, "USER32.dll")){
        if (!strcmp(n, "CharUpperA")){             /* string in place (returns it) or one character */
            uint32_t p = ARG(0);
            if (p > 0xFFFFu){ for (uint32_t q = p; MR8(q); q++) if (MR8(q) >= 'a' && MR8(q) <= 'z') MR8(q) -= 32; *r = p; }
            else *r = (p >= 'a' && p <= 'z') ? p - 32 : p;
            return 1; }
        if (!strcmp(n, "ScreenToClient") || !strcmp(n, "ClientToScreen")){ *r = 1; return 1; }   /* window at 0,0 */
        if (!strcmp(n, "GetDC") || !strcmp(n, "GetWindowDC")){ *r = 0x00D00001u; return 1; }   /* one screen DC */
        if (!strcmp(n, "ReleaseDC")){ *r = 1; return 1; }
        if (!strcmp(n, "GetKeyboardType")){ uint32_t t = ARG(0); *r = t == 0 ? 4u : t == 2 ? 12u : 0u; return 1; }
    }
    return 0;
}
