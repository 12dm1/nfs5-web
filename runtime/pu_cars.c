/* CARS1: more cars per race (runtime/pu_cars.h, docs/findings/CARS1-car-limit.md).
 *
 *   CARLOG=<s>   every <s> simulation seconds: each car's index, kind flags, slice, speed and race place (stderr)
 *   CARCHK=1     with a lift made with CARCHK=1: indexed reads and writes of game tables whose offset is past the
 *                table's extent in the census (tools/car_census.py), and any access to the old place of a moved
 *                table; the report (max offset per instruction) goes to $CARCHK_OUT, default carchk.txt
 */
#include "cpu.h"
#include "pu_cars.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SIMTICK 0x00606AC4u
void pu_cars_chk_dump(void);

void pu_cars_monitor(void){
    static int every = -1; static uint32_t next;
    if (every < 0){ const char *e = getenv("CARLOG"); every = e && *e ? atoi(e) * 64 : 0; next = 1; }
    if (!every) return;
    const uint32_t tick = MR32(SIMTICK);
    if (!tick || tick < next) return;
    next = tick + (uint32_t)every;
    const int n = (int)MR32(CARS_N_CARS);
    char line[2048]; int o = 0;
    o += snprintf(line + o, sizeof line - o, "[cars] t=%.0f n=%d racers=%d numcars=%d |", tick / 64.0, n,
                  (int)MR32(CARS_N_RACERS), (int)MR32(CARS_NUMCARS));
    for (int i = 0; i < n && i < CARS_MAX && o < (int)sizeof line - 80; i++){
        const uint32_t car = MR32(CARS_L_CARS + 4u * (uint32_t)i);
        if (!car){ o += snprintf(line + o, sizeof line - o, " %d:-", i); continue; }
        const float *v = (const float *)(g_mem + car + 0x33C), *p = (const float *)(g_mem + car + 0x330);
        const double kmh = sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]) * 3.6;
        o += snprintf(line + o, sizeof line - o, " %d:%x/%d/%.0f/p%d", i, MR32(car + 0x52C) & 0xFFFu,
                      (int)(int16_t)MR16(car + 8u), kmh, (int)MR32(car + 0xB48u));
        (void)p;
    }
    fprintf(stderr, "%s\n", line);
    pu_cars_chk_dump();
}

/* ---- CARCHK: bounds census at run time ---- */
#define CHK_SLOTS 65536u
static struct { uint32_t ip, base, gap, maxoff, n; } g_chk[CHK_SLOTS];
static uint32_t g_chk_used;
static const struct { uint32_t lo, hi; } g_old[] = {
#define CARS_OLD(n, o, ol, a, al) { o, o + ol },
    CARS_MOVES(CARS_OLD)
#undef CARS_OLD
};
static void chk_record(uint32_t ip, uint32_t base, uint32_t gap, uint32_t off){
    uint32_t h = (ip * 2654435761u) & (CHK_SLOTS - 1);
    for (uint32_t k = 0; k < CHK_SLOTS; k++, h = (h + 1) & (CHK_SLOTS - 1)){
        if (g_chk[h].ip == ip){ g_chk[h].n++; if (off > g_chk[h].maxoff) g_chk[h].maxoff = off; return; }
        if (!g_chk[h].ip){ g_chk[h].ip = ip; g_chk[h].base = base; g_chk[h].gap = gap; g_chk[h].maxoff = off; g_chk[h].n = 1; g_chk_used++; return; }
    }
}
static uint64_t g_chk_calls;
uint32_t chk_ea(uint32_t ea, uint32_t base, uint32_t gap, uint32_t ip){
    g_chk_calls++;
    const uint32_t off = ea - base;
    if (off >= gap) chk_record(ip, base, gap, off);
    for (unsigned i = 0; i < sizeof g_old / sizeof g_old[0]; i++)
        if (ea >= g_old[i].lo && ea < g_old[i].hi) chk_record(ip | 0x80000000u, g_old[i].lo, 0, ea - g_old[i].lo);
    return ea;
}
void pu_cars_chk_dump(void){
    if (getenv("CARCHKDBG")) fprintf(stderr, "[carchk] calls %llu used %u\n", (unsigned long long)g_chk_calls, g_chk_used);
    if (!g_chk_calls) return;               /* not a CARCHK lift */
    const char *p = getenv("CARCHK_OUT"); if (!p || !*p) p = "carchk.txt";
    FILE *f = fopen(p, "w"); if (!f){ fprintf(stderr, "[carchk] cannot write %s\n", p); return; }
    fprintf(f, "# %llu checks, %u instructions out of extent\n", (unsigned long long)g_chk_calls, g_chk_used);
    for (uint32_t h = 0; h < CHK_SLOTS; h++) if (g_chk[h].ip)
        fprintf(f, "%s %08x base %08x gap %x maxoff %x n %u\n", g_chk[h].ip & 0x80000000u ? "OLD" : "IDX",
                g_chk[h].ip & 0x7FFFFFFFu, g_chk[h].base, g_chk[h].gap, g_chk[h].maxoff, g_chk[h].n);
    fclose(f);
}

