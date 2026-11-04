/* nfs5-web: PU imports and driver entry points that nfsu2-web's win32.c does
 * not serve.  win32_call asks pu_call() before its generic layers; a 1 means
 * handled (*r = the return value; win32_call pops the stdcall arguments). */
#include "cpu.h"
#include <stdio.h>
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
    if (!strcasecmp(d, "gimme.dll") && !strcmp(n, "ConvertFile")){   /* driver picture -> FSH: not yet (WEB2+) */
        fprintf(stderr, "[pu] gimme ConvertFile(\"%s\") -> 0 (not converted)\n", (const char *)(g_mem + MR32(c->esp)));
        *r = 0; return 1; }
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
