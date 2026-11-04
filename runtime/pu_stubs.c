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

/* ---- nfsu2-web subsystems PU does not have (or has later) ----------------- */
#include "module.h"
typedef struct { uint32_t iat; const char*dll; const char*name; int nargs; } ImpEnt;
typedef void (*mod_fn)(CPU*);
/* server.dll (SPEED2's LAN server, lifted as a second module): none in PU */
const ImpEnt g_srv_imp[1]; const uint32_t g_srv_imp_count = 0;
const uint32_t g_srv_fn_addr[1]; const mod_fn g_srv_fn_ptr[1]; const uint32_t g_srv_fn_count = 0;
const ModInfo g_srv_modinfo = { .name = "" };   /* empty name = no module in this slot */
/* winsock.c (Winsock HLE + host sockets): WEB2n.  Until then Winsock calls
 * fall through to the generic "unimplemented import" path (logged). */
__attribute__((weak)) int winsock_call(CPU *c, const char *dll, const char *n, uint32_t *r){ (void)c; (void)dll; (void)n; (void)r; return 0; }
__attribute__((weak)) void winsock_report(void){ }
__attribute__((weak)) int winsock_session_active(void){ return 0; }
/* audio host (nfsu2-web: audio.m / the AudioWorklet): WEB1 is silent */
__attribute__((weak)) void audio_set_muted(int on){ (void)on; }
/* autotest.c, discord.c, overrides.c, language.c, framerate.c, roam.c, keyboard.c, d3dx_hle.c */
__attribute__((weak)) long autotest_frame(void){ return 0; }
__attribute__((weak)) void autotest_note_unimpl(const char *dll, const char *n, uint32_t ra){ (void)dll; (void)n; (void)ra; }
__attribute__((weak)) void discord_lifecycle_changed(void){ }
__attribute__((weak)) void discord_shutdown(void){ }
__attribute__((weak)) void overrides_report(void){ }
__attribute__((weak)) const char *language_registry(void){ return "English"; }
__attribute__((weak)) int hfr_mode_off(void){ return 1; }
__attribute__((weak)) uint64_t hfr_vclock_floor_us(void){ return 0; }
__attribute__((weak)) void roam_game_parked(int on){ (void)on; }
__attribute__((weak)) void kbd_pump(void){ }
__attribute__((weak)) void d3dx_hle_report(void){ }
/* window host (nfsu2-web: metal.m; WEB2: dx11z window).  Frames and the game
 * size come from thrash_hle.c. */
__attribute__((weak)) void metal_get_cursor(int *x, int *y){ *x = 320; *y = 240; }
__attribute__((weak)) void metal_mem_stats(uint64_t *a, unsigned *lt, unsigned *ts, uint64_t *tb){ *a = 0; *lt = 0; *ts = 0; *tb = 0; }
__attribute__((weak)) void metal_request_quit(void){ }
