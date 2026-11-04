# Architecture

Modelled on nfsu2-web; the differences for Porsche Unleashed are noted.

## Build time

```
Porsche.exe v3.5 --tools/exe.py (check + patches/)--> out/PORSCHE.EXE
                 --tools/relocs.py + genall.py (Capstone)--> gen/all/shard_*.c, fntab.c, decls.h
runtime/*.c + gen/*.c --clang (wasi-sdk, wasm32-wasip1-threads)--> nfs5.wasm
web/src/*.ts --bundler--> page + workers
```

Every x86 function becomes `void sub_XXXXXXXX(CPU *c)`; registers live in `CPU`, guest memory is wasm linear
memory with base 0 (`g_mem = 0`), so a guest address is the same number as on Windows. Indirect calls go through
a table the size of the code range (`DISPATCH_TEXT_SIZE`, PU: `.text` + `PACODE`); a miss goes to the runtime
(imports, COM objects, THRASH entry points, mod hooks at synthetic addresses).

## Run time

```
 page main thread                    workers (one shared WebAssembly.Memory)
 ───────────────────                 ─────────────────────────────────────────
 WebGPU encoder   <── frame records ── game threads (wasi thread-spawn; CreateThread -> another worker)
 input            ── mailbox ──>
 AudioWorklet     <── PCM ring ──     I/O worker  (/data game folder over HTTP + OPFS cache, /save in OPFS)
                                      net worker  (WebSocket <-> the runtime's socket rings)
```

- One big guest lock: one thread runs guest code at a time; released in Sleep/Wait*/file I/O.
- Subsystems publish their shared blocks to the page (`register(kind, ptr, size)`); `Atomics.wait/notify` doorbells;
  the page's main thread never blocks.
- **Renderer (PU-specific):** PU draws through THRASH (`LoadLibrary("Drivers\\...z")`, 42 entry points), not
  Direct3D. The runtime's THRASH implementation is dx11z's own code: natively it keeps D3D11 (WEB2); in the browser
  its device layer records frames into shared memory and the page encodes them with WebGPU (WEB3), HLSL -> WGSL.
- Sound: DirectSound buffers mixed by an audio-pump thread into a ring an AudioWorklet plays.
- Network: Winsock HLE over rings; the net worker carries them over a WebSocket to the server, which bridges to
  real TCP/UDP (N1 server: lobby and races with PC players).
- Server: password, COOP/COEP headers (SharedArrayBuffer), page + module + game data, WebSocket bridge.
