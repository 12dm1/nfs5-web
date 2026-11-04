# renderer/

`dx11z/` is a copy of the D3D11 core of our own dx11z THRASH driver (the NFS5 mod project's
`_mod/wrapper/src`, commit 4ddf325): our code, no game code, released here under this repository's MIT
licence. Changes against the wrapper: `dx11_draw.cpp` reads the clip-code global through `R_GuestPtrAt`
(guest memory instead of the 32-bit process), `feature_stubs.cpp` stands in for the feature modules not
ported yet.

`thrash_dx11.cpp` connects it to the runtime's THRASH HLE (`runtime/thrash_hle.c`), owns the window and
feeds its input to the guest. See `docs/tasks/WEB2-native-picture.md`.
