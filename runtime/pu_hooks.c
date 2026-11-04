/* nfs5-web: PU's instruction hooks (HOOK_SITE, gen.sh HOOKS=...).  Wait loops need none:
 * GUEST_BACKEDGE (cpu.h) covers them.  The two sites are the network autotest's (nettest.c). */
#include "cpu.h"

void nettest_hook(CPU *c, uint32_t ip);

int pu_hook(CPU *c, uint32_t ip){
    switch (ip){
    case 0x4D2B80u: case 0x4D3D00u: nettest_hook(c, ip); return 1;
    }
    return 0;
}
