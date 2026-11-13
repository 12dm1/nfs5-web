// nfs5-web, feature port of the PC wrapper's ai_tuning.cpp and the stuck reset of race_trace.cpp: racing AI on the converted
// High Stakes tracks. The [ai] values of the PC mod's dx11z.ini are constants here. runtime/pu_hooks.c calls in from the sites.
// Every number's story: the PC mod's ai_tuning.h and docs/findings (AI1, AI2, AI4, AI5, AI6).
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "hs_walls_math.h"
#include "hs_ai.h"
#include "../pu_cars.h"
#include "hs_walls.h"

extern "C" int fileio_resolve_game(const char *win, char *out, size_t outsz);

namespace {
const float kSpeedLookahead = 1.3f;     // slices are 6.0 m on PU's tracks, 4.6 m on the converted ones
const float kGuardHalfWidth = 1.2f, kGuardMaxAccel = 9.0f;
const float kCapUnit = 1.0f / 3.6f;     // target speed +EB8 per km/h
const float kRefTrigger = 1.12f;
const float kSlipCos2 = 0.95677f;       // cos^2(12 deg)
const int kSlipThrottle = 64;
const float kStuckSeconds = 5.0f, kStuckMetres = 5.0f;

const uint32_t kCarTable = CARS_L_CARS, kSimTick = 0x00606AC4, kTrackData = 0x00628BC0, kReverse = 0x00657430, kDerby = 0x00657400;
const uint32_t kTrackName = 0x00657444, kRaceSetup = CARS_INFO;   // CARS1: both tables moved (runtime/pu_cars.h)

uint8_t *g_m;
bool g_converted;
template <class T> T &at(uint32_t a) { return *(T *)(g_m + a); }
uint32_t guest(const uint8_t *p) { return (uint32_t)(p - g_m); }

FILE *openGame(const char *win, const char *mode) {
    char path[1024];
    return fileio_resolve_game(win, path, sizeof path) ? fopen(path, mode) : nullptr;
}
int sliceCount() {
    const uint32_t td = at<uint32_t>(kTrackData);
    if (!td) return 0;
    const int n = at<int>(td);
    return n > 0 && n < 20000 ? n : 0;
}

// ---- reference speed table (AICarData/default.ais +0x80): no car takes a bend faster than the reference at the same severity
float g_refTable[40];
int g_refState;     // 0 not read, 1 on, -1 none
struct RefCar { uint32_t record; bool clamp; };
RefCar g_refCars[32]; int g_refCarCount;
void refLoad() {
    g_refState = -1;
    FILE *f = openGame("GameData\\Simulation\\AICarData\\default.ais", "rb");
    if (!f) return;
    if (fseek(f, 0x80, SEEK_SET) == 0 && fread(g_refTable, 4, 40, f) == 40) g_refState = 1;
    fclose(f);
}
bool refClampCar(uint32_t record) {
    for (int i = 0; i < g_refCarCount; ++i) if (g_refCars[i].record == record) return g_refCars[i].clamp;
    const float *own = &at<float>(record + 0x80);
    float worst = 0.0f;
    for (int i = 15; i < 40; ++i) if (g_refTable[i] > 0.0f && own[i] / g_refTable[i] > worst) worst = own[i] / g_refTable[i];
    const bool clamp = worst > kRefTrigger;
    if (g_refCarCount < 32) { g_refCars[g_refCarCount].record = record; g_refCars[g_refCarCount++].clamp = clamp; }
    return clamp;
}

// ---- per-car scale of the speed profile: GameData/Track/aiprofile_cars.txt + per-track "# carmul", "# carscale" lines
struct AisFile { char name[32]; uint8_t data[0x130]; float scale; bool listed; };
AisFile *g_ais; int g_aisCount;
float g_carScaleOther = 1.0f;
struct CarScaleSeen { uint32_t record; float scale; };
CarScaleSeen g_carSeen[64]; int g_carSeenCount;
float g_trackCarMul = 1.0f;
char g_trackScaleNames[16][32]; float g_trackScales[16]; int g_trackScaleCount;

void loadCarScales() {
    free(g_ais); g_ais = nullptr; g_aisCount = 0; g_carSeenCount = 0; g_carScaleOther = 1.0f;
    FILE *f = openGame("GameData\\Track\\aiprofile_cars.txt", "r");
    if (!f) return;
    static char names[64][32]; static float scales[64];
    int listed = 0;
    char line[128];
    while (fgets(line, sizeof line, f) && listed < 64) {
        char nm[32]; float k = 0.0f;
        if (line[0] == '#' || sscanf(line, "%31s %f", nm, &k) != 2 || k < 0.0f) continue;
        if (!strcmp(nm, "*")) { g_carScaleOther = k; continue; }
        strcpy(names[listed], nm); scales[listed++] = k;
    }
    fclose(f);
    char dirp[1024];
    if (!fileio_resolve_game("GameData\\Simulation\\AICarData\\default.ais", dirp, sizeof dirp)) return;
    char *slash = strrchr(dirp, '/');
    if (!slash) return;
    *slash = 0;
    DIR *d = opendir(dirp);
    if (!d) { fprintf(stderr, "[ai] cannot list %s\n", dirp); return; }
    g_ais = (AisFile *)malloc(sizeof(AisFile) * 128);
    while (struct dirent *e = readdir(d)) {
        const size_t len = strlen(e->d_name);
        if (g_aisCount >= 128 || len < 5 || strcasecmp(e->d_name + len - 4, ".ais")) continue;
        char file[1400];
        snprintf(file, sizeof file, "%s/%s", dirp, e->d_name);
        FILE *a = fopen(file, "rb");
        if (!a) continue;
        AisFile &x = g_ais[g_aisCount];
        const size_t got = fread(x.data, 1, sizeof x.data, a);
        fclose(a);
        if (got != sizeof x.data) continue;
        snprintf(x.name, sizeof x.name, "%.*s", (int)(len - 4), e->d_name);
        x.scale = g_carScaleOther; x.listed = false;
        for (int i = 0; i < listed; ++i) if (!strcasecmp(names[i], x.name)) { x.scale = scales[i]; x.listed = true; }
        x.scale *= g_trackCarMul;
        for (int i = 0; i < g_trackScaleCount; ++i) if (!strcasecmp(g_trackScaleNames[i], x.name)) x.scale = g_trackScales[i];
        ++g_aisCount;
    }
    closedir(d);
    g_carScaleOther *= g_trackCarMul;
    fprintf(stderr, "[ai] per-car profile scale: %d listed, others x%.2f, %d .ais files; track x%.2f, %d overrides\n", listed, g_carScaleOther, g_aisCount, g_trackCarMul, g_trackScaleCount);
}
float carScaleOf(const uint8_t *car) {
    if (!g_aisCount) return 1.0f;
    const uint32_t rec = *(const uint32_t *)(car + 0x764);
    if (!rec) return g_carScaleOther;
    for (int i = 0; i < g_carSeenCount; ++i) if (g_carSeen[i].record == rec) return g_carSeen[i].scale;
    int hit = -1;
    // By name first: the race setup (0065808C, stride 0x580) keeps the AI spec name at +0x2A8. Quick Race bots drive tuned
    // cars whose AI record matches no file byte for byte.
    const char *aisName = nullptr;
    for (int i = 0; i < CARS_MAX; ++i)
        if (at<uint32_t>(kCarTable + 4u * i) == guest(car)) { aisName = (const char *)(g_m + kRaceSetup + i * 0x580 + 0x2A8); break; }
    if (aisName && aisName[0])
        for (int i = 0; i < g_aisCount; ++i)
            if (!strncasecmp(aisName, g_ais[i].name, 32) && (hit < 0 || (g_ais[i].listed && !g_ais[hit].listed))) hit = i;
    float scale;
    if (hit >= 0) {
        // Parts make the game rebuild the AI record: the speed table against the stock file's says how much faster the car
        // became; the scale follows it up (x1.0 .. x1.2). The game lifts every Quick Race bot's table by 1.035: the base.
        const float *own = &at<float>(rec + 0x80);
        float stock[40];
        memcpy(stock, g_ais[hit].data + 0x80, sizeof stock);
        float sum = 0.0f; int cnt = 0;
        for (int i = 0; i < 40; ++i) if (stock[i] > 1.0f && own[i] > 0.0f && own[i] < 500.0f) { sum += own[i] / stock[i]; ++cnt; }
        const float tuned = cnt ? sum / (float)cnt / 1.035f : 1.0f;
        scale = g_ais[hit].scale * (tuned < 1.0f ? 1.0f : tuned > 1.2f ? 1.2f : tuned);
    } else {
        const uint8_t *r = &at<uint8_t>(rec);
        for (int i = 0; i < g_aisCount; ++i) {
            if (memcmp(r, g_ais[i].data, sizeof g_ais[i].data)) continue;
            if (hit < 0 || (g_ais[i].listed && !g_ais[hit].listed) || (g_ais[i].listed == g_ais[hit].listed && g_ais[i].scale < g_ais[hit].scale)) hit = i;
        }
        for (int i = 0; i < g_aisCount && hit < 0; ++i) if (!memcmp(r + 0x80, g_ais[i].data + 0x80, 160)) hit = i;
        int nearest = -1, best = (int)sizeof g_ais[0].data / 2;
        for (int i = 0; i < g_aisCount && hit < 0; ++i) {
            int diff = 0;
            for (int b = 0; b < (int)sizeof g_ais[i].data; ++b) diff += r[b] != g_ais[i].data[b];
            if (diff < best || (diff == best && nearest >= 0 && g_ais[i].listed && !g_ais[nearest].listed)) { best = diff; nearest = i; }
        }
        if (hit < 0) hit = nearest;
        scale = hit >= 0 ? g_ais[hit].scale : g_carScaleOther;
    }
    if (g_carSeenCount < 64) { g_carSeen[g_carSeenCount].record = rec; g_carSeen[g_carSeenCount++].scale = scale; }
    return scale;
}

// ---- per-track tables: <track>[_r].aiprofile (slice kmh), <track>[_r].aispeed (start end factor; max start end kmh)
float *g_profile, *g_zoneTable, *g_capTable;
int g_zoneCount;
char g_zoneTrack[40];
int g_zoneReverse = -1;

bool loadProfile(const char *track, int rev, int n) {
    free(g_profile); g_profile = nullptr;
    char win[128];
    snprintf(win, sizeof win, "GameData\\Track\\%s%s.aiprofile", track, rev ? "_r" : "");
    FILE *f = openGame(win, "r");
    if (!f) return false;
    float *table = (float *)calloc((size_t)n, sizeof(float));
    char line[128];
    int count = 0;
    g_trackCarMul = 1.0f; g_trackScaleCount = 0;
    while (fgets(line, sizeof line, f)) {
        int s = 0; float k = 0.0f; char nm[32];
        if (sscanf(line, "# carmul %f", &k) == 1 && k > 0.0f) { g_trackCarMul = k; continue; }
        if (g_trackScaleCount < 16 && sscanf(line, "# carscale %31s %f", nm, &k) == 2 && k >= 0.0f) {
            strcpy(g_trackScaleNames[g_trackScaleCount], nm); g_trackScales[g_trackScaleCount++] = k;
            continue;
        }
        if (line[0] == '#' || sscanf(line, "%d %f", &s, &k) != 2 || s < 0 || s >= n || k <= 0.0f) continue;
        table[s] = k * kCapUnit;
        ++count;
    }
    fclose(f);
    if (!count) { free(table); return false; }
    g_profile = table;
    loadCarScales();
    fprintf(stderr, "[ai] speed profile %s: %d of %d slices\n", win, count, n);
    return true;
}
void loadZones(const char *track, int rev) {
    free(g_zoneTable); free(g_capTable);
    g_zoneTable = g_capTable = nullptr; g_zoneCount = 0;
    const int count = sliceCount();
    const int n = count ? count : 4096;
    if (loadProfile(track, rev, n)) g_zoneCount = n;
    char win[128];
    snprintf(win, sizeof win, "GameData\\Track\\%s%s.aispeed", track, rev ? "_r" : "");
    FILE *f = openGame(win, "r");
    if (!f) return;
    float *table = (float *)malloc(sizeof(float) * (size_t)n), *caps = (float *)calloc((size_t)n, sizeof(float));
    for (int i = 0; i < n; ++i) table[i] = 1.0f;
    char line[256];
    int zones = 0, capped = 0;
    while (fgets(line, sizeof line, f)) {
        int a = 0, b = 0; float k = 1.0f;
        if (line[0] == '#') continue;
        if (sscanf(line, "max %d %d %f", &a, &b, &k) == 3) {
            for (int i = a; i <= b; ++i) caps[((i % n) + n) % n] = k > 0.0f ? k * kCapUnit : 0.0f;
            ++capped;
            continue;
        }
        if (sscanf(line, "%d %d %f", &a, &b, &k) != 3) continue;
        for (int i = a; i <= b; ++i) table[((i % n) + n) % n] = k;
        ++zones;
    }
    fclose(f);
    g_zoneTable = table;
    if (capped) g_capTable = caps; else free(caps);
    g_zoneCount = n;
    fprintf(stderr, "[ai] %s: %d zones + %d speed caps over %d slices\n", win, zones, capped, n);
}
void checkZoneTrack() {
    const char *cur = (const char *)(g_m + kTrackName);
    const int rev = at<int>(kReverse) != 0;
    if (!strncmp(cur, g_zoneTrack, 32) && rev == g_zoneReverse) return;
    memcpy(g_zoneTrack, cur, 32);
    g_zoneTrack[32] = 0;
    g_zoneReverse = rev;
    g_refCarCount = 0;
    loadZones(g_zoneTrack, rev);
}
bool profiled(const uint8_t *car) {
    if (!g_profile || !g_converted) return false;
    const uint32_t flags = *(const uint32_t *)(car + 0x52C);    // 1 racer, 10 traffic, 20 cop, 80 smackable
    if (!(flags & 1) || (flags & 0xB0)) return false;
    const int s = *(const int16_t *)(car + 8);
    return s >= 0 && s < g_zoneCount && g_profile[s] > 0.0f && carScaleOf(car) > 0.0f;     // scale 0: planner + .aispeed
}

// ---- target guard: the AI does not know the real walls (islands, pillars, inner walls)
bool pathBlocked(const HsWallSeg *walls, int n, float px, float pz, float tx, float tz, float y, float w) {
    const float dx = tx - px, dz = tz - pz;
    const float len = sqrtf(dx * dx + dz * dz);
    if (len < 1.0f) return false;
    const float nx = -dz / len, nz = dx / len;
    for (int k = -1; k <= 1; ++k) {
        const float ax = px + nx * w * k, az = pz + nz * w * k, bx = tx + nx * w * k, bz = tz + nz * w * k;
        for (int i = 0; i < n; ++i) {
            const HsWallSeg &s = walls[i];
            if (!HsWallYHits(s, y)) continue;
            const float ex = s.bx - s.ax, ez = s.bz - s.az;
            const float den = (bx - ax) * ez - (bz - az) * ex;
            if (fabsf(den) < 1e-6f) continue;
            const float u = ((s.ax - ax) * ez - (s.az - az) * ex) / den;
            const float v = ((s.ax - ax) * (bz - az) - (s.az - az) * (bx - ax)) / den;
            if (u > 1.0f / len && u <= 1.0f && v >= 0.0f && v <= 1.0f) return true;
        }
    }
    return false;
}
float neededAccel(const uint8_t *car, const float *p, float cx, float cz) {
    const float *vel = (const float *)(car + 0x33C), *right = (const float *)(car + 0x364);
    const float v2 = vel[0] * vel[0] + vel[2] * vel[2];
    const float rl = sqrtf(right[0] * right[0] + right[2] * right[2]);
    const float dx = cx - p[0], dz = cz - p[2], d2 = dx * dx + dz * dz;
    if (rl < 0.1f || d2 < 1.0f) return 0.0f;
    const float lat = (dx * right[0] + dz * right[2]) / rl;
    return v2 * 2.0f * fabsf(lat) / d2;
}

// ---- stuck reset (AI5)
struct StuckState {
    uint32_t car, lastTick, anchorTick, lastResetTick;
    int lastSlice, progress, anchorProgress, lastResetSlice, repeats;
    bool moved;
    float anchorPos[3];
};
StuckState g_stuck[CARS_MAX];
} // namespace

