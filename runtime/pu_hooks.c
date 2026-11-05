/* nfs5-web: PU's instruction hooks (HOOK_SITE, gen.sh HOOKS=...).  Wait loops need none:
 * GUEST_BACKEDGE (cpu.h) covers them.  0x4D2B80 + 0x4D3D00 are the network autotest's (nettest.c). */
#include "cpu.h"

void nettest_hook(CPU *c, uint32_t ip);

/* The race's video mode: 00467FC0(w, h, bpp, 2) at race start and in network games (PC mod NP1). A new profile asks for
 * 640x480x16, where the LOD selector (00439F90 divides by the width) picks low-poly cars close by; so 32 bits and the
 * picture's own size whatever the settings say. */
static void race_mode(CPU *c){
    const uint32_t a = c->esp;
    MR32(a) = 1280; MR32(a + 4) = 960; MR32(a + 8) = 32;
}

int pu_hook(CPU *c, uint32_t ip){
    switch (ip){
    case 0x4D2B80u: case 0x4D3D00u: nettest_hook(c, ip); return 1;
    case 0x41562Eu: case 0x4B6CB9u: race_mode(c); return 1;
    }
    return 0;
}
