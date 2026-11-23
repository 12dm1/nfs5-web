/* nfs5-web ON1: PvE bots of a network race (port of the PC wrapper's net_sync_probe.cpp "PvE bots" and race_modes.cpp
 * "keep the car the server picked for a slot").  Overrides of lifted functions: gen.sh OVERRIDE=...
 *
 * A bot is a member of the game that no client speaks for: the server (web/online/netgather.mjs) invents it.  The game's
 * creator (slot 0) runs it with its own AI and, after each of its own car-state packets (BE, 0x28 bytes, 0055D4D0), sends
 * one more for every car nobody has fed for 1.5 s.  Every client applies them through the full car array
 * (patches/exe_v35.txt 0048F665).
 * The server announces a bot's car with records 0x226 (car id) and 0x225 (colour); the network race build (004B4E32)
 * replays the frontend's own setup over carInfo and loses them, so they are kept per slot and written back after it. */
#include "cpu.h"
#include "pu_cars.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
uint32_t gheap_alloc(int heap, uint32_t n, int zero);
void sub_0055d4d0_orig(CPU *);
void sub_004a0df0_orig(CPU *);
void sub_004b4b80_orig(CPU *);
void sub_0048d6a0_orig(CPU *);

#define COMMS     0x00628C70u
#define CARS      CARS_L_CARS      /* CARS1: moved (pu_cars.h) */
#define MYCAR     0x0065741Cu
#define SIMTICK   0x00606AC4u
#define CARINFO   CARS_INFO        /* CARS1: moved */
#define CI_STRIDE 0x580u
#define BUILD_CAR_STATE 0x004A0110u
#define SEND      0x0055D4D0u
#define FIND_CAR  0x004F4360u
#define CATALOGUE 0x006919E8u
#define FED_GRACE 96u                     /* 1.5 s of sim ticks: after that a silent car is a bot */

static uint32_t g_fed[16], g_last_tick, g_packets, g_pkt;
static int g_in_send;

static void note_fed(uint32_t car){
    for (uint32_t i = 0; i < CARS_MAX; i++) if (MR32(CARS + 4 * i) == car){ g_fed[i] = MR32(SIMTICK); return; }
}

/* 0055D4D0(link, buf, len): send on a link, unreliable, cdecl */
void sub_0055d4d0(CPU *restrict c){
    const uint32_t link = MR32(c->esp + 4), buf = MR32(c->esp + 8), len = MR32(c->esp + 12);
    sub_0055d4d0_orig(c);
    if (g_in_send || len != 0x28 || !buf || MR8(buf) != 0xBE) return;
    const uint32_t comms = MR32(COMMS);
    if (!comms || MR32(comms + 0xDC) != 0) return;               /* only the game's creator owns the bots */
    const uint32_t eax = c->eax, now = MR32(SIMTICK), mine = MR32(MYCAR);
    if (now < g_last_tick || !g_last_tick) for (int i = 0; i < 16; i++) g_fed[i] = now;   /* a new race */
    g_last_tick = now;
    if (now < FED_GRACE) return;
    if (!g_pkt) g_pkt = gheap_alloc(1, 0x28, 1);
    const uint32_t head = MR32(comms + 0x260);                   /* the member list, node+8 = player */
    if (!head) return;
    g_in_send = 1;
    int n = 0;
    for (uint32_t node = MR32(head); node && node != head && n < 16; node = MR32(node), n++){
        const uint32_t pl = MR32(node + 8);
        if (!pl) continue;
        const int slot = (int)MR32(pl + 0x40), idx = (int)MR32(pl + 0x4C);
        if (idx < 0 || idx > 8 || (uint32_t)idx == mine || slot < 0 || slot > 7) continue;
        if (now - g_fed[idx] < FED_GRACE) continue;              /* somebody speaks for it */
        const uint32_t car = MR32(CARS + 4u * (uint32_t)idx);
        if (!car) continue;
        memset(g_mem + g_pkt, 0, 0x28);
        MR8(g_pkt) = 0xBE; MR8(g_pkt + 1) = (uint8_t)slot;
        { const uint32_t a[2] = { g_pkt + 4, car }; call_guest(c, BUILD_CAR_STATE, 2, a); }
        { const uint32_t a[3] = { link, g_pkt, 0x28 }; call_guest(c, SEND, 3, a); }
        if ((g_packets++ & 1023) == 0) fprintf(stderr, "[bots] %u car state packets sent (slot %d)\n", g_packets, slot);
    }
    g_in_send = 0;
    c->eax = eax;
}