extern "C" void hs_ai_init(uint8_t *mem) { g_m = mem; }

extern "C" void hs_ai_set_converted(int converted) {
    if (!g_refState) refLoad();
    checkZoneTrack();
    g_converted = converted != 0;
}
extern "C" float hs_ai_speed_lookahead(void) { return g_converted ? kSpeedLookahead : 1.0f; }
extern "C" float hs_ai_zone(const uint8_t *car) {
    const int s = *(const int16_t *)(car + 8);
    return g_zoneTable && (unsigned)s < (unsigned)g_zoneCount ? g_zoneTable[s] : 1.0f;
}
// after fstp [car+EB8] at 0040BAEA; idx = the severity index of 0040B650
extern "C" void hs_ai_target(uint8_t *car, int idx) {
    float *target = (float *)(car + 0xEB8);
    const bool prof = profiled(car);
    if (prof) *target = g_profile[*(const int16_t *)(car + 8)] * carScaleOf(car);
    if (g_refState == 1 && g_converted && idx >= 0 && idx < 40 && !prof) {
        const uint32_t record = *(const uint32_t *)(car + 0x764);
        if (record && refClampCar(record)) {
            const float own = (&at<float>(record + 0x80))[idx];
            if (own > g_refTable[idx] && own > 0.0f) *target *= g_refTable[idx] / own;
        }
    }
    if (g_converted) {
        const float *v = (const float *)(car + 0x33C), *f = (const float *)(car + 0x37C);
        const float vv = v[0] * v[0] + v[2] * v[2], ff = f[0] * f[0] + f[2] * f[2];
        if (vv >= 69.4f && ff >= 0.01f) {       // from 30 km/h
            const float d = v[0] * f[0] + v[2] * f[2];
            if (!(d > 0.0f && d * d >= kSlipCos2 * vv * ff)) { const float speed = sqrtf(vv); if (*target > speed) *target = speed; }
        }
    }
    if (g_capTable && !prof) {
        const int s = *(const int16_t *)(car + 8);
        if ((unsigned)s < (unsigned)g_zoneCount && g_capTable[s] > 0.0f && g_capTable[s] < *target) *target = g_capTable[s];
    }
}
// at 0040C865, call 00532330 (target - position): t and p are the target and the car's position
extern "C" void hs_ai_guard(const uint8_t *car, float *t, const float *p) {
    if (!g_converted || !car) return;
    static HsWallSeg walls[288];
    int n = hs_walls_near(p[0], p[2], walls, 96);
    n += hs_walls_near((p[0] + t[0]) * 0.5f, (p[2] + t[2]) * 0.5f, walls + n, 96);
    n += hs_walls_near(t[0], t[2], walls + n, 96);
    if (!n) return;
    const float y = p[1], w = kGuardHalfWidth;
    if (!pathBlocked(walls, n, p[0], p[2], t[0], t[2], y, w)) return;
    const float dx = t[0] - p[0], dz = t[2] - p[2];
    const float len = sqrtf(dx * dx + dz * dz);
    if (len < 1.0f) return;
    const float nx = -dz / len, nz = dx / len;
    float bestOff = 0.0f, bestAccel = 1e30f;
    bool found = false;
    for (int step = 1; step <= 28 && !found; ++step) {  // up to 14 m either side
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            const float off = 0.5f * step * sgn, cx = t[0] + nx * off, cz = t[2] + nz * off;
            if (pathBlocked(walls, n, p[0], p[2], cx, cz, y, w)) continue;
            const float acc = neededAccel(car, p, cx, cz);
            if (acc <= kGuardMaxAccel) { bestOff = off; found = true; break; }
            if (acc < bestAccel) { bestAccel = acc; bestOff = off; }
        }
    }
    if (!found && bestAccel > 1e29f) return;
    t[0] += nx * bestOff;
    t[2] += nz * bestOff;
}
// at 0040C8DA: a car sliding more than 12 degrees gets at most this throttle
extern "C" void hs_ai_throttle(uint8_t *car) {
    if (!g_converted) return;
    const float *v = (const float *)(car + 0x33C), *f = (const float *)(car + 0x37C);
    const float vv = v[0] * v[0] + v[2] * v[2], ff = f[0] * f[0] + f[2] * f[2];
    if (vv < 69.4f || ff < 0.01f) return;
    const float d = v[0] * f[0] + v[2] * f[2];
    if (d > 0.0f && d * d >= kSlipCos2 * vv * ff) return;
    if (car[0xD9C] > kSlipThrottle) car[0xD9C] = (uint8_t)kSlipThrottle;
}
// at 0041041E, before the car's tick: 1 = respawn it now (the caller runs 0040F540(car, 1, 0), then hs_ai_stuck_done)
extern "C" int hs_ai_stuck(uint8_t *car) {
    const uint32_t gcar = guest(car);
    int idx = -1;
    for (int i = 0; i < CARS_MAX; ++i) if (at<uint32_t>(kCarTable + 4u * i) == gcar) { idx = i; break; }
    if (idx < 0) return 0;
    StuckState &st = g_stuck[idx];
    const uint32_t flags = *(const uint32_t *)(car + 0x52C);
    if (!(flags & 1) || (flags & 0xB0) || (flags & 4) || *(const int *)(car + 0xDAC) || *(const int *)(car + 0xB50) || at<int>(kDerby)) { st.car = 0; return 0; }
    const uint32_t tick = at<uint32_t>(kSimTick);
    const int slice = *(const int16_t *)(car + 8);
    if (st.car != gcar || tick < st.lastTick || tick > st.lastTick + 64) {     // a new race, a new car object, or a pause
        memset(&st, 0, sizeof st);
        st.lastResetSlice = -1000;
        st.car = gcar; st.lastTick = st.anchorTick = tick; st.lastSlice = slice;
        return 0;
    }
    const int n = sliceCount();
    int d = slice - st.lastSlice;
    if (n) { if (d > n / 2) d -= n; if (d < -n / 2) d += n; }
    const int rev = at<int>(kReverse);
    if (rev) d = -d;
    st.progress += d; st.lastSlice = slice; st.lastTick = tick;
    const float *pos = (const float *)(car + 0x330), *vel = (const float *)(car + 0x33C);
    if (!st.moved) {
        if ((vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]) * 3.6f * 3.6f > 20.0f * 20.0f) {
            st.moved = true; st.anchorTick = tick; st.anchorProgress = st.progress;
            memcpy(st.anchorPos, pos, sizeof st.anchorPos);
        }
        return 0;
    }
    const float ax = pos[0] - st.anchorPos[0], az = pos[2] - st.anchorPos[2];
    if (ax * ax + az * az > kStuckMetres * kStuckMetres) {
        st.anchorTick = tick; st.anchorProgress = st.progress;
        memcpy(st.anchorPos, pos, sizeof st.anchorPos);
        return 0;
    }
    if (tick - st.anchorTick < (uint32_t)(kStuckSeconds * 64.0f)) return 0;
    // Stuck again near the last reset within 20 s: the spot itself traps it, so move it 10, 20, 40, at most 80 slices on.
    int gap = slice - st.lastResetSlice;
    if (gap < 0) gap = -gap;
    if (n && gap > n / 2) gap = n - gap;
    st.repeats = (tick - st.lastResetTick < 20 * 64 && gap < 15) ? st.repeats + 1 : 0;
    const int ahead = st.repeats == 0 ? 0 : (st.repeats >= 4 ? 80 : 10 << (st.repeats - 1));
    int target = slice;
    if (ahead && n) {
        target = slice + (rev ? -ahead : ahead);
        target = ((target % n) + n) % n;
        *(int16_t *)(car + 8) = (int16_t)target;
    }
    static unsigned resets;
    if (++resets <= 50) fprintf(stderr, "[ai] stuck reset: car %d at slice %d -> respawn at %d\n", idx, slice, target);
    *(int *)(car + 0x540) = 0;      // 0040F540 does nothing while it counts
    st.lastResetSlice = slice;
    st.lastResetTick = tick;
    return 1;
}
extern "C" void hs_ai_stuck_done(uint8_t *car) {
    const uint32_t gcar = guest(car);
    for (int i = 0; i < CARS_MAX; ++i) if (at<uint32_t>(kCarTable + 4u * i) == gcar) {
        StuckState &st = g_stuck[i];
        st.lastSlice = *(const int16_t *)(car + 8);
        st.progress = st.anchorProgress;
        st.anchorTick = at<uint32_t>(kSimTick);
        memcpy(st.anchorPos, car + 0x330, sizeof st.anchorPos);
    }
}
