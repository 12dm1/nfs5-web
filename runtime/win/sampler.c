/* nfs5-web: host-side sampling profiler (HOSTSAMPLE=<file>).
 *
 * A sampler thread suspends every other thread of the process every
 * millisecond, reads its RIP and counts it per thread; every 5 s the
 * histogram (thread id, module, offset; most hit first) is written to <file>.
 * The lifted functions are built with -g1, so
 *   tools/hostsample.sh <file>
 * (llvm-symbolizer) names the guest function (sub_XXXXXXXX) of every hot
 * spot, also inside host code (runtime, renderer; system DLLs: module+off). */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NB 65536
static struct { DWORD64 pc; DWORD tid; unsigned n; } g_h[NB];
static const char *g_path;
static unsigned long long g_total;
static DWORD g_self;
#define MAXT 64
static DWORD g_tid[MAXT]; static HANDLE g_th[MAXT]; static int g_nt;

static void refresh_threads(void){
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (s == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof te;
    DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(s, &te); ok; ok = Thread32Next(s, &te)){
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == g_self) continue;
        int known = 0;
        for (int i = 0; i < g_nt; i++) if (g_tid[i] == te.th32ThreadID) known = 1;
        if (known || g_nt >= MAXT) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (h){ g_tid[g_nt] = te.th32ThreadID; g_th[g_nt++] = h; }
    }
    CloseHandle(s);
}

static void dump(void){
    FILE *f = fopen(g_path, "w");
    if (!f) return;
    static unsigned idx[NB]; unsigned m = 0;
    for (unsigned i = 0; i < NB; i++) if (g_h[i].n) idx[m++] = i;
    unsigned top = m < 600 ? m : 600;
    for (unsigned a = 0; a < top; a++){
        unsigned b = a;
        for (unsigned k = a + 1; k < m; k++) if (g_h[idx[k]].n > g_h[idx[b]].n) b = k;
        unsigned t = idx[a]; idx[a] = idx[b]; idx[b] = t;
    }
    HMODULE exe = GetModuleHandleA(0);
    fprintf(f, "# %llu samples\n", g_total);
    for (unsigned a = 0; a < top; a++){
        DWORD64 pc = g_h[idx[a]].pc & ~(1ull << 63);
        const int via = (int)(g_h[idx[a]].pc >> 63);
        HMODULE mod = 0; char mn[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &mod) && mod)
            GetModuleFileNameA(mod, mn, sizeof mn);
        const char *b = strrchr(mn, 92); b = b ? b + 1 : mn;
        fprintf(f, "%8u t%-6lu %s 0x%llx\n", g_h[idx[a]].n, g_h[idx[a]].tid, mod == exe ? (via ? "exe-waiting-in" : "exe") : b,
                (unsigned long long)(pc - (DWORD64)mod));
    }
    fclose(f);
}

static DWORD WINAPI sampler(LPVOID p){
    (void)p;
    timeBeginPeriod(1);
    DWORD last = GetTickCount(), lastref = 0;
    for (;;){
        Sleep(1);
        if (GetTickCount() - lastref > 500){ lastref = GetTickCount(); refresh_threads(); }
        for (int i = 0; i < g_nt; i++){
            if (SuspendThread(g_th[i]) == (DWORD)-1) continue;
            CONTEXT ctx; ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(g_th[i], &ctx)){
                DWORD64 pc = ctx.Rip;
                /* outside the exe (a wait in ntdll, a pthread call): the first
                 * exe frame up the host stack instead, marked by bit 63 */
                HMODULE exe = GetModuleHandleA(0);
                HMODULE m0 = 0;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &m0);
                if (m0 != exe){
                    CONTEXT u = ctx;
                    for (int d = 0; d < 24 && u.Rip; d++){
                        DWORD64 base = 0;
                        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(u.Rip, &base, NULL);
                        if (!f){ u.Rip = *(DWORD64 *)u.Rsp; u.Rsp += 8; }
                        else { void *hd; DWORD64 ef; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, u.Rip, f, &u, &hd, &ef, NULL); }
                        HMODULE m = 0;
                        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)u.Rip, &m);
                        if (m == exe){ pc = u.Rip | (1ull << 63); break; }
                    }
                }
                unsigned h = (unsigned)(((pc >> 2) ^ g_tid[i]) * 2654435761u) & (NB - 1);
                for (unsigned k = 0; k < NB; k++, h = (h + 1) & (NB - 1)){
                    if (g_h[h].pc == pc && g_h[h].tid == g_tid[i]){ g_h[h].n++; break; }
                    if (!g_h[h].pc){ g_h[h].pc = pc; g_h[h].tid = g_tid[i]; g_h[h].n = 1; break; }
                }
                g_total++;
            }
            ResumeThread(g_th[i]);
        }
        if (GetTickCount() - last > 5000){ last = GetTickCount(); dump(); }
    }
    return 0;
}

static void __attribute__((constructor)) sampler_init(void){
    g_path = getenv("HOSTSAMPLE");
    if (!g_path || !*g_path) return;
    CreateThread(0, 0, sampler, 0, 0, &g_self);
}