/* ---- the race setup block: reset and replay copies (pu_hooks.c) ---- */
_Static_assert(CARS_INFO_OLD + CARS_INFO_OLDN * CARS_INFO_STRIDE == CARS_SETUP + CARS_SETUP_LEN, "carInfo ends the setup block");
_Static_assert(CARS_INFO_X + (CARS_MAX - CARS_INFO_OLDN) * CARS_INFO_STRIDE <= CARS_REGION_END, "shadow fits");
/* CARS2: cars and smackables (signs, cones: NUMSMACKABLES 65807C) share the engine's object list of 128 (descriptions
 * 628FC0[128], pointers 5E8C24[128]; 004A1280 reads (id & 0x7F) - numcars).  The converter keeps at most 119 on a track
 * (128 - 9 cars); with 16 racers Empire City and Snowy Ridge overran it and the race crashed as it started.  Hook
 * 00414E1C, right after 004A17F0 counted them and before 004110A0 builds the race's objects: with more than 9 cars
 * the last ones are not created (the count of 9 or fewer cars keeps the stock behaviour). */
void pu_cars_smack_cap(void){
    const int32_t cars = (int32_t)MR32(CARS_NUMCARS), smack = (int32_t)MR32(CARS_NUMSMACK);
    const int32_t cap = (int32_t)CARS_OBJECTS - (cars > 9 ? cars : 9);
    if (smack <= cap) return;
    MR32(CARS_NUMSMACK) = (uint32_t)(cap > 0 ? cap : 0);
    fprintf(stderr, "[cars] roadside objects %d -> %d: %d cars share the object list of %u\n", smack, cap, cars, CARS_OBJECTS);
}

void pu_cars_setup_hook(uint32_t ip){
    const uint32_t lo = CARS_INFO_OLDN * CARS_INFO_STRIDE, all = CARS_MAX * CARS_INFO_STRIDE;
    uint8_t *info = g_mem + CARS_INFO, *rep = g_mem + CARS_REPLAY + (CARS_INFO_OLD - CARS_SETUP), *x = g_mem + CARS_INFO_X;
    switch (ip){
    case 0x4B4DBBu: case 0x4B6AC3u:                 /* after memset(6573E8, 0, 0x3E24) */
        memset(info, 0, all); break;
    case 0x413E8Bu: case 0x414002u: case 0x414699u: /* after setup -> replay buffer */
        memcpy(rep, info, lo); memcpy(x, info + lo, all - lo); break;
    case 0x413DD1u:                                 /* after replay buffer -> setup */
        memcpy(info, rep, lo); memcpy(info + lo, x, all - lo); break;
    }
}
