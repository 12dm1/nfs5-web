/* The race's reflection cube (PC wrapper scene_probe.cpp RunCubemap, R1). After the rear mirror the scene is drawn once more from the main
 * camera's spot into one face of a cube, with the game's own view, traversal and flush; the player's car is left out. One face per frame
 * the PC draws all six every second frame, which a browser's frame budget does not hold. ?env=CUBE=0 turns it off; CUBE_EVERY=<n>
 * draws a face every n-th frame (web/src/quality.ts, phones). */
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

int r_cube_faces_per_frame(void);
static void cube_face(CPU *c);
/* at 00468AA6, after the mirror pass: edi = the race renderer; r_web.c picks how many faces (1..6) by the frame rate */
void pu_cube_capture(CPU *c){
    const int n = r_cube_faces_per_frame();
    for (int i = 0; i < n; i++) cube_face(c);
}
static void cube_face(CPU *c){
    static int on = -1, face, every = 1, skip;
    static uint32_t buf;
    if (on < 0){
        const char *e = getenv("CUBE"); on = !(e && *e == '0');
        /* CUBE_EVERY=<n> (the page's light profile on phones): one face every n-th frame; the whole scene is drawn once more for a face */
        const char *n = getenv("CUBE_EVERY"); every = n && *n ? atoi(n) : 1;
        if (every < 1 || every > 30) every = 1;
    }
    if (!on) return;
    if (++skip < every) return;
    skip = 0;
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

/* ---- the menu's showroom cube (PC scene_probe.cpp FE1) ----
 * The menu frame 004D2F30 begins its 3D view at 004D2FEA, the screen draws its scene objects through 004D15E0 (type [obj+14]: 2 = a location
 * model of FEData/Models, 0 = the car) and ends the view at 004D308C. After that one face a frame is drawn from the car's spot with only that
 * frame's location models, so every room reflects itself; a screen without a location has no cube. */
void r_cube_reset(void);
#define FE_ROOMS 8
static uint32_t g_fe_view, g_fe_rooms[FE_ROOMS], g_fe_car;
static int g_fe_nrooms;
void sub_004d15e0(CPU *restrict c){
    const uint32_t obj = c->ecx;
    if (obj && !g_cam_capture){
        if (MR32(obj + 0x14) == 2){
            int known = 0;
            for (int i = 0; i < g_fe_nrooms; i++) known |= g_fe_rooms[i] == obj;
            if (!known && g_fe_nrooms < FE_ROOMS) g_fe_rooms[g_fe_nrooms++] = obj;
        } else g_fe_car = obj;
    }
    sub_004d15e0_orig(c);
}
void pu_fe_begin_view(CPU *c){ g_fe_view = MR32(c->esp); }
void pu_fe_capture(CPU *c){
    static int on = -1, face;
    static uint32_t buf, last_room;
    uint32_t rooms[FE_ROOMS];
    const int nrooms = g_fe_nrooms;
    memcpy(rooms, g_fe_rooms, sizeof rooms);
    const uint32_t car = g_fe_car, main_view = g_fe_view;
    g_fe_nrooms = 0; g_fe_car = 0; g_fe_view = 0;
    if (on < 0){ const char *e = getenv("CUBE"); on = !(e && *e == '0'); }
    if (!on) return;
    const uint32_t room0 = nrooms > 0 ? rooms[0] : 0;
    if (room0 != last_room){ last_room = room0; face = 0; r_cube_reset(); }
    const uint32_t engine = MR32(ENGINE);
    if (!engine || !main_view || !room0) return;
    const uint32_t owner = MR32(engine + 0x70), backend = owner ? MR32(owner + 4) : 0;
    const uint32_t pose = MR32((car ? car : room0) + 8);
    if (!backend || !pose) return;
    if (!buf) buf = gheap_alloc(1, 64, 1);
    /* the car's spot: every object of the location is posed at +400; +334 adds the object's own height, a car's one when no car is on screen */
    float p[3], y334;
    memcpy(p, g_mem + pose + 0x400, 12); memcpy(&y334, g_mem + pose + 0x334, 4);
    p[1] += (car ? y334 - p[1] : 0.58f) + 0.55f;

    const uint32_t r_eax = c->eax, r_ecx = c->ecx, r_edx = c->edx, r_ebx = c->ebx, r_esi = c->esi, r_edi = c->edi, r_ebp = c->ebp;
    const uint32_t keep_z = MR32(backend + 0x04), keep_zw = MR32(backend + 0x0C), keep_cull = MR32(backend + 0x10);
    g_cam_capture = 1;
    const uint32_t color = MR32(main_view + 0x28);
    const uint32_t ctor[8] = { 0, 0, 0, 0x3F800000u, 0x3F800000u, color, MR32(main_view + 0x18), MR32(main_view + 0x24) };
    const uint32_t view = pu_guest_call(c, 0x004672F0u, engine, ctor, 8);
    if (!view){ g_cam_capture = 0; return; }
    memcpy(g_mem + view + 0x118, g_mem + main_view + 0x118, 5);
    MR32(view + 0x120) = MR32(main_view + 0x120); MR32(view + 0x124) = MR32(main_view + 0x124); MR32(view + 0x128) = MR32(main_view + 0x128);
    const float half = 45.0f;
    memcpy(g_mem + view + 0x130, &half, 4);
    memcpy(g_mem + buf, p, 12);
    memcpy(g_mem + buf + 12, k_rot[face], 36);
    const uint32_t a_pos = buf, a_rot = buf + 12;
    pu_guest_call(c, 0x00466280u, view + 0xB8, &a_rot, 1);
    pu_guest_call(c, 0x00466220u, view + 0xB8, &a_pos, 1);
    const uint32_t aux = MR32(engine + 0x64);
    MR32(engine + 0x64) = 1;

    r_capture_target(face);
    r_capture_phase(1, 1);      /* the room's lamp panels light the paint; no sun sprite: the alpha stays 0 */
    pu_guest_call(c, 0x00467230u, engine, &view, 1);
    /* the menu's own state for its 3D view (004D2FEF..004D3025) */
    uint32_t a = 2; pu_guest_call(c, 0x00537890u, backend, &a, 1);
    a = 1; pu_guest_call(c, 0x00537870u, backend, &a, 1);
    a = 3; pu_guest_call(c, 0x00537830u, backend, &a, 1);
    for (int i = 0; i < nrooms; i++){
        const uint32_t r3d = MR32(rooms[i] + 0x10), pass = 0;
        if (r3d) pu_guest_call(c, 0x004430A0u, r3d, &pass, 1);
    }
    pu_guest_call(c, 0x00467290u, engine, NULL, 0);
    r_capture_phase(0, 0);
    pu_guest_call(c, 0x004672A0u, engine, NULL, 0);
    r_capture_target(-1);
    r_capture_mips(face);

    MR32(engine + 0x64) = aux;
    pu_guest_call(c, 0x00466E90u, view, NULL, 0);
    pu_guest_call(c, 0x0059F050u, 0, &view, 1);
    pu_guest_call(c, 0x00537890u, backend, &keep_cull, 1);
    pu_guest_call(c, 0x00537870u, backend, &keep_zw, 1);
    pu_guest_call(c, 0x00537830u, backend, &keep_z, 1);
    g_cam_capture = 0;
    c->eax = r_eax; c->ecx = r_ecx; c->edx = r_edx; c->ebx = r_ebx; c->esi = r_esi; c->edi = r_edi; c->ebp = r_ebp;
    face = (face + 1) % 6;
}

/* ---- the sun and its lens flare (PC G7v5, G8) ----
 * 00460290 draws the sun and, while [sun+144] is 0, reads the 4x4 pixels under it once as the flare's reference (1 = taken,
 * 2 = the read failed: no flares for the race); 00460460 re-reads them every 200 ms and draws flare.dat by how many still match.
 * The cube captures draw the sky too, so the first read happened in a face: a wrong reference, the flares never showed. In a capture
 * the sun is drawn with the reference step skipped (144 = 3 for the call) and the flare check is off (140 = 0 for the call). */
static int sun_on_screen(uint32_t sun){
    float x, y; memcpy(&x, g_mem + sun + 0x12C, 4); memcpy(&y, g_mem + sun + 0x130, 4);
    const float w = (float)(int32_t)MR32(0x00619784u), h = (float)(int32_t)MR32(0x00619788u);
    return x >= 3.0f && y >= 3.0f && x <= w - 3.0f && y <= h - 3.0f;
}
void sub_00460290(CPU *restrict c){
    const uint32_t sun = c->ecx;
    /* also not off the screen: the first race frame has the sun at (0,-1), half the reference outside */
    if (sun && MR32(sun + 0x144) == 0 && (g_cam_capture || !sun_on_screen(sun))){
        MR32(sun + 0x144) = 3;
        sub_00460290_orig(c);
        MR32(sun + 0x144) = 0;
        return;
    }
    sub_00460290_orig(c);
}
void sub_00460460(CPU *restrict c){
    const uint32_t sun = c->ecx;
    if (g_cam_capture && sun){
        const uint8_t on = g_mem[sun + 0x140];
        g_mem[sun + 0x140] = 0;
        sub_00460460_orig(c);
        g_mem[sun + 0x140] = on;
        return;
    }
    sub_00460460_orig(c);
}
/* 00451710 sizes the sun and flare sprites as half width = W/640 s, half height = H/480 s: oval on a wide screen. Its width load
 * (fild [00619784] at 0045181A) reads 00460458 instead (patches/exe_v35.txt), which holds H * 4/3 for the call. */
void sub_00451710(CPU *restrict c){
    MR32(0x00460458u) = MR32(0x00619788u) * 4u / 3u;
    sub_00451710_orig(c);
}
#else
#include "cpu.h"
#include "decls.h"
void sub_004d15e0(CPU *restrict c){ sub_004d15e0_orig(c); }
void sub_00460290(CPU *restrict c){ sub_00460290_orig(c); }
void sub_00460460(CPU *restrict c){ sub_00460460_orig(c); }
void sub_00451710(CPU *restrict c){ MR32(0x00460458u) = MR32(0x00619784u); sub_00451710_orig(c); }
#endif
