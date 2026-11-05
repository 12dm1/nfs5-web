// nfs5-web, feature port of the PC wrapper's hs_corridor.cpp: the walls of the converted High Stakes tracks (.hsw) against the
// car's box. No guest types here: runtime/pu_ext.c passes host pointers and the game's own wall response as callbacks.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hs_walls_math.h"
#include "hs_walls.h"

namespace {
const float kCell = 8.0f;
const float kStepDt = 1.0f / 32.0f;     // 0049506E: predicted move per step
const int kMaxNear = 256;               // poles and trees add edges, a dense corner can hold well over 96
struct Cell { uint64_t key; int seg; };
HsWallSeg *g_segs; int g_nsegs;
Cell *g_cells; int g_ncells;
bool g_oneSided;
// The game's collision steps run under the guest lock, one at a time.
HsWallSeg g_local[kMaxNear];

uint64_t cellKey(int x, int z) { return (uint64_t)(uint32_t)x << 32 | (uint32_t)z; }
int cellCmp(const void *a, const void *b) {
    const Cell *p = (const Cell *)a, *q = (const Cell *)b;
    if (p->key != q->key) return p->key < q->key ? -1 : 1;
    return p->seg - q->seg;
}
int cellsOf(const HsWallSeg &s, Cell *out) {
    const int x0 = (int)floorf(fminf(s.ax, s.bx) / kCell), x1 = (int)floorf(fmaxf(s.ax, s.bx) / kCell);
    const int z0 = (int)floorf(fminf(s.az, s.bz) / kCell), z1 = (int)floorf(fmaxf(s.az, s.bz) / kCell);
    const int n = (x1 - x0 + 1) * (z1 - z0 + 1);
    if (out) for (int x = x0; x <= x1; ++x) for (int z = z0; z <= z1; ++z) (out++)->key = cellKey(x, z);
    return n;
}
void buildIndex() {
    long total = 0;
    for (int i = 0; i < g_nsegs; ++i) total += cellsOf(g_segs[i], nullptr);
    g_cells = (Cell *)malloc(sizeof(Cell) * (size_t)(total ? total : 1));
    int k = 0;
    for (int i = 0; i < g_nsegs; ++i) {
        const int n = cellsOf(g_segs[i], g_cells + k);
        for (int j = 0; j < n; ++j) g_cells[k + j].seg = i;
        k += n;
    }
    g_ncells = k;
    qsort(g_cells, (size_t)k, sizeof(Cell), cellCmp);
}
int gather(float x, float z, HsWallSeg *out, int cap) {
    const int cx = (int)floorf(x / kCell), cz = (int)floorf(z / kCell);
    int n = 0;
    for (int dx = -1; dx <= 1; ++dx) for (int dz = -1; dz <= 1; ++dz) {
        const uint64_t key = cellKey(cx + dx, cz + dz);
        int lo = 0, hi = g_ncells;
        while (lo < hi) { const int m = (lo + hi) / 2; if (g_cells[m].key < key) lo = m + 1; else hi = m; }
        for (; lo < g_ncells && g_cells[lo].key == key; ++lo) {
            const HsWallSeg &s = g_segs[g_cells[lo].seg];
            bool dup = false;
            for (int k = 0; k < n && !dup; ++k) dup = out[k].ax == s.ax && out[k].az == s.az && out[k].bx == s.bx && out[k].bz == s.bz;
            if (dup) continue;
            out[n++] = s;
            if (n >= cap) return n;
        }
    }
    return n;
}
// The wrapper's ReplayWallHit: the contact goes through the game's own response (bounce, spin, sound, sparks, damage),
// at its time of impact. Returns true if the car was moved.
bool replayHit(float *pos, float *vel, const HsCarBox &box, const HsWallSeg *segs, int n, const HsWallGame *g) {
    HsWallContact c = HsWallNearest(pos, segs, n, box, 8.0f);
    if (!c.valid || c.sep >= 0) c = HsWallSweep(pos, vel, segs, n, box, kStepDt);
    if (!c.valid) return false;
    const float vn = vel[0] * c.nx + vel[2] * c.nz;
    if (vn >= 0) return false;
    const float gap = c.sep;
    if (gap > 0) { pos[0] -= c.nx * gap; pos[2] -= c.nz * gap; }
    const float normal[3] = {c.nx, 0.0f, c.nz};
    float point[3];
    HsBoxSupportPoint(box, pos, c.nx, c.nz, point);
    point[1] = pos[1];
    const float depth = c.sep < 0 ? -c.sep : 0.0f;
    const float push[3] = {c.nx * depth, 0.0f, c.nz * depth};
    g->hit(g->ctx, normal, point, push);
    return gap > 0;
}
} // namespace

extern "C" void hs_walls_clear(void) {
    free(g_segs); free(g_cells);
    g_segs = nullptr; g_cells = nullptr; g_nsegs = g_ncells = 0; g_oneSided = false;
}

extern "C" int hs_walls_load(const char *path) {
    hs_walls_clear();
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char magic[4];
    uint32_t count = 0;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "HSW1", 4) != 0 || fread(&count, 4, 1, f) != 1 || count == 0 || count > 20000) { fclose(f); return -2; }
    g_segs = (HsWallSeg *)malloc(sizeof(HsWallSeg) * count);
    if (fread(g_segs, sizeof(HsWallSeg), count, f) != count) { fclose(f); hs_walls_clear(); return -3; }
    // HZ7 tail: 'HSX1', flags (1 = one-sided), count, then object walls (poles, trees, rocks) in the same layout
    uint32_t tail[2] = {0, 0};
    g_nsegs = (int)count;
    if (fread(magic, 1, 4, f) == 4 && memcmp(magic, "HSX1", 4) == 0 && fread(tail, 4, 2, f) == 2 && tail[1] <= 40000) {
        g_oneSided = (tail[0] & 1) != 0;
        g_segs = (HsWallSeg *)realloc(g_segs, sizeof(HsWallSeg) * (count + tail[1]));
        g_nsegs = (int)(count + fread(g_segs + count, sizeof(HsWallSeg), tail[1], f));
    }
    fclose(f);
    buildIndex();
    return g_nsegs;
}

extern "C" void hs_walls_step(uint8_t *car, int before, const HsWallGame *g) {
    if (!g_nsegs) return;
    if (!car[0x7F] || car[0x7D] || !car[0x7C]) return;
    float *pos = (float *)(car + 0x330), *vel = (float *)(car + 0x33C);
    HsCarBox box;
    memcpy(box.axis, car + 0x364, sizeof box.axis);
    memcpy(box.half, car + 0x3A8, sizeof box.half);
    int n = gather(pos[0], pos[2], g_local, kMaxNear);
    if (g_oneSided) {
        int kept = 0;
        for (int i = 0; i < n; ++i) if (HsWallFacing(g_local[i], pos)) g_local[kept++] = g_local[i];
        n = kept;
    }
    const bool advanced = before && replayHit(pos, vel, box, g_local, n, g);
    const auto r = HsWallConstrain(pos, vel, g_local, n, box, before ? kStepDt : 0.0f);
    if (!r.valid) { if (advanced) g->rebuild(g->ctx); return; }
    if (r.shift != 0 || advanced) g->rebuild(g->ctx);    // the car's transformed body points
}

extern "C" int hs_walls_near(float x, float z, void *out, int cap) {
    return g_nsegs ? gather(x, z, (HsWallSeg *)out, cap) : 0;
}
