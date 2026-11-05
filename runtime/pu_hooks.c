/* nfs5-web: PU's instruction hooks (HOOK_SITE, gen.sh HOOKS=...).  Wait loops need none:
 * GUEST_BACKEDGE (cpu.h) covers them.  0x4D2B80 + 0x4D3D00 are the network autotest's (nettest.c). */
#include "cpu.h"
#include "decls.h"
#include "ext/hs_ai.h"

void nettest_hook(CPU *c, uint32_t ip);

/* The race's video mode: 00467FC0(w, h, bpp, 2) at race start and in network games (PC mod NP1). A new profile asks for
 * 640x480x16, where the LOD selector (00439F90 divides by the width) picks low-poly cars close by; so 32 bits and the
 * picture's own size whatever the settings say. */
static void race_mode(CPU *c){
    const uint32_t a = c->esp;
    MR32(a) = 1280; MR32(a + 4) = 960; MR32(a + 8) = 32;
}

/* AI on the converted tracks (runtime/ext/hs_ai.cpp): the PC wrapper's call patches, as hooks before the same instructions */
static void stuck_check(CPU *c){
    const uint32_t car = MR32(c->esp), sp = c->esp;     /* push esi; call 0040F740: eax, ecx, edx are free here */
    if (!car || !hs_ai_stuck(g_mem + car)) return;
    c->esp -= 4; MR32(c->esp) = 0;
    c->esp -= 4; MR32(c->esp) = 1;
    c->esp -= 4; MR32(c->esp) = car;
    c->esp -= 4; MR32(c->esp) = 0;
    sub_0040f540(c);
    c->esp = sp;
    hs_ai_stuck_done(g_mem + car);
}
void pu_car_lod(CPU *c);
void pu_car_lod_record(CPU *c);
int pu_hook(CPU *c, uint32_t ip){
    switch (ip){
    case 0x43A3B0u: pu_car_lod(c); return 1;
    case 0x43C431u: pu_car_lod_record(c); return 1;
    case 0x40B6FDu: ST(0) *= hs_ai_speed_lookahead(); return 1;         /* fstp [ebp+8]: the slices the planner looks ahead */
    case 0x40BA91u: ST(0) *= hs_ai_zone(g_mem + c->esi); return 1;      /* after fmulp: the bend severity */
    case 0x40BAF0u: hs_ai_target(g_mem + c->esi, (int)MR32(c->ebp + 8)); return 1;     /* after fstp [esi+EB8] */
    case 0x40C865u: hs_ai_guard(g_mem + c->esi, (float *)(g_mem + MR32(c->esp + 4)), (const float *)(g_mem + MR32(c->esp + 8))); return 1;
    case 0x40C8DAu: hs_ai_throttle(g_mem + c->esi); return 1;
    case 0x41041Eu: stuck_check(c); return 1;
    case 0x4D2B80u: case 0x4D3D00u: nettest_hook(c, ip); return 1;
    case 0x41562Eu: case 0x4B6CB9u: race_mode(c); return 1;
    }
    return 0;
}
