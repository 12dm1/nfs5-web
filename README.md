# Need for Speed: Porsche Unleashed in the browser

A fan project that runs the PC game *Need for Speed: Porsche Unleashed* (2000) in the browser, with the
NFS5 Reforged features, tracks converted from *Need for Speed: High Stakes*, and online races. The game's x86 code
is translated to C ahead of time and compiled to WebAssembly; the game's THRASH renderer runs on WebGPU, the sound
on Web Audio. You bring your own copy of the game.

Built after [nfsu2-web](https://github.com/th0mk/nfsu2-web) (MIT), the browser port of NFS Underground 2, whose
lifter this repository started from (`licenses/`).

> Not affiliated with or endorsed by Electronic Arts. This repository contains none of the game's code or data:
> no EXE, no DLL, no game files, no lifted C, no compiled module. All of that is made locally from your copy.

**Play it:** https://nfs5-reforged.duckdns.org/ — in a current browser on a PC, Mac, Android phone, iPhone or iPad.

**Status:** the game runs in the browser on PCs and phones: menus, career, Quick Race with up to 15 opponents,
the High Stakes tracks, the EA add-on cars, gamepad and touch controls, and online races (see [docs/PLAN.md](docs/PLAN.md)).

## What you need

- **The game:** your own English PC copy of *Porsche Unleashed* with the official **v3.5** update. The build takes
  the update's `Porsche.exe` (or the update archive `nfspu_v35.zip` itself): 2,129,965 bytes, SHA-256
  `e4684948ca9bfb552a0fc0eeb7024a13bfe67f2e40ebb6ba121ae6a81dd166bb`. Other versions will not work: every
  address in this project is from that build.
- **To build (Windows):** Python 3 with `pip install -r requirements.txt`, Git Bash, and clang — the one that
  ships with Visual Studio 2026 (component "C++ Clang tools") works for both targets. The browser build will
  also need [wasi-sdk](https://github.com/WebAssembly/wasi-sdk/releases) (Windows release) in `deps/`.

## Build

```sh
pip install -r requirements.txt
./gen.sh "/path/to/nfspu_v35.zip"     # patch a copy of the EXE, translate it to C (into gen/)
./build.sh                            # compile check, native x64   (out/obj)
TARGET=wasm ./build.sh                # compile check, wasm32       (out/wobj)
```

`gen.sh` checks the EXE, applies `patches/exe_v35.txt` to a copy (`out/PORSCHE.EXE`; your file is never
changed) and lifts it: 8,711 functions, 0 missing callees, ~50 MB of C.

## Run it in the browser

Needs [wasi-sdk](https://github.com/WebAssembly/wasi-sdk/releases) in `deps/`, Node.js, a browser with WebGPU (Chrome)
and an installed copy of the game (default `M:/Games/nfs5_clean`, or `--data <folder>`).

```sh
./gen.sh "/path/to/nfspu_v35.zip"     # once
./web/build.sh                        # the module, out/web/nfs5.wasm
run_web.bat --data "C:/Games/Porsche" # builds the page, starts the server, opens http://127.0.0.1:8077/
```

Saves live in the browser's own storage (OPFS) of that address. The server takes `--port` and `--password`.

## How it works

- **`tools/`:** a Python lifter (Capstone) turns every function of `PORSCHE.EXE` into C over a `CPU` struct.
  Guest addresses are wasm linear-memory addresses, so the game's globals sit where they sit on Windows.
- **`patches/`:** byte patches applied to the EXE before lifting (ours; byte values only).
- **`runtime/`**: Windows, DirectSound, DirectInput 5 and the THRASH renderer reimplemented, the
  browser parts in `runtime/web/`.
- **`web/`**: the page (game threads as workers on one shared memory, WebGPU, AudioWorklet) and the
  server (password-protected; serves the page, the module and your game folder; WebSocket bridge for online play).

More: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Credits

The static recompilation method and the lifter come from Talha Müderrisoğlu's
[NFSU2Recomp](https://github.com/TalhaMudo/NFSU2Recomp) and th0mk's [nfsu2-web](https://github.com/th0mk/nfsu2-web),
which follow [N64: Recompiled](https://github.com/N64Recomp/N64Recomp) and
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp). Built with the help of Claude Code.

## License

MIT, see [LICENSE](LICENSE). The game itself is Electronic Arts' and is not part of this repository.
