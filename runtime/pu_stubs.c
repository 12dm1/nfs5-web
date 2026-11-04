/* nfs5-web: what the copied nfsu2-web runtime still calls into its NFSU2-only
 * parts (D3D9 COM layer, profile hooks, audio clock, autotest).  Each is
 * replaced by the PU subsystem when that exists (WEB1 steps 2-4). */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
extern void (*g_report_fn)(void);   /* mem.c */

/* COM method bridges (0xE0000000..0xEFFFFFFF): nfsu2-web's com.c served
 * Direct3D 9 there.  PU's COM objects (DirectDraw, DirectSound, DirectInput)
 * come with runtime/pu_com.c; until then a call is fatal and says which. */
__attribute__((weak)) void com_call(CPU *c, uint32_t addr){
    fprintf(stderr, "\n*** COM call to bridge 0x%08x: no PU COM layer yet (ret 0x%08x)\n", addr, MR32(c->esp));
    if (g_report_fn) g_report_fn();
    exit(6);
}
/* fileio.c: nfsu2-web's career-garage and save-game logs on profile writes */
__attribute__((weak)) void garage_profile_written(const char *path){ (void)path; }
__attribute__((weak)) void garage_profile_deleted(const char *path){ (void)path; }
__attribute__((weak)) void savegame_profile_written(void){ }
/* log timestamps: nfsu2-web's audio clock; here the monotonic clock in seconds */
__attribute__((weak)) double audio_time(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9;
}
__attribute__((weak)) void autotest_note_trap(uint32_t addr, const char *mn){ (void)addr; (void)mn; }
