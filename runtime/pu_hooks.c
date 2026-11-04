/* nfs5-web: PU's instruction hooks (HOOK_SITE, gen.sh HOOKS=...).
 *
 * 0x533CB1: the body of PU's "wait for a job" loop (sub_00533C20:
 * while (!done) sub_00567F70(job)).  It is pure guest code, no import call,
 * so the guest lock was never handed over: the main thread spun inside it
 * while the thread that finishes the job waited for the lock (the "6 frames"
 * hang, ~1 run in 4).  Windows would have preempted it; this is the yield. */
#include "cpu.h"

void guest_fair_yield(void);

int pu_hook(CPU *c, uint32_t ip){
    (void)c;
    switch (ip){
    case 0x00533CB1u: guest_fair_yield(); return 1;
    }
    return 0;
}
