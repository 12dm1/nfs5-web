# WEB0: can PU run in a browser the nfsu2-web way?

Spike of 2026-11-04. Reference: https://github.com/th0mk/nfsu2-web (MIT, fork of
NFSU2Recomp by Talha Müderrisoğlu; upstream revision in `_mod/web/upstream/REVISION`).
Its method: static recompilation. A Python/Capstone lifter turns every function of the
EXE into C over a `CPU` struct, clang compiles it to wasm32, and a runtime reimplements
Win32, Direct3D 9 (on WebGPU), DirectSound (AudioWorklet) and DirectInput. Threads are
workers on one SharedArrayBuffer. The user supplies the game files.

**Verdict: feasible.** The lifter works on PORSCHE.EXE almost unchanged, and the output
compiles for x64 and for wasm32 with the clang that ships with VS 18. The runtime and
the port of our own mod are the real work.

## 1. Lift (done in this spike)

Workspace `_mod/web/` (tools only; `gen/`, `out/` are derived from the game and are
not committed, same rule as upstream).

| Step | Result |
|---|---|
| `tools/relocs.py` | PORSCHE.EXE keeps a normal `.reloc` directory: 56 746 HIGHLOW fixups (NFSU2 had to dig its out of a zeroed directory) |
| `JTSWITCH=1 tools/genall.py PORSCHE.EXE 64` | 17 s: **8 711 functions**, 106 jump tables became C switches, **0 missing callees, 0 truncated branches**, 50 MB of C |
| traps (unsupported instructions) | 85, see below |
| native compile (clang 22, x64, -O2) | all 65 files, no errors, 14 s |
| wasm32 compile (`-pthread -matomics -msimd128 -mbulk-memory -mnontrapping-fptoint -mtail-call -O3`) | all 65 files, no errors, 21 MB of objects (header shims in `runtime/wasm_inc/`; the real build takes wasi-sdk's libc) |

Changes to the upstream lifter (`_mod/web/tools/`):
- `lift.py`: PACODE (second code section, 0x5B0000..0x5B1AF9, right after `.text`) lifted
  as one range with `.text`; `lock inc/dec/add/...` lifted as the plain read-modify-write
  (CRT locale guard in `_stricmp` 0x5AE3C0).
- `split_reloc.py`: NFSU2 split-screen operand relocation removed (plain constants).
- `retpast.txt`: emptied (its SPEED2 addresses mean nothing in PU).
- `runtime/cpu.h`: `DISPATCH_TEXT_SIZE` = 0x1B0AF9 (PU code range).

Traps: almost all of them are "functions" seeded by a reloc inside `.text` that point at
data (switch tables and pointer tables the lifter decodes as code: `das`, `aaa`, `iretd`,
`int 0x51`, ...). Nothing calls them. The real ones are all cold CRT paths:
- `fnsave/frstor/fnstenv/fldenv` (0x5A3AB2, 0x5A6293): x87 exception/environment helpers.
- The CRT x87 transcendental dispatch table at .data 0x5E5920 (16 entries 0x5A59CA..0x5A5DF4,
  decoded mid-instruction as `int 6`). Must be checked before the first race (sin/cos through
  the CRT, not inline `fsin`).

## 2. Imports vs the nfsu2-web runtime

| DLL | covered by upstream runtime | missing |
|---|---|---|
| KERNEL32 | 106/132 | comm/modem port API, file mappings, semaphores (mostly stubs) |
| USER32 | 32/48 | hooks (SetWindowsHookEx), GetKeyState, ScreenToClient/ClientToScreen, ... |
| WSOCK32 | 27/27 | - |
| WINMM | 6/8 | joyGetPosEx, joyGetDevCapsA |
| DSOUND | 1/2 | DirectSoundEnumerateA (#6); the DirectSound object model needs checking against PU's v1-5 use |
| DINPUT | 0/1 | DirectInputCreateA (PU = DirectInput 5, upstream = 8) |
| DDRAW | 0/2 | DirectDrawEnumerateA, DirectDrawCreate (PU uses them only around the THRASH driver) |
| VERSION, SHELL32, GDI32, ADVAPI32, mrbupd.dll | mostly 0 | trivial stubs |

Rendering is NOT upstream's D3D9-on-WebGPU. PU draws through THRASH (`LoadLibrary`
Drivers\dx11z + 42 entry points). In the browser that driver is our own dx11z, compiled
to wasm directly (it is our C++), with its D3D11 calls moved to WebGPU and HLSL to WGSL.
D3D11 and WebGPU have a similar shape, which makes this simpler than upstream's D3D9 layer.

## 3. Our mod in a recompiled game (patch census)

`tools/patch_census.py` runs one fe=0 race (race_test launch), snapshots `.text`,
PACODE and `.rdata` from memory 3 s after the start and after 8 s of racing, then diffs
them against the EXE. Result (`out/patch_census.json`, race snapshot; 3 s after the start
only 9 patches existed):

| kind | count | in a recompiled build |
|---|---|---|
| call-site redirects (rel32 of an existing `call` -> wrapper) | 187 | patch the EXE before lifting to a synthetic address; `dispatch_indirect` maps it to a host C function |
| detours (`jmp/call` rel32 written at a function entry or mid-function) | 52 | upstream `OVERRIDE` (sub_X -> runtime C, original stays as sub_X_orig) / `HOOK_SITE` |
| operand pointers into wrapper data (tables and globals moved) | ~150 (88 aligned + the unaligned dwords in "bytes") | lift-time symbolic operand (what upstream `split_reloc.py` already does) |
| `.rdata` pointers (vtables) | 17 | host thunk at a synthetic address |
| plain byte edits (immediates, jcc flips) | ~40 | patch the EXE before lifting |

That is about 450 sites on the race path alone. Frontend-only patches (FE1, NP1, menus,
gamepad cursor) are not in this count yet; a census in the menus is still to do.

What makes the port mechanical:
- In wasm32 guest address == linear-memory address (`g_mem` = 0), so every
  `*(int*)0x00691D90` in the wrapper keeps working unchanged.
- Hook functions need an adapter: arguments from `c->esp` (plus `ecx` for thiscall),
  return value in `eax`. It can be generated per signature with a C++ template.
- Calls from the wrapper into game code (~400 address-to-function-pointer constants)
  become `guest_call(addr, args...)` through the same template.

What needs care:
- 20 `__asm` blocks in 7 files: rewrite by hand.
- Patches that the wrapper toggles at run time (by INI / game state) are baked in at lift
  time. They need either a lift-time switch or a runtime flag check in the hook.
- Wrapper threads and timing (PERF work, precise_timer) do not carry over; browser timing
  and workers replace them.

## 4. Plan if we go on

1. **WEB1 native boot**: build the lifted C natively on Windows x64 against the upstream
   runtime parts that are not Mac-specific (win32*, heap, fileio, dispatch, mem, com,
   winsock) plus a stub THRASH driver. Goal: the frontend runs its game logic (log only,
   no picture). Upstream's Mac runtime is POSIX; it needs a Windows or Linux/WSL host layer.
2. **WEB2 picture**: dx11z with a WebGPU backend (or first: dx11z native over the lifted
   game, which proves the THRASH interface without a browser).
3. **WEB3 browser**: wasi-sdk build, AudioWorklet DirectSound, DirectInput 5 on
   keyboard/Gamepad API, files over HTTP / File System Access, COOP/COEP server (the N1
   Node server can serve it; netplay over WebSocket to the same server).
4. **WEB4 mod port**: hook manifest generated from the census + frontend census, adapters,
   the `__asm` rewrites, per-feature checks.

Unknowns that decide the schedule: the THRASH/DirectDraw interplay at startup, the
DirectSound streaming model, and how many wrapper features rely on toggling patches.
