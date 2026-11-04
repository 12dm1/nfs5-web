/* nfs5-web: PU's instruction hooks (HOOK_SITE, gen.sh HOOKS=...).  None at
 * the moment: the wait loops that needed one (0x533CB1, the tick wait) are
 * covered by GUEST_BACKEDGE (cpu.h) on every backward branch. */
#include "cpu.h"

int pu_hook(CPU *c, uint32_t ip){
    (void)c;
    switch (ip){
    }
    return 0;
}
