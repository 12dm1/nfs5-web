/* The race's reflection cube (PC wrapper scene_probe.cpp RunCubemap, R1). After the rear mirror the scene is drawn once more from the main
 * camera's spot into one face of a cube, with the game's own view, traversal and flush; the player's car is left out. One face per frame:
 * the PC draws all six every second frame, which a browser's frame budget does not hold. ?env=CUBE=0 turns it off. */
#ifdef __wasm__
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "decls.h"

uint32_t pu_guest_call(CPU *c, uint32_t fn, uint32_t ecx, const uint32_t *args, int n);
uint32_t gheap_alloc(int kind, uint32_t size, int zero);
void r_capture_target(int face);
void r_capture_mips(int face);
void r_capture_phase(int clear_alpha, int opaque_gain);
extern int g_cam_capture;       /* pu_ext.c: 90 degrees, square, while a face is drawn */

#define ENGINE      0x00628130u
#define TRAVERSAL   0x00628BACu
#define CAMERA_FLAGS 0x00627F43u
#define CULL_FLAG   0x0065742Cu

static const float k_rot[6][9] = {
    { 0, 0,-1,   0, 1, 0,   1, 0, 0},   /* +X; rows: right, up, forward, as a Direct3D cube */
    { 0, 0, 1,   0, 1, 0,  -1, 0, 0},   /* -X */
    { 1, 0, 0,   0, 0,-1,   0, 1, 0},   /* +Y */
    { 1, 0, 0,   0, 0, 1,   0,-1, 0},   /* -Y */
    { 1, 0, 0,   0, 1, 0,   0, 0, 1},   /* +Z */
    {-1, 0, 0,   0, 1, 0,   0, 0,-1},   /* -Z */
};

/* at 00468AA6, after the mirror pass: edi = the race renderer */
void pu_cube_capture(CPU *c){
    static int on = -1, face;
    static uint32_t buf;
    if (on < 0){ const char *e = getenv("CUBE"); on = !(e && *e == '0'); }
    if (!on) return;
    const uint32_t race = c->edi, engine = MR32(ENGINE);
    const uint32_t main_view = race ? MR32(race + 4) : 0, traversal = MR32(TRAVERSAL);
    if (!engine || !main_view || !traversal) return;
    const uint32_t owner = MR32(engine + 0x70), backend = owner ? MR32(owner + 4) : 0;
    if (!backend) return;
    if (!buf) buf = gheap_alloc(1, 64, 1);

    const uint32_t r_eax = c->eax, r_ecx = c->ecx, r_edx = c->edx, r_ebx = c->ebx, r_esi = c->esi, r_edi = c->edi, r_ebp = c->ebp;
    const uint32_t keep_z = MR32(backend + 0x04), keep_zw = MR32(backend + 0x0C), keep_cull = MR32(backend + 0x10);
    g_cam_capture = 1;
    const uint32_t color = MR32(main_view + 0x28);
    const uint32_t ctor[8] = { 0, 0, 0, 0x3F800000u, 0x3F800000u, color, MR32(main_view + 0x18), MR32(main_view + 0x24) };
    const uint32_t view = pu_guest_call(c, 0x004672F0u, engine, ctor, 8);
    if (!view){ g_cam_capture = 0; return; }
    memcpy(g_mem + view + 0x118, g_mem + main_view + 0x118, 5);
    /* the second sky branch of 0045D470 is on for pass type 2 only */
    MR32(view + 0x12C) = 2;
    MR32(view + 0x120) = MR32(main_view + 0x120); MR32(view + 0x124) = MR32(main_view + 0x124); MR32(view + 0x128) = MR32(main_view + 0x128);
    const float half = 45.0f;
    memcpy(g_mem + view + 0x130, &half, 4);
    memcpy(g_mem + buf, g_mem + main_view + 0xB8 + 0x54, 12);
    memcpy(g_mem + buf + 12, k_rot[face], 36);
    const uint32_t a_pos = buf, a_rot = buf + 12;
    pu_guest_call(c, 0x00466280u, view + 0xB8, &a_rot, 1);
    pu_guest_call(c, 0x00466220u, view + 0xB8, &a_pos, 1);
    const uint8_t flags = g_mem[CAMERA_FLAGS];
    const uint32_t aux = MR32(engine + 0x64);
    g_mem[CAMERA_FLAGS] = flags | 0x10;     /* no player's car */
    MR32(engine + 0x64) = 1;                /* no main-only layers */

    r_capture_target(face);
    r_capture_phase(1, 0);
    uint32_t a = MR32(CULL_FLAG) ? 1u : 2u;
    pu_guest_call(c, 0x00537890u, backend, &a, 1);
    pu_guest_call(c, 0x00467230u, engine, &view, 1);
    pu_guest_call(c, 0x0045D7D0u, 0, NULL, 0);      /* sky */
    r_capture_phase(1, 1);
    pu_guest_call(c, 0x0047FB40u, traversal, NULL, 0);
    pu_guest_call(c, 0x00443610u, 0, NULL, 0);      /* cars */
    a = 1; pu_guest_call(c, 0x00537870u, backend, &a, 1);
    a = 3; pu_guest_call(c, 0x00537830u, backend, &a, 1);
    pu_guest_call(c, 0x00467290u, engine, NULL, 0); /* flush */
    r_capture_phase(0, 0);
    pu_guest_call(c, 0x0045D7F0u, 0, NULL, 0);      /* the sky's background */
    pu_guest_call(c, 0x004672A0u, engine, NULL, 0);
    pu_guest_call(c, 0x0044FEA0u, 0, NULL, 0);      /* the traversal's marks, for the main pass */
    r_capture_target(-1);
    r_capture_mips(face);

    MR32(engine + 0x64) = aux;
    g_mem[CAMERA_FLAGS] = flags;
    pu_guest_call(c, 0x00466E90u, view, NULL, 0);
    pu_guest_call(c, 0x0059F050u, 0, &view, 1);
    pu_guest_call(c, 0x00537890u, backend, &keep_cull, 1);
    pu_guest_call(c, 0x00537870u, backend, &keep_zw, 1);
    pu_guest_call(c, 0x00537830u, backend, &keep_z, 1);
    g_cam_capture = 0;
    c->eax = r_eax; c->ecx = r_ecx; c->edx = r_edx; c->ebx = r_ebx; c->esi = r_esi; c->edi = r_edi; c->ebp = r_ebp;
    face = (face + 1) % 6;
}
#endif
