# WEB1: native boot (Windows x64, no picture)

Goal: the lifted PORSCHE.EXE runs as a Windows x64 program on the runtime and its frontend logic reaches the
main menu (log only; THRASH is a stub). Status: in progress (see "Progress" at the end).

## Decisions

- **Toolchain: llvm-mingw** (`deps/llvm-mingw-20261020-ucrt-x86_64`, clang 23, UCRT, winpthreads; downloaded with the user's permission). It covers pthread/unistd/dirent/clock_gettime itself, so the POSIX shim is small; dx11z will be built from source with the same toolchain. (Superseded first choice: Visual Studio 18 clang, MSVC target (`x86_64-pc-windows-msvc`).)
- **POSIX subset as a shim** (`runtime/win/posix_*.h/.c`) over Win32, so the nfsu2-web runtime files stay close
  to upstream and diffs stay readable. Measured use in the core (win32, win32_ext, fileio, mem, heap, dispatch,
  module, genexec, lifecycle, debug, watchdog, winsock): ~70 functions.
  - pthread: mutex (incl. recursive, trylock), cond (+timedwait, broadcast), create/detach/exit, once, attr
    stacksize, setname -> SRWLOCK/CRITICAL_SECTION, CONDITION_VARIABLE, `_beginthreadex`, SetThreadDescription.
  - time: clock_gettime(MONOTONIC/REALTIME), nanosleep, usleep, sched_yield -> QPC, waitable timer, SwitchToThread.
  - files: open/close/read/write/pread/lseek/fstat/stat/mkdir/rmdir/unlink/rename/access/ftruncate/fsync, dirent
    -> UCRT `_open` family + FindFirstFile.
  - memory: mmap/mprotect of the 4 GB guest reservation -> VirtualAlloc(MEM_RESERVE) / VirtualProtect.
  - signals: sigaction(SIGSEGV) + sigaltstack for the guest-fault report -> vectored exception handler.
  - strings: strcasecmp/strncasecmp -> _stricmp/_strnicmp.
  - Mac-only bits to replace: mach_absolute_time, os_unfair_lock, CommonCrypto SHA-256 (upstream web/sha256.c),
    kqueue/ifaddrs in winsock.c (network is WEB2n; WEB1 builds the Winsock HLE without a host backend first).
- Guest memory: `g_mem` = base of a 4 GB reservation (as on the Mac); pages committed on demand.

## PU-specific changes to the copied runtime

| File | Change |
|---|---|
| `cpu.h` | `DISPATCH_TEXT_SIZE` = 0x1B0AF9 (done) |
| `heap.c` | `TEXT_LO/HI` = 0x401000..0x5B1AF9, the CRT range of PU (to find), image end 0x6E3000 instead of 0x932000 |
| `module.c` | drop the server.dll path (SPEED2 0x5FA300/0x89CF48); keep the generic second-module loader for later (dplayerx.dll?) |
| `debug.c` | drop the SPEED2 debug sites (0x65AE25, 0x66887E, __security_check_cookie at 0x76115E) |
| `dispatch.c` | drop the EFXSTAT watch (0x650000 range) |
| `win32.c` | import table of PU (gen: `tools/argcount.py` -> `gen/all/impargs.c`); missing imports from WEB0: comm/modem stubs, file mappings, SetWindowsHookEx, joyGetPosEx, VERSION.dll, ShellExecuteA, RegFlushKey, GetStockObject, mrbupd.dll stubs |
| new `thrash_stub.c` | LoadLibraryA("Drivers\\...z") + GetProcAddress of the 42 THRASH entry points -> host functions that log and succeed |
| new | DirectDrawEnumerateA / DirectDrawCreate (what PU does with them around THRASH, to find), DirectInputCreateA (DI5) minimal, DirectSoundCreate + DirectSoundEnumerateA (#6) minimal (silence) |
| new `main_win.c` | from upstream `tests/boot.c` / `runtime/web/main_web.c`: memory, PE image, TEB/PEB, file roots (/data = clean v3.5 install, /save), dispatch, win32, guest entry on its own thread |

## Steps

1. posix shim + compile the copied core with VS clang (MSVC target); fix until it links with the lifted shards.
2. PU import table + missing-import stubs; THRASH/DDraw/DInput/DSound stubs.
3. Run: CRT startup -> WinMain; log every import call (upstream VERBOSE / trace) and every trap.
4. Iterate to the frontend: file loads from the data folder, the THRASH init sequence, the first frontend screen
   (the log shows the screen/layout loads).

## Done when

The log shows the frontend's main menu being built (layout/screen files loaded, THRASH frame calls in a loop)
with no trap and no unknown import on the way.

## Progress

2026-11-04:
- `runtime/win/posix_compat.h` (force-included) + Windows branches in mem.c (VirtualAlloc 4 GB, VEH fault
  report), heap.c (SRWLOCK, PU ranges), dispatch.c (rdtsc on QPC), win32_ext.c (waitable-timer sleep, disk space,
  tm_gmtoff), fileio.c, module.c: the 12 core files compile.
- `tools/argcount.py`: real stdcall counts for all 39 PU imports nfsu2-web did not know (mrbupd.dll = cdecl);
  `gen/all/impargs.c` has 238 entries, none guessed.
- debug.c: NFSU2 mod hook chain and SPEED2 debug sites replaced by a weak `pu_hook()`; dispatch.c: SPEED2 EFXSTAT
  range removed; `runtime/pu_stubs.c`: weak stand-ins (COM bridge, profile hooks, audio clock, autotest).
- `runtime/win/main_win.c` (entry 0x5A2D1A, TEB/PEB, guest stack 0x7F000000..0x7FF00000) + `link_native.sh`.
- Link: 46 undefined symbols left, by group:
  - generic upstream files still to copy: `res.c` (PE resources), `modheap.c` (module heaps), `winsock.c`
    (Winsock HLE; WEB2n), `call_guest`/`com_scratch` (in upstream com.c);
  - **PU COM layer to write** (`runtime/pu_com.c`): com_init, com_forget_cpu, com_mem_stats, com_scratch,
    call_guest, and the DirectDraw / DirectSound / DirectInput 5 objects behind DirectDrawCreate,
    DirectSoundCreate, DirectInputCreateA; upstream com.c is the model for the vtable bridges (0xE0000000+);
  - window/graphics host (`metal_*`: frames, game size, cursor, mem stats, quit) -> headless stubs for WEB1,
    dx11z for WEB2;
  - NFSU2-only (stub): language_registry, overrides_report, hfr_*, roam_game_parked, discord_*, audio_set_muted,
    kbd_pump, d3dx_hle_report, autotest_frame/note_unimpl, g_srv_* (server.dll module tables: stub modinfo).