/* 004A0DF0(car, packet): apply a received car state, cdecl.  A packet for a car this client has not built yet would
 * read car+3C */
void sub_004a0df0(CPU *restrict c){
    const uint32_t car = MR32(c->esp + 4);
    if (!car){ POP32(c->eip); return; }
    note_fed(car);
    sub_004a0df0_orig(c);
}

/* ---- the server's car per slot ---- */
static int g_slot_car[8], g_slot_colour[8];
static char g_slot_model[8][32], g_slot_sound[8][16];

static void resolve_slot(CPU *c, int slot){
    if (!MR32(CATALOGUE)) return;                                /* the catalogue exists only in the frontend */
    const uint32_t id = (uint32_t)g_slot_car[slot];
    const uint32_t car = call_guest(c, FIND_CAR, 1, &id);
    if (!car || MR16(car) != (uint16_t)id) return;
    snprintf(g_slot_sound[slot], sizeof g_slot_sound[slot], "%s", (const char *)(g_mem + car + 0x179));
    snprintf(g_slot_model[slot], sizeof g_slot_model[slot], "%s", (const char *)(g_mem + car + 0x7F));
}

/* 004B4B80(record): apply a received setup record [id][slot][value], cdecl */
void sub_004b4b80(CPU *restrict c){
    const uint32_t rec = MR32(c->esp + 4);
    sub_004b4b80_orig(c);
    const uint32_t comms = MR32(COMMS);
    /* the race build sends every car again with its dense race index in the slot field: only lobby records are per slot */
    if (!rec || (comms && !MR8(comms + 0xBC))) return;
    const uint32_t id = MR32(rec), slot = MR32(rec + 4), v = MR32(rec + 8);
    if (slot >= 8) return;
    const uint32_t eax = c->eax;
    if (id == 0x225) g_slot_colour[slot] = (int)v;
    if (id == 0x226 && !(g_slot_car[slot] == (int)v && g_slot_model[slot][0])){
        g_slot_car[slot] = (int)v; g_slot_model[slot][0] = 0; g_slot_sound[slot][0] = 0;
        resolve_slot(c, (int)slot);
    }
    c->eax = eax;
}

static int dense_of_slot(int slot){
    const uint32_t comms = MR32(COMMS), head = comms ? MR32(comms + 0x260) : 0;
    int k = 0;
    for (uint32_t p = head ? MR32(head) : 0; p && p != head && k < 16; p = MR32(p), k++){
        const uint32_t pl = MR32(p + 8);
        if (pl && (int)MR32(pl + 0x40) == slot) return (int)MR32(pl + 0x4C);
    }
    return -1;
}

/* 0048D6A0: called by the network race build at 004B5051, after the frontend's setup replay */
void sub_0048d6a0(CPU *restrict c){
    const uint32_t ret = MR32(c->esp);
    sub_0048d6a0_orig(c);
    if (ret != 0x004B5056u) return;
    for (int i = 0; i < 8; i++){
        if (!g_slot_car[i] || !g_slot_model[i][0]) continue;
        const int d = dense_of_slot(i);
        if (d < 0 || d > 7) continue;
        const uint32_t ci = CARINFO + (uint32_t)d * CI_STRIDE;
        char *model = (char *)(g_mem + ci + 0x228), *sound = (char *)(g_mem + ci + 0x2E8);
        if ((int)MR32(ci + 0x574) == g_slot_car[i] && !strcasecmp(model, g_slot_model[i]) &&
            (!g_slot_sound[i][0] || !strcasecmp(sound, g_slot_sound[i]))) continue;   /* a player's own car */
        MR32(ci + 0x574) = (uint32_t)g_slot_car[i];
        snprintf(model, 32, "%s", g_slot_model[i]);
        if (g_slot_sound[i][0]) snprintf(sound, 16, "%s", g_slot_sound[i]);
        if (g_slot_colour[i]){
            MR32(ci + 0x10) = (uint32_t)g_slot_colour[i];
            for (int k = 0; k < 3; k++) MR32(ci + 0x218 + 4u * (uint32_t)k) = (uint32_t)g_slot_colour[i];
        }
        fprintf(stderr, "[bots] slot %d (car index %d) -> car %d (%s) colour %08X\n", i, d, g_slot_car[i], model, (unsigned)g_slot_colour[i]);
    }
}
