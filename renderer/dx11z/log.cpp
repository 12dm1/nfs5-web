#include "log.h"

#include <cstdarg>
#include <cstring>
#include <windows.h>
#include <cstdint>
#include <intrin.h>
#include <vector>

// PERF1: time and lines of Log / LogTransition (defined in perf.cpp).
extern uint64_t g_logTicks;
extern uint32_t g_logLines;

// HITCH1: Log used to fflush every line on the calling thread, and a write to
// the log disk now and then blocks for 100-250 ms (HITCH reports: the render
// thread inside WriteFile at ~5 % CPU). Lines now go to a memory buffer; a
// writer thread puts them on disk every 100 ms with WriteFile (no CRT FILE
// lock the game thread could wait on). LogFlush() writes synchronously
// (crash reports, shutdown).
namespace {

struct LogSink {
    HANDLE file = INVALID_HANDLE_VALUE;
    std::vector<char> pending;   // under g_logLock
};
LogSink g_log, g_trans;

// R4: the crash report is written from whichever thread faulted while the
// main thread keeps logging its frame, so a line has to be indivisible.
CRITICAL_SECTION g_logLock;      // the pending buffers
CRITICAL_SECTION g_writeLock;    // the files (writer thread / LogFlush)
bool g_logLockReady = false;
HANDLE g_writer = nullptr;
HANDLE g_wake = nullptr;
volatile LONG g_writerRun = 0;
const size_t kWakeBytes = 256 * 1024;

struct LogLock {
    LogLock() { if (g_logLockReady) EnterCriticalSection(&g_logLock); }
    ~LogLock() { if (g_logLockReady) LeaveCriticalSection(&g_logLock); }
};

HANDLE OpenLogFile(const char* path) {
    // Shared for reading, so the log can be watched while the game runs.
    return CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

void WriteAll(HANDLE f, const std::vector<char>& data) {
    size_t off = 0;
    while (off < data.size()) {
        DWORD wrote = 0;
        if (!WriteFile(f, data.data() + off, (DWORD)(data.size() - off), &wrote, nullptr) || !wrote) break;
        off += wrote;
    }
}

// Moves both pending buffers to disk. 'bounded': give up waiting for the
// write lock after ~200 ms (a writer killed mid-write at process exit never
// releases it) and write anyway.
void Drain(bool bounded) {
    if (!g_logLockReady) return;
    bool locked = true;
    if (bounded) {
        locked = false;
        for (int i = 0; i < 200 && !(locked = TryEnterCriticalSection(&g_writeLock) != 0); ++i) Sleep(1);
    } else {
        EnterCriticalSection(&g_writeLock);
    }
    std::vector<char> logBuf, transBuf;
    {
        LogLock lock;
        logBuf.swap(g_log.pending);
        transBuf.swap(g_trans.pending);
        g_log.pending.reserve(logBuf.capacity());
        g_trans.pending.reserve(transBuf.capacity());
    }
    if (g_log.file != INVALID_HANDLE_VALUE) WriteAll(g_log.file, logBuf);
    if (g_trans.file != INVALID_HANDLE_VALUE) WriteAll(g_trans.file, transBuf);
    if (locked) LeaveCriticalSection(&g_writeLock);
}

DWORD WINAPI WriterProc(void*) {
    while (g_writerRun) {
        WaitForSingleObject(g_wake, 100);
        Drain(false);
    }
    return 0;
}

void Append(std::vector<char>& dst, const char* text, size_t len) {
    dst.insert(dst.end(), text, text + len);
}

// The formatted line in 'stack', or in 'heap' when it is longer.
const char* Format(char* stack, size_t size, std::vector<char>& heap, int& len, const char* fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    len = _vsnprintf_s(stack, size, _TRUNCATE, fmt, copy);
    va_end(copy);
    if (len >= 0) return stack;
    const int need = _vscprintf(fmt, args);
    if (need < 0) { len = 0; return stack; }
    heap.resize((size_t)need + 1);
    len = vsnprintf(heap.data(), heap.size(), fmt, args);
    if (len < 0) len = 0;
    return heap.data();
}

}  // namespace

static LARGE_INTEGER g_qpcFreq = {};
static LARGE_INTEGER g_qpcStart = {};

// <exe dir>\name, or <DX11Z_OUT>\name when the variable is set.
bool WrapperOutPath(char* out, size_t size, const char* name) {
    char dir[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableA("DX11Z_OUT", dir, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        const size_t len = strlen(dir);
        if (len && dir[len - 1] != '\\' && dir[len - 1] != '/') strcat_s(dir, "\\");
    } else {
        if (GetModuleFileNameA(nullptr, dir, MAX_PATH) == 0) return false;
        char* slash = strrchr(dir, '\\');
        if (!slash) return false;
        slash[1] = '\0';
    }
    return sprintf_s(out, size, "%s%s", dir, name) > 0;
}

// <exe dir>\dx11z.ini, or DX11Z_INI when the variable is set.
bool WrapperIniPath(char* out, size_t size) {
    const DWORD n = GetEnvironmentVariableA("DX11Z_INI", out, (DWORD)size);
    if (n > 0 && n < size) return true;
    if (GetModuleFileNameA(nullptr, out, (DWORD)size) == 0) return false;
    char* slash = strrchr(out, '\\');
    if (!slash || (size_t)(slash + 1 - out) + 10 > size) return false;
    memcpy(slash + 1, "dx11z.ini", 10);
    return true;
}

void LogInit() {
    if (!g_logLockReady) {
        InitializeCriticalSection(&g_logLock);
        InitializeCriticalSection(&g_writeLock);
        g_logLockReady = true;
    }
    char path[MAX_PATH] = {};
    char transPath[MAX_PATH] = {};
    WrapperOutPath(path, sizeof path, "dx11z.log");
    WrapperOutPath(transPath, sizeof transPath, "dx11z_transitions.log");
    g_log.file = OpenLogFile(path);
    g_trans.file = OpenLogFile(transPath);
    g_log.pending.reserve(kWakeBytes * 2);
    g_trans.pending.reserve(kWakeBytes);
    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpcStart);
    if (!g_writer && g_log.file != INVALID_HANDLE_VALUE) {
        g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        g_writerRun = 1;
        g_writer = CreateThread(nullptr, 0, &WriterProc, nullptr, 0, nullptr);
    }
}

void LogFlush() { Drain(true); }

void LogClose() {
    // The writer may already be gone (ExitProcess ends the other threads first).
    g_writerRun = 0;
    Drain(true);
    if (g_trans.file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_trans.file);
        g_trans.file = INVALID_HANDLE_VALUE;
    }
    if (g_log.file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log.file);
        g_log.file = INVALID_HANDLE_VALUE;
    }
}

