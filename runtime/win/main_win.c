/* nfs5-web: native Windows x64 entry point (WEB1).  After nfsu2-web's
 * tests/boot.c (macOS) and runtime/web/main_web.c (browser):
 * memory, the PE image, TEB/PEB, file roots, dispatch, win32; then the
 * guest's entry point on its own thread.
 *
 *   nfs5_native.exe <out/PORSCHE.EXE> [game folder]
 *
 * The EXE is the patched copy gen.sh made; the game folder (default: the
 * EXE's folder) is where the game's files are read from (fileio_set_root).
 * Environment as in nfsu2-web: VERBOSE / TRACE_MAX (import call trace),
 * BT (backtraces), ALARM=<s> (time limit, default 30 s; 0 = none). */
#include "cpu.h"
#include "decls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <pthread.h>
#include <windows.h>

extern jmp_buf g_exit_jmp; extern int g_exit_code, g_exit_armed; extern int g_bt_on;
void dispatch_init(void); void dispatch_report(void);
void win32_init(void); void win32_report(void);
void fileio_set_root(const char*);
void fileio_set_write_root(const char*);
void lifecycle_guest_exited(int code); int lifecycle_drain_writes(double max_s);
int win32_freeze_guest(int max_ms);
void guest_lock(CPU*); void guest_unlock(void);
void win32_set_trace_max(uint32_t);
extern uint64_t g_hook_hits, g_gs_fail;
extern uint64_t g_stat_indirect_ok, g_stat_indirect_miss, g_stat_unsupported, g_win32_calls;
extern void (*g_report_fn)(void);

/* PU's entry point: OptionalHeader.AddressOfEntryPoint 0x1A2D1A (the CRT's WinMainCRTStartup) */
#define PU_ENTRY   0x005A2D1Au
#define TEB_BASE   0x00C00000u
#define PEB_BASE   0x00C10000u
#define STACK_LO   0x7F000000u
#define STACK_HI   0x7FF00000u

static CPU cpu; static CPU *const c = &cpu;

static void all_reports(void){
    static int done; if (done) return; done = 1;
    win32_report();
    dispatch_report();
    fflush(stderr);
}

static DWORD WINAPI alarm_thread(LPVOID p){
    Sleep((DWORD)(uintptr_t)p * 1000u);
    fprintf(stderr, "\n*** TIME LIMIT REACHED - the guest is still running\n");
    fprintf(stderr, "    import calls: %llu  indirect: %llu  traps: %llu\n",
            (unsigned long long)g_win32_calls, (unsigned long long)g_stat_indirect_ok,
            (unsigned long long)g_stat_unsupported);
    { extern void guest_backtrace(CPU*, int);
      extern CPU *g_all_cpus[]; extern uint32_t g_all_tids[]; extern int g_all_n;
      fprintf(stderr, "    --- call chains of all guest threads (%d) ---\n", g_all_n);
      for (int i = 0; i < g_all_n; i++){
          if (!g_all_cpus[i]) continue;
          fprintf(stderr, "    [thread %d  tid=%u  esp=0x%08x]\n", i, g_all_tids[i], g_all_cpus[i]->esp);
          guest_backtrace(g_all_cpus[i], 26);
      } }
    all_reports();
    _exit(2);
    return 0;
}

static void *guest_main_thread(void *unused){
    (void)unused;
    mem_altstack_install();
    if (setjmp(g_exit_jmp) == 0){
        g_exit_armed = 1;
        guest_lock(&cpu);
        PUSH32(0xDEADC0DEu);
        sub_005a2d1a(&cpu);
        guest_unlock();
        fprintf(stderr, "\n*** guest entry point returned (eip=0x%08x, eax=0x%08x)\n", cpu.eip, cpu.eax);
    } else {
        fprintf(stderr, "\n*** guest exited with code %d\n", g_exit_code);
        guest_unlock();
    }
    lifecycle_guest_exited(g_exit_code);
    return 0;
}

int main(int argc, char **argv){
    const char *exe = argc > 1 ? argv[1] : "PORSCHE.EXE";
    const char *root = argc > 2 ? argv[2] : exe;
    setvbuf(stderr, 0, _IONBF, 0);

    mem_init(); mem_fault_handler_install();
    if (mem_load_pe(exe)) return 1;
    mem_commit(TEB_BASE, 0x20000);
    mem_commit(STACK_LO, STACK_HI - STACK_LO);
    fileio_set_root(root);
    { const char *sd = getenv("NFS5_SAVEDIR"); if (sd && *sd) fileio_set_write_root(sd); }
    dispatch_init(); win32_init();
    if (getenv("BT")) g_bt_on = 1;
    g_report_fn = all_reports;
    if (getenv("TRACE_MAX")) win32_set_trace_max((uint32_t)atoi(getenv("TRACE_MAX")));
    else if (getenv("VERBOSE")) win32_set_trace_max(400u);

    /* TEB (fs:) */
    MR32(TEB_BASE+0x00) = 0xFFFFFFFFu;      /* end of the SEH chain */
    MR32(TEB_BASE+0x04) = STACK_HI;         /* StackBase  */
    MR32(TEB_BASE+0x08) = STACK_LO;         /* StackLimit */
    MR32(TEB_BASE+0x18) = TEB_BASE;         /* Self       */
    MR32(TEB_BASE+0x20) = 100;              /* ProcessId  */
    MR32(TEB_BASE+0x24) = 1;                /* ThreadId   */
    MR32(TEB_BASE+0x2C) = TEB_BASE+0x1000;  /* TLS array  */
    MR32(TEB_BASE+0x30) = PEB_BASE;         /* PEB        */
    mem_commit(PEB_BASE, 0x1000);
    MR32(PEB_BASE+0x08) = 0x00400000u;      /* ImageBaseAddress */
    MR32(PEB_BASE+0xA4) = 5; MR32(PEB_BASE+0xA8) = 1;

    { extern CPU *g_cur_cpu; g_cur_cpu = &cpu; }
    memset(&cpu, 0, sizeof cpu);
    cpu.esp = STACK_HI - 0x1000u;
    cpu.top = 0; cpu.fcw = 0x027f;          /* Windows default: PC=53, RC near, masked */
    cpu.fs_base = TEB_BASE;
    cpu.gs_base = TEB_BASE;

    fprintf(stderr, "\n=== running the entry point: 0x%08x ===\n", PU_ENTRY);
    fprintf(stderr, "    TEB=0x%08x  stack=0x%08x..0x%08x  esp=0x%08x\n\n", TEB_BASE, STACK_LO, STACK_HI, cpu.esp);
    { unsigned a = getenv("ALARM") ? (unsigned)atoi(getenv("ALARM")) : 30u;
      if (a) CloseHandle(CreateThread(0, 0, alarm_thread, (LPVOID)(uintptr_t)a, 0, 0)); }
    {   /* the lifted code carries the guest call chain on the host C stack */
        pthread_attr_t at; pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 256u * 1024u * 1024u);
        pthread_t th;
        if (pthread_create(&th, &at, guest_main_thread, 0) != 0){
            fprintf(stderr, "could not start the guest main thread\n"); return 1; }
        pthread_join(th, 0);
    }
    lifecycle_drain_writes(5.0);
    if (!win32_freeze_guest(2000)) fprintf(stderr, "a guest thread still holds the guest lock\n");
    fprintf(stderr, "\n  hook hits: %llu   /GS failures: %llu\n",
            (unsigned long long)g_hook_hits, (unsigned long long)g_gs_fail);
    all_reports();
    return g_exit_code;
}
