# nfs5-web — agent notes

Browser port of NFS Porsche Unleashed by static recompilation (x86 -> C -> wasm), after nfsu2-web.
Plan and task ids: `docs/PLAN.md`. Architecture: `docs/ARCHITECTURE.md`. Findings: `docs/findings/`.

## Hard rules

- **Never commit anything from the game**: EXE/DLL, game files, lifted C (`gen/`), objects/modules (`out/`),
  converted tracks, screenshots of game assets. Check `git status` before every commit.
- The only supported EXE is English Porsche.exe v3.5 (SHA-256 e4684948...66bb, see `tools/exe.py`).
  Byte patches go in `patches/exe_v35.txt`, never into a copy on disk by hand.
- No screenshots for analysis; text/binary only.
- If stuck, write it down in `docs/findings/` and stop.

## Paths on this machine

- PC mod (feature source, RE notes, wrapper dx11z): `M:\Games\nfspu_nglide\_mod` (its own CLAUDE.md)
- Modded game install: `M:\Games\nfspu_nglide` (launch only via `run_dx11.bat` / race_test)
- Clean English install + official update: `M:\Games\nfs5_clean` (`nfspu_v35.zip`)
- clang: `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin\clang.exe`
- nfsu2-web reference clone: clone it into the scratchpad when needed (`git clone https://github.com/th0mk/nfsu2-web`)

## Commands

```sh
./gen.sh /m/Games/nfs5_clean/nfspu_v35.zip   # patched EXE copy + lift (gen/all)
./build.sh ; TARGET=wasm ./build.sh          # compile checks
```
