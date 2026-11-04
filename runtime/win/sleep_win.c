/* nfs5-web: nanosleep on a high-resolution waitable timer (posix_compat.h). */
#include <windows.h>
#include <time.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#undef nanosleep
int nfs5_nanosleep(const struct timespec *req, struct timespec *rem){
    if (rem){ rem->tv_sec = 0; rem->tv_nsec = 0; }
    if (!req) return -1;
    long long ns = (long long)req->tv_sec * 1000000000LL + req->tv_nsec;
    if (ns <= 0){ SwitchToThread(); return 0; }
    static __thread HANDLE t;
    static __thread int failed;
    if (!t && !failed){
        t = CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!t) failed = 1;
    }
    if (t){
        LARGE_INTEGER due; due.QuadPart = -(ns / 100 > 0 ? ns / 100 : 1);
        if (SetWaitableTimer(t, &due, 0, 0, 0, FALSE)){ WaitForSingleObject(t, INFINITE); return 0; }
    }
    Sleep((DWORD)((ns + 999999) / 1000000));
    return 0;
}