void Log(const char* fmt, ...) {
    if (g_log.file == INVALID_HANDLE_VALUE) return;
    const uint64_t t0 = __rdtsc();
    char stack[1024];
    std::vector<char> heap;
    int len = 0;
    va_list args;
    va_start(args, fmt);
    const char* text = Format(stack, sizeof stack, heap, len, fmt, args);
    va_end(args);
    bool wake = false;
    {
        LogLock lock;
        Append(g_log.pending, text, (size_t)len);
        g_log.pending.push_back('\n');
        wake = g_log.pending.size() >= kWakeBytes;
    }
    if (wake && g_wake) SetEvent(g_wake);
    ++g_logLines;
    g_logTicks += __rdtsc() - t0;
}

void LogTransition(const char* fmt, ...) {
    if (g_trans.file == INVALID_HANDLE_VALUE) return;
    const uint64_t t0 = __rdtsc();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double ms = (double)(now.QuadPart - g_qpcStart.QuadPart) * 1000.0
              / (double)g_qpcFreq.QuadPart;
    char stamp[32];
    const int stampLen = _snprintf_s(stamp, sizeof stamp, _TRUNCATE, "[%10.1f] ", ms);
    char stack[1024];
    std::vector<char> heap;
    int len = 0;
    va_list args;
    va_start(args, fmt);
    const char* text = Format(stack, sizeof stack, heap, len, fmt, args);
    va_end(args);
    {
        LogLock lock;
        Append(g_trans.pending, stamp, (size_t)stampLen);
        Append(g_trans.pending, text, (size_t)len);
        g_trans.pending.push_back('\n');
        if (g_log.file != INVALID_HANDLE_VALUE) {
            g_log.pending.push_back('T');
            Append(g_log.pending, stamp, (size_t)stampLen);
            Append(g_log.pending, text, (size_t)len);
            g_log.pending.push_back('\n');
        }
    }
    g_logLines += 2;
    g_logTicks += __rdtsc() - t0;
}

void LogGameTrackTable(const char* where) {
    char name[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, name, MAX_PATH);
    if (!strstr(name, "PORSCHE.EXE") && !strstr(name, "porsche.exe"))
        return;

    int count = *(int*)0x00691C94;
    void* buf = *(void**)0x00691C90;
    void* ce8 = *(void**)0x00691CE8;
    const char* path = (const char*)0x00691C98;
    const char* saveDir = *(const char**)0x0065B360;
    const char* factoryDir = *(const char**)0x0065B348;
    Log("track table @ %s count=%d buf=%p ce8=%p", where, count, buf, ce8);
    Log("  nfs5 path='%s'", path);
    Log("  factoryDir='%s' saveDir='%s'",
        factoryDir ? factoryDir : "(null)",
        saveDir ? saveDir : "(null)");
}
