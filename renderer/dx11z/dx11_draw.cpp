#include "tex_anim.h"
#include "perf.h"
#include "dx11_draw.h"
#include "dx11_device.h"
#include "dx11_state.h"
#include "car_reflection.h"
#include "scene_probe.h"
#include "materials.h"
#include "car_mirror.h"
#include "motion_blur.h"
#include "thrash_api.h"
#include "log.h"
#include "dx11_texture.h"
#include "car_shading.h"
#include "car_paint.h"
#include <math.h>
const uint8_t* R_GuestPtrAt(uint32_t guestAddr);   // nfs5-web (thrash_dx11.cpp)

static bool g_carNormals = false;        // CAR1
static bool g_lastTriMeshCar = false;    // CAR1
static bool g_paintPageDraw = false;     // CP1: the draw being shaded is on a car paint page

void Draw_SetCarNormals(bool on) { g_carNormals = on; }
bool Draw_LastTriMeshCarMaterial() { return g_lastTriMeshCar; }

// GPU vertex layout — reordered from THRASH_Vertex {x,y,z,rhw,color,specular,u,v}
// to group position floats together for the input layout (POSITION=float4, TEXCOORD=float2,
// COLOR0/1=R8G8B8A8_UNORM). The .bgra swizzle in the vertex shader corrects the
// D3DCOLOR BGRA→RGBA channel mismatch.
struct DrawVertex {
    float x, y, z, w;
    float u, v;
    uint32_t color;
    uint32_t specular;
    // G10: UV rectangle of the triangle (minU, minV, maxU, maxV) for magnified
    // 2D art outside the race; all zero = no clamp. Set per triangle in
    // FlushVerts, so multi-quad UI meshes get one rect per sub-image.
    float rect[4];
};

static ID3D11Buffer* g_dynVB = nullptr;
static const int MAX_VERTS = 65536;
// L2: the dynamic vertex buffer is a ring. Every draw used to Map it with
// WRITE_DISCARD, which renames the whole 3 MB buffer per draw; at 1000-2000
// draws a frame the driver spent ~25-35 us per draw on that alone (dx11z.log
// frame times scaled with the draw count). NO_OVERWRITE appends behind the
// cursor and DISCARD happens only when the ring wraps.
static int g_vbCursor = MAX_VERTS;   // forces a DISCARD on the first draw
// Scratch for the batched entry points (they used to new[]/delete[] per draw).
static DrawVertex* g_scratch = nullptr;
static DrawVertex* g_mirrorScratch = nullptr;   // MAT14: mirrored car draws

static DrawVertex ConvertVertex(const THRASH_Vertex* src) {
    DrawVertex dv;
    dv.rect[0] = dv.rect[1] = dv.rect[2] = dv.rect[3] = 0.0f;
    dv.x = src->x;
    dv.y = src->y;
    dv.z = src->z;
    dv.w = src->rhw;
    dv.u = src->u;
    dv.v = src->v;
    dv.color = src->color;
    dv.specular = src->specular;
    return dv;
}

// MAT11: per-draw smooth vertex normals for the material shader. The game
// hands over screen-space vertices without normals, and PSMaterial rebuilds a
// flat normal per triangle from ddx/ddy, which shows every polygon of the road
// in the specular and the reflection. Here the vertices of one draw are
// welded by their exact (x, y, rhw) bits, the camera-space position is rebuilt
// with the same maths as VSMaterial/PSMaterial, the face normals (oriented
// toward the camera) are area-weighted into each welded vertex, and the result
// travels in rect.xyz with rect.w = 1 (TEXCOORD1 is free for material draws in
// the race). A vertex without a usable normal keeps rect.w = 0 and the shader
// falls back to the flat normal.
static const int SN_HASH = 1 << 17;
static uint32_t g_snHashGen[SN_HASH];
static int      g_snHashVal[SN_HASH];
static uint32_t g_snGen = 0;
static uint32_t g_snKey[MAX_VERTS][3];
static float    g_snPos[MAX_VERTS][3];
static float    g_snAcc[MAX_VERTS][3];
static int      g_snId[MAX_VERTS];
static bool     g_materialDraw = false;

// MAT11b: stitching across draw calls. A road segment is one draw, so a
// vertex on the seam between two segments only sees the faces of its own draw
// and the two sides disagree by the old facet angle, which cuts a sharp line
// through the reflection. The track is static and a vertex's world normal does
// not depend on the camera, so every draw also adds its per-vertex sums into a
// map keyed by the quantised world position (2 cm), and a vertex takes the sum
// of the PREVIOUS frame when the map has it: that sum already holds both
// segments. One frame of latency on static geometry is invisible.
static const int SW_CAP = 1 << 16;
struct SnWorldEntry { int32_t k[3]; float n[3]; };
static SnWorldEntry g_swMap[2][SW_CAP];
static uint32_t     g_swGen[2][SW_CAP];
static uint32_t     g_swStamp[2] = {1, 1};
static int          g_swCur = 0;
static uint32_t     g_swFrame = 0xFFFFFFFFu;
static uint32_t     g_swHits = 0, g_swLookups = 0;
static float        g_snOut[MAX_VERTS][3];
static float        g_snWorld[MAX_VERTS][3];   // MAT14c: world position per welded vertex

static SnWorldEntry* SwFind(int m, const int32_t k[3], bool create) {
    uint32_t h = (uint32_t)k[0] * 0x9E3779B1u;
    h ^= (uint32_t)k[1] * 0x85EBCA6Bu + (h << 6) + (h >> 2);
    h ^= (uint32_t)k[2] * 0xC2B2AE35u;
    h ^= h >> 15;
    uint32_t slot = h & (SW_CAP - 1);
    for (int probe = 0; probe < 32; probe++) {
        if (g_swGen[m][slot] != g_swStamp[m]) {
            if (!create) return nullptr;
            g_swGen[m][slot] = g_swStamp[m];
            SnWorldEntry& e = g_swMap[m][slot];
            e.k[0] = k[0]; e.k[1] = k[1]; e.k[2] = k[2];
            e.n[0] = e.n[1] = e.n[2] = 0.0f;
            return &e;
        }
        SnWorldEntry& e = g_swMap[m][slot];
        if (e.k[0] == k[0] && e.k[1] == k[1] && e.k[2] == k[2]) return &e;
        slot = (slot + 1) & (SW_CAP - 1);
    }
    return nullptr;
}

static inline int32_t SwQuant(float v) {
    const float q = v * 50.0f;   // 2 cm cells
    return (int32_t)(q >= 0.0f ? q + 0.5f : q - 0.5f);
}

static void SmoothNormals(DrawVertex* verts, int count) {
    if (count < 3 || !Materials_SmoothNormalsEnabled()) return;
    float proj[4];
    if (!Materials_ViewProj(proj)) return;
    const float sw = (float)g_dx.logicalW, sh = (float)g_dx.logicalH;
    if (sw <= 0.0f || sh <= 0.0f || proj[0] == 0.0f || proj[1] == 0.0f) return;
    if (count > MAX_VERTS) count = MAX_VERTS;

    if (++g_snGen == 0) { memset(g_snHashGen, 0, sizeof(g_snHashGen)); g_snGen = 1; }
    int unique = 0;
    for (int i = 0; i < count; i++) {
        const DrawVertex& v = verts[i];
        uint32_t key[3];
        memcpy(&key[0], &v.x, 4); memcpy(&key[1], &v.y, 4); memcpy(&key[2], &v.w, 4);
        uint32_t h = key[0] * 0x9E3779B1u;
        h ^= key[1] + 0x7F4A7C15u + (h << 6) + (h >> 2);
        h ^= key[2] * 0x85EBCA6Bu;
        h ^= h >> 15;
        uint32_t slot = h & (SN_HASH - 1);
        int id = -1;
        for (int probe = 0; probe < 64; probe++) {
            if (g_snHashGen[slot] != g_snGen) {
                g_snHashGen[slot] = g_snGen;
                g_snHashVal[slot] = unique;
                id = unique++;
                g_snKey[id][0] = key[0]; g_snKey[id][1] = key[1]; g_snKey[id][2] = key[2];
                const float q = (v.w == v.w && v.w > 0.0f && v.w < 3.0e38f) ? v.w : 1.0f;
                const float d = 1.0f / q;
                const float ndcx = (v.x / sw) * 2.0f - 1.0f;
                const float ndcy = 1.0f - (v.y / sh) * 2.0f;
                g_snPos[id][0] = (ndcx - proj[2]) / proj[0] * d;
                g_snPos[id][1] = (ndcy + proj[3]) / proj[1] * d;
                g_snPos[id][2] = -d;
                g_snAcc[id][0] = g_snAcc[id][1] = g_snAcc[id][2] = 0.0f;
                break;
            }
            const int cand = g_snHashVal[slot];
            if (g_snKey[cand][0] == key[0] && g_snKey[cand][1] == key[1] && g_snKey[cand][2] == key[2]) {
                id = cand;
                break;
            }
            slot = (slot + 1) & (SN_HASH - 1);
        }
        if (id < 0) return;   // table too crowded: leave the flat normals
        g_snId[i] = id;
    }

    for (int t = 0; t + 2 < count; t += 3) {
        const float* p0 = g_snPos[g_snId[t]];
        const float* p1 = g_snPos[g_snId[t + 1]];
        const float* p2 = g_snPos[g_snId[t + 2]];
        const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        const float e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                      e1[2] * e2[0] - e1[0] * e2[2],
                      e1[0] * e2[1] - e1[1] * e2[0]};
        const float c[3] = {p0[0] + p1[0] + p2[0], p0[1] + p1[1] + p2[1], p0[2] + p1[2] + p2[2]};
        if (n[0] * c[0] + n[1] * c[1] + n[2] * c[2] > 0.0f) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
        for (int k = 0; k < 3; k++) {
            float* a = g_snAcc[g_snId[t + k]];
            a[0] += n[0]; a[1] += n[1]; a[2] += n[2];
        }
    }

    // MAT11b: stitch with the previous frame's world-space sums.
    float right[3], up[3], fwd[3], camPos[3];
    const bool stitch = Materials_ViewBasis(right, up, fwd, camPos);
    if (stitch) {
        const uint32_t frame = Materials_CurrentFrame();
        if (frame != g_swFrame) {
            if (g_swFrame != 0xFFFFFFFFu && (frame % 300u) == 0u && g_swLookups)
                Log("MAT11 stitch frame=%u: lookups=%u hits=%u", frame, g_swLookups, g_swHits);
            if ((frame % 300u) == 0u) { g_swLookups = 0; g_swHits = 0; }
            g_swFrame = frame;
            g_swCur ^= 1;
            if (++g_swStamp[g_swCur] == 0) {
                memset(g_swGen[g_swCur], 0, sizeof(g_swGen[g_swCur]));
                g_swStamp[g_swCur] = 1;
            }
        }
    }
    const int cur = g_swCur, prev = g_swCur ^ 1;
    // MAT14: road samples for the per-car mirror planes (reflective materials
    // of the main view only; the rear-view mirror projects with another camera).
    const bool planarSamples = stitch && CarMirror_Enabled() && !CarMirror_Pass() &&
                               SceneProbe_MainViewActive() && Materials_BoundReflection() > 0.01f;
    for (int id = 0; id < unique; id++) {
        const float* a = g_snAcc[id];
        float* o = g_snOut[id];
        o[0] = a[0]; o[1] = a[1]; o[2] = a[2];
        if (!stitch) continue;
        const float* pp = g_snPos[id];
        int32_t key[3];
        float world[3];
        for (int c = 0; c < 3; c++) {
            world[c] = camPos[c] + pp[0] * right[c] + pp[1] * up[c] - pp[2] * fwd[c];
            key[c] = SwQuant(world[c]);
            g_snWorld[id][c] = world[c];
        }
        float nw[3];
        for (int c = 0; c < 3; c++) nw[c] = a[0] * right[c] + a[1] * up[c] - a[2] * fwd[c];
        if (planarSamples) {
            const float nl2 = nw[0] * nw[0] + nw[1] * nw[1] + nw[2] * nw[2];
            if (nl2 > 1e-24f && nw[1] > 0.0f && nw[1] * nw[1] > 0.25f * nl2) {
                const float inv = 1.0f / sqrtf(nl2);
                const float un[3] = {nw[0] * inv, nw[1] * inv, nw[2] * inv};
                CarMirror_AddRoadSample(world, un);
            }
        }
        SnWorldEntry* e = SwFind(cur, key, true);
        if (e) { e->n[0] += nw[0]; e->n[1] += nw[1]; e->n[2] += nw[2]; }
        ++g_swLookups;
        const SnWorldEntry* pe = SwFind(prev, key, false);
        if (pe) {
            ++g_swHits;
            o[0] =  pe->n[0] * right[0] + pe->n[1] * right[1] + pe->n[2] * right[2];
            o[1] =  pe->n[0] * up[0]    + pe->n[1] * up[1]    + pe->n[2] * up[2];
            o[2] = -(pe->n[0] * fwd[0]  + pe->n[1] * fwd[1]   + pe->n[2] * fwd[2]);
        }
    }

    // MAT14c: the road mesh is coarse (vertices 10-20 m apart), too sparse for
    // the per-car plane fits and the road height field. Add points over every
    // up-facing triangle at ~1.5 m spacing (barycentric grid, face normal).
    if (planarSamples) {
        for (int t = 0; t + 2 < count; t += 3) {
            const float* w0 = g_snWorld[g_snId[t]];
            const float* w1 = g_snWorld[g_snId[t + 1]];
            const float* w2 = g_snWorld[g_snId[t + 2]];
            const float e1[3] = {w1[0] - w0[0], w1[1] - w0[1], w1[2] - w0[2]};
            const float e2[3] = {w2[0] - w0[0], w2[1] - w0[1], w2[2] - w0[2]};
            float fn[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                           e1[2] * e2[0] - e1[0] * e2[2],
                           e1[0] * e2[1] - e1[1] * e2[0]};
            const float fl2 = fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2];
            if (!(fl2 > 1e-12f)) continue;
            if (fn[1] < 0.0f) { fn[0] = -fn[0]; fn[1] = -fn[1]; fn[2] = -fn[2]; }
            if (fn[1] * fn[1] < 0.25f * fl2) continue;   // steeper than 60 degrees
            const float inv = 1.0f / sqrtf(fl2);
            const float un[3] = {fn[0] * inv, fn[1] * inv, fn[2] * inv};
            const float l1 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2];
            const float l2 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
            const float e3[3] = {w2[0] - w1[0], w2[1] - w1[1], w2[2] - w1[2]};
            const float l3 = e3[0] * e3[0] + e3[1] * e3[1] + e3[2] * e3[2];
            const float longest = sqrtf(l1 > l2 ? (l1 > l3 ? l1 : l3) : (l2 > l3 ? l2 : l3));
            int n = (int)(longest / 1.5f + 0.5f);
            if (n < 1) n = 1;
            if (n > 16) n = 16;
            const float step = 1.0f / (float)n;
            for (int i = 0; i <= n; i++) {
                for (int j = 0; i + j <= n; j++) {
                    if ((i == 0 || i == n) && (j == 0 || i + j == n)) continue;   // the vertices are in already
                    const float a = i * step, b = j * step;
                    const float p[3] = {w0[0] + e1[0] * a + e2[0] * b,
                                        w0[1] + e1[1] * a + e2[1] * b,
                                        w0[2] + e1[2] * a + e2[2] * b};
                    CarMirror_AddRoadSample(p, un);
                }
            }
        }
    }

    for (int i = 0; i < count; i++) {
        const float* a = g_snOut[g_snId[i]];
        const float len2 = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
        DrawVertex& v = verts[i];
        if (len2 > 1e-24f) {
            const float inv = 1.0f / sqrtf(len2);
            v.rect[0] = a[0] * inv; v.rect[1] = a[1] * inv; v.rect[2] = a[2] * inv; v.rect[3] = 1.0f;
        } else {
            v.rect[0] = v.rect[1] = v.rect[2] = v.rect[3] = 0.0f;
        }
    }
}

static inline const THRASH_Vertex* VertAt(const uint8_t* base, int index, int stride) {
    return (const THRASH_Vertex*)(base + index * stride);
}

static inline uint16_t IdxAt(const uint8_t* base, int i, int stride) {
    return *(const uint16_t*)(base + i * stride);
}


void Draw_Init() {
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = MAX_VERTS * sizeof(DrawVertex);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    g_dx.device->CreateBuffer(&bd, nullptr, &g_dynVB);
    g_vbCursor = MAX_VERTS;
    if (!g_scratch) g_scratch = new DrawVertex[MAX_VERTS];
    if (!g_mirrorScratch) g_mirrorScratch = new DrawVertex[MAX_VERTS];

    UINT stride = sizeof(DrawVertex);
    UINT offset = 0;
    g_dx.context->IASetVertexBuffers(0, 1, &g_dynVB, &stride, &offset);
}

void Draw_Shutdown() {
    if (g_dynVB) { g_dynVB->Release(); g_dynVB = nullptr; }
    delete[] g_scratch; g_scratch = nullptr;
    delete[] g_mirrorScratch; g_mirrorScratch = nullptr;
}

// G4: Glide guard band. The game submits some geometry unclipped (the sky
// dome: depth ALWAYS, no Z write); a vertex that crossed behind the camera
// projects to a huge or mirrored screen position and D3D rasterises a
// screen-sized flat-coloured triangle over the sky (the pink "gradient gone"
// frames at certain view angles). Voodoo hardware rejects triangles outside
// its guard band (about +-2048 window units at 640x480, i.e. ~3 screens);
// do the same, in units of the logical screen, and drop vertices behind the
// camera (rhw <= 0) or non-finite.
static bool g_guardBand = true;
static float g_guardScreens = 3.0f;
static int g_guardRejectedTris = 0;
static int g_guardRejectedDraws = 0;

void Draw_ConfigureGuardBand(bool enabled, float screens) {
    g_guardBand = enabled;
    g_guardScreens = screens > 1.0f ? screens : 1.0f;
}
void Draw_GuardBandStats(int* tris, int* draws, bool reset) {
    if (tris) *tris = g_guardRejectedTris;
    if (draws) *draws = g_guardRejectedDraws;
    if (reset) { g_guardRejectedTris = 0; g_guardRejectedDraws = 0; }
}

static inline bool VertexInBand(const DrawVertex& v, float xMin, float xMax, float yMin, float yMax) {
    if (!(v.x == v.x) || !(v.y == v.y) || !(v.w == v.w)) return false;   // NaN
    if (v.x < xMin || v.x > xMax || v.y < yMin || v.y > yMax) return false;
    return true;
}

// Compacts a triangle list in place; returns the surviving vertex count.
static int GuardBandFilter(DrawVertex* verts, int count) {
    if (!g_guardBand || count < 3) return count;
    const float w = (float)g_dx.logicalW, h = (float)g_dx.logicalH;
    const float xMin = -(g_guardScreens - 1.0f) * w, xMax = g_guardScreens * w;
    const float yMin = -(g_guardScreens - 1.0f) * h, yMax = g_guardScreens * h;
    int out = 0, rejected = 0;
    for (int t = 0; t + 2 < count; t += 3) {
        const DrawVertex& a = verts[t];
        const DrawVertex& b = verts[t + 1];
        const DrawVertex& c = verts[t + 2];
        // A strictly negative rhw is a vertex behind the camera that escaped
        // the game's clipper (its projection is mirrored through the centre):
        // such triangles show as stray slivers at the screen edge at certain
        // view angles. rhw == 0 is the UI's affine marker and stays.
        const bool ok = VertexInBand(a, xMin, xMax, yMin, yMax) &&
                        VertexInBand(b, xMin, xMax, yMin, yMax) &&
                        VertexInBand(c, xMin, xMax, yMin, yMax) &&
                        a.w >= 0.0f && b.w >= 0.0f && c.w >= 0.0f;
        if (ok) {
            // Diagnostic: triangles that reach more than one screen beyond an
            // edge (inside the band, so still drawn) — candidates for the
            // stray slivers seen at the screen edge.
            static int s_wideLogged = 0;
            if (s_wideLogged < 40) {
                const DrawVertex* tri[3] = {&a, &b, &c};
                bool wide = false;
                for (const DrawVertex* v : tri)
                    if (v->x < -w || v->x > 2.0f * w || v->y < -h || v->y > 2.0f * h) wide = true;
                if (wide) {
                    s_wideLogged++;
                    Log("wide tri: (%.0f,%.0f z%.3f w%.4f) (%.0f,%.0f z%.3f w%.4f) (%.0f,%.0f z%.3f w%.4f) col=%08X tex=%p",
                        a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w, c.x, c.y, c.z, c.w, a.color, Tex_Bound());
                }
            }
            if (out != t) { verts[out] = a; verts[out + 1] = b; verts[out + 2] = c; }
            out += 3;
        } else {
            rejected++;
        }
    }
    if (rejected) {
        g_guardRejectedTris += rejected;
        g_guardRejectedDraws++;
        if (State_LoggingEnabled()) {
            float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f, mnw = 1e30f;
            for (int i = 0; i < count; i++) {
                mnx = verts[i].x < mnx ? verts[i].x : mnx; mxx = verts[i].x > mxx ? verts[i].x : mxx;
                mny = verts[i].y < mny ? verts[i].y : mny; mxy = verts[i].y > mxy ? verts[i].y : mxy;
                mnw = verts[i].w < mnw ? verts[i].w : mnw;
            }
            Log("guard: rejected %d of %d tris x[%.0f..%.0f] y[%.0f..%.0f] minrhw=%.5f tex=%p",
                rejected, count / 3, mnx, mxx, mny, mxy, mnw, Tex_Bound());
        }
    }
    return out;
}

static bool g_uiUvClamp = true;
void Draw_ConfigureUiUvClamp(bool enabled) { g_uiUvClamp = enabled; }

// G10: outside the race the game's 640-designed 2D art is magnified and
// sampled bilinearly (G7), so at every sub-image edge the filter blended the
// neighbouring texels of the atlas page (transparent/black) into the edge:
// thin dark or see-through seams between adjacent UI pieces. For 2D
// triangles (rhw 0 or 1, textured) the triangle's UV bounding box is the
// sub-image it shows (both triangles of an axis-aligned quad span the whole
// quad rect); the pixel shader keeps the sample inside it by half a texel.
static void AssignUiUvRects(DrawVertex* verts, int count) {
    if (!g_uiUvClamp || State_RaceVideoMode() || Tex_Bound() == nullptr) return;
    for (int t = 0; t + 2 < count; t += 3) {
        DrawVertex* v = verts + t;
        if ((v[0].w != 1.0f && v[0].w != 0.0f) || (v[1].w != 1.0f && v[1].w != 0.0f) ||
            (v[2].w != 1.0f && v[2].w != 0.0f))
            continue;
        float lo0 = v[0].u, hi0 = v[0].u, lo1 = v[0].v, hi1 = v[0].v;
        for (int i = 1; i < 3; i++) {
            lo0 = v[i].u < lo0 ? v[i].u : lo0; hi0 = v[i].u > hi0 ? v[i].u : hi0;
            lo1 = v[i].v < lo1 ? v[i].v : lo1; hi1 = v[i].v > hi1 ? v[i].v : hi1;
        }
        if (hi0 <= lo0 || hi1 <= lo1 || lo0 < -1.0f || hi0 > 2.0f || lo1 < -1.0f || hi1 > 2.0f)
            continue;
        for (int i = 0; i < 3; i++) {
            v[i].rect[0] = lo0; v[i].rect[1] = lo1; v[i].rect[2] = hi0; v[i].rect[3] = hi1;
        }
    }
}

static float g_emissiveGain = 1.0f, g_emissiveThreshold = 0.6f, g_emissivePower = 2.0f;
// CAR2 fix 9: the same for opaque draws (mode 0) of the track and car phases
// of a capture: lamp panels, lit windows, headlight textures. A high threshold
// keeps white walls and road markings as they are.
static float g_emissiveOpaqueGain = 1.0f, g_emissiveOpaqueThreshold = 0.85f, g_emissiveOpaquePower = 4.0f;
static bool  g_emissiveOpaqueUnlit = true;   // only full-bright (unlit) vertices: lamp panels, not sunlit road markings
static bool  g_emissiveOpaqueDark = true;    // only while the game's section light is dark (tunnels, night)
void Draw_ConfigureEmissive(float gain, float threshold, float power) {
    g_emissiveGain = gain >= 1.0f && gain <= 64.0f ? gain : 1.0f;
    g_emissiveThreshold = threshold >= 0.0f && threshold < 1.0f ? threshold : 0.6f;
    g_emissivePower = power > 0.0f && power <= 16.0f ? power : 2.0f;
    g_dx.emissiveThreshold = g_emissiveThreshold;
    g_dx.emissivePower = g_emissivePower;
}
void Draw_ConfigureEmissiveOpaqueGates(bool unlitOnly, bool darkOnly) {
    g_emissiveOpaqueUnlit = unlitOnly;
    g_emissiveOpaqueDark = darkOnly;
}
void Draw_ConfigureEmissiveOpaque(float gain, float threshold, float power) {
    g_emissiveOpaqueGain = gain >= 1.0f && gain <= 64.0f ? gain : 1.0f;
    g_emissiveOpaqueThreshold = threshold >= 0.0f && threshold < 1.0f ? threshold : 0.85f;
    g_emissiveOpaquePower = power > 0.0f && power <= 16.0f ? power : 4.0f;
}

// G12: while an HDR cubemap face is being captured, additive draws (state
// 0x38 = 1: sun, lamp glows, headlights, brake lights) are written with the
// emissive gain so the R2 reflection sees them as light sources. CAR2 fix 9:
// opaque draws of the track / car phases get their own gain and threshold.
static void UpdateEmissive() {
    float e = 1.0f, t = g_emissiveThreshold, pw = g_emissivePower;
    if (SceneProbe_CubemapHdr() && SceneProbe_Active()) {
        const int mode = State_Peek(THRASH_STATE_BLEND_MODE);
        if (mode == 1 && g_emissiveGain > 1.0f) e = g_emissiveGain;
        else if (mode == 0 && g_emissiveOpaqueGain > 1.0f && g_dx.captureOpaqueGain) {
            // dark gate: full below ambient 0.3, none above 0.5 (Coastal open road
            // 0.59-0.78, its tunnels 0.25-0.36, Industrial 0.13-0.73)
            float dark = 1.0f;
            if (g_emissiveOpaqueDark) {
                const float amb = CarShading_SectionAmbient();
                if (amb >= 0.0f) { dark = (0.5f - amb) / 0.2f; dark = dark < 0.0f ? 0.0f : (dark > 1.0f ? 1.0f : dark); }
            }
            e = 1.0f + (g_emissiveOpaqueGain - 1.0f) * dark;
            t = g_emissiveOpaqueThreshold;
            pw = g_emissiveOpaqueUnlit ? -g_emissiveOpaquePower : g_emissiveOpaquePower;   // negative = vertex gate in the shader
        }
    }
    if (e != g_dx.emissive || t != g_dx.emissiveThreshold || pw != g_dx.emissivePower) {
        g_dx.emissive = e; g_dx.emissiveThreshold = t; g_dx.emissivePower = pw;
        DX11_MarkScreenCBDirty(g_dx);
    }
}

// Upload to the ring buffer and draw (the tail of every draw path).
static void SubmitVerts(const DrawVertex* verts, int count, D3D11_PRIMITIVE_TOPOLOGY topo) {
    if (count > MAX_VERTS) count = MAX_VERTS;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (g_vbCursor + count > MAX_VERTS) {
        mode = D3D11_MAP_WRITE_DISCARD; g_vbCursor = 0;
        if (g_perfOn) Perf_Global(CG_VBWRAP, 1);
    }
    if (g_perfOn) { Perf_Count(g_perfView, CT_D3D, 1); Perf_Count(g_perfView, CT_VERTS, (uint32_t)count); }
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;
    {
        PerfCallScope perf(PC_VB_MAP);
        hr = g_dx.context->Map(g_dynVB, 0, mode, 0, &mapped);
    }
    if (FAILED(hr)) return;

    {
        PerfCallScope perf(PC_VB_COPY);
        memcpy((DrawVertex*)mapped.pData + g_vbCursor, verts, count * sizeof(DrawVertex));
        g_dx.context->Unmap(g_dynVB, 0);
    }

    PerfCallScope perf(PC_DRAW);
    UINT stride = sizeof(DrawVertex);
    UINT offset = 0;
    g_dx.context->IASetVertexBuffers(0, 1, &g_dynVB, &stride, &offset);
    g_dx.context->IASetPrimitiveTopology(topo);
    g_dx.context->Draw(count, g_vbCursor);
    g_vbCursor += count;
}

// MAT14: the mirror image of one car draw (car_mirror.h). Every vertex is
// unprojected with the main camera (P = ray / rhw, as SmoothNormals),
// reflected in the car's road plane, clipped at a near plane in camera space
// (the mirror image of a car beside the camera can cross it) and projected
// again; depth comes from the mirrored distance (z = A - B/w), the texture
// coordinates keep Glide's perspective through the new rhw. The reflection
// flips the winding, so every triangle swaps two vertices.
struct MirrorVert { float p[3]; float u, v; uint32_t color, specular; };

static uint32_t LerpColor(uint32_t a, uint32_t b, float t) {
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) {
        const float ca = (float)((a >> s) & 0xFF), cb = (float)((b >> s) & 0xFF);
        int v = (int)(ca + (cb - ca) * t + 0.5f);
        v = v < 0 ? 0 : (v > 255 ? 255 : v);
        r |= (uint32_t)v << s;
    }
    return r;
}

static MirrorVert LerpMirrorVert(const MirrorVert& a, const MirrorVert& b, float t) {
    MirrorVert o;
    for (int k = 0; k < 3; k++) o.p[k] = a.p[k] + (b.p[k] - a.p[k]) * t;
    o.u = a.u + (b.u - a.u) * t;
    o.v = a.v + (b.v - a.v) * t;
    o.color = LerpColor(a.color, b.color, t);
    o.specular = LerpColor(a.specular, b.specular, t);
    return o;
}

static void MirrorFlush(const DrawVertex* verts, int count, D3D11_PRIMITIVE_TOPOLOGY topo) {
    if (topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST || !g_mirrorScratch) return;
    float nc[3], dc, proj[4];
    if (!CarMirror_Plane(nc, &dc) || !Materials_ViewProj(proj)) return;
    const float sw = (float)g_dx.logicalW, sh = (float)g_dx.logicalH;
    if (sw <= 0.0f || sh <= 0.0f || proj[0] == 0.0f || proj[1] == 0.0f) return;
    const float kNear = 0.5f;   // keep p.z <= -kNear (camera looks down -Z)
    int out = 0;
    for (int t = 0; t + 2 < count; t += 3) {
        MirrorVert tri[3];
        bool bad = false;
        for (int k = 0; k < 3; k++) {
            const DrawVertex& v = verts[t + k];
            const float q = v.w;
            if (!(q == q) || !(q > 0.0f) || !(q < 3.0e38f)) { bad = true; break; }
            const float d = 1.0f / q;
            const float ndcx = (v.x / sw) * 2.0f - 1.0f;
            const float ndcy = 1.0f - (v.y / sh) * 2.0f;
            const float p[3] = {(ndcx - proj[2]) / proj[0] * d, (ndcy + proj[3]) / proj[1] * d, -d};
            const float s = nc[0] * p[0] + nc[1] * p[1] + nc[2] * p[2] - dc;
            MirrorVert& m = tri[k];
            for (int c = 0; c < 3; c++) m.p[c] = p[c] - 2.0f * s * nc[c];
            m.u = v.u; m.v = v.v; m.color = v.color; m.specular = v.specular;
        }
        if (bad) continue;
        // Near-plane clip (Sutherland-Hodgman): at most four vertices.
        MirrorVert poly[4];
        int n = 0;
        for (int k = 0; k < 3; k++) {
            const MirrorVert& a = tri[k];
            const MirrorVert& b = tri[(k + 1) % 3];
            const bool ia = a.p[2] <= -kNear, ib = b.p[2] <= -kNear;
            if (ia) poly[n++] = a;
            if (ia != ib) poly[n++] = LerpMirrorVert(a, b, (-kNear - a.p[2]) / (b.p[2] - a.p[2]));
        }
        if (n < 3) continue;
        for (int fan = 1; fan + 1 < n; fan++) {
            if (out + 3 > MAX_VERTS) break;
            const MirrorVert* src[3] = {&poly[0], &poly[fan + 1], &poly[fan]};   // winding flipped
            DrawVertex dv[3];
            bool off = false;
            for (int k = 0; k < 3; k++) {
                const MirrorVert& m = *src[k];
                const float dist = -m.p[2];
                const float iz = 1.0f / dist;
                const float nx = m.p[0] * iz * proj[0] + proj[2];
                const float ny = m.p[1] * iz * proj[1] - proj[3];
                DrawVertex& o = dv[k];
                o.x = (nx + 1.0f) * 0.5f * sw;
                o.y = (1.0f - ny) * 0.5f * sh;
                if (!(o.x > -8.0f * sw && o.x < 9.0f * sw && o.y > -8.0f * sh && o.y < 9.0f * sh)) {
                    off = true;
                    break;
                }
                const float z = MotionBlur_DistanceToDepth(dist);
                o.z = z < 0.0f ? 0.0f : (z > 1.0f ? 1.0f : z);
                o.w = iz;
                o.u = m.u; o.v = m.v; o.color = m.color; o.specular = m.specular;
                o.rect[0] = o.rect[1] = o.rect[2] = o.rect[3] = 0.0f;
            }
            if (off) continue;
            g_mirrorScratch[out++] = dv[0];
            g_mirrorScratch[out++] = dv[1];
            g_mirrorScratch[out++] = dv[2];
        }
    }
    if (out < 3) return;
    UpdateEmissive();
    DX11_FlushScreenCB(g_dx);
    SubmitVerts(g_mirrorScratch, out, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    CarMirror_CountDraw(out / 3);
}

static void FlushVerts(DrawVertex* verts, int count, D3D11_PRIMITIVE_TOPOLOGY topo) {
    if (count <= 0 || !g_dx.context) return;
    // HZ4: NFS4 scrolling texture (waterfalls, Dolphin Cove water).
    if (const float dv = TexAnim_VOffset(Tex_BoundSlot()))
        for (int i = 0; i < count; ++i) verts[i].v += dv;
    // MAT14: during the extra car pass every draw is the mirror image of a car.
    if (CarMirror_Pass()) { MirrorFlush(verts, count, topo); return; }
    if (topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
        count = GuardBandFilter(verts, count);
        if (count <= 0) return;
        AssignUiUvRects(verts, count);
    }
    // HZ6: a keyed cutout in the sorted queue (no depth write) goes hard only
    // while every vertex is opaque; a fading object keeps its blend.
    bool hardSuppressed = false;
    if (g_dx.hardCutoutActive && !g_dx.depthWrite) {
        for (int i = 0; i < count && !hardSuppressed; ++i)
            hardSuppressed = (verts[i].color >> 24) < 0xF8;
        if (hardSuppressed) State_SuppressHardCutout(true);
    }
    UpdateEmissive();
    DX11_FlushScreenCB(g_dx);
    if (SceneProbe_Active()) SceneProbe_NoteCaptureDraw(State_Peek(THRASH_STATE_BLEND_MODE) == 1);

    // HS11: observe actual submitted cutout state, capped per reference.
    static unsigned cutoutLogs[256] = {};
    const int ref = (int)(g_dx.alphaRef * 255.0f + 0.5f);
    if (State_RaceVideoMode() && cutoutLogs[ref] < 4 &&
        (g_dx.hardCutoutActive || (ref >= 64 && ref <= 255))) {
        ++cutoutLogs[ref];
        ID3D11BlendState* bs = nullptr; float factors[4]; UINT mask;
        ID3D11SamplerState* ss = nullptr;
        g_dx.context->OMGetBlendState(&bs, factors, &mask);
        g_dx.context->PSGetSamplers(0, 1, &ss);
        D3D11_BLEND_DESC bd = {}; D3D11_SAMPLER_DESC sd = {};
        if (bs) { bs->GetDesc(&bd); bs->Release(); }
        if (ss) { ss->GetDesc(&sd); ss->Release(); }
        Log("HS11 cutout draw: ref=%02X hard=%d a2c=%d blend=%d zcmp=%d zwrite=%d sampler=%d/%d maxLOD=%.1f tex=%p uv0=(%.4f,%.4f) count=%d",
            ref, g_dx.hardCutoutActive, bd.AlphaToCoverageEnable,
            State_Peek(THRASH_STATE_BLEND_MODE), g_dx.depthComparison, g_dx.depthWrite,
            sd.AddressU, sd.AddressV, sd.MaxLOD, Tex_Bound(), verts[0].u, verts[0].v, count);
    }
    // HZ6: the first draw of each keyed bitmap below the 8% cutout rule, per
    // depth-write state, with the lowest vertex alpha.
    if (Tex_BoundIsKeyed() && !Tex_BoundIsCutout()) {
        static uint8_t s_keyedLogged[4096][2] = {};
        static int s_keyedLogs = 0;
        const int slot = Tex_BoundSlot();
        const int zw = g_dx.depthWrite ? 1 : 0;
        if (slot >= 0 && slot < 4096 && !s_keyedLogged[slot][zw] && s_keyedLogs < 96) {
            s_keyedLogged[slot][zw] = 1;
            ++s_keyedLogs;
            unsigned minA = 255;
            for (int i = 0; i < count; i++) {
                const unsigned a = verts[i].color >> 24;
                if (a < minA) minA = a;
            }
            Log("HZ6 keyed draw: slot=%d name=%.4s ref=%02X hard=%d a2c=%d blend=%d zwrite=%d "
                "zcmp=%d minVA=%u capture=%d race=%d",
                slot, Materials_SlotName(slot), ref, g_dx.hardCutoutActive, g_dx.a2cActive,
                State_Peek(THRASH_STATE_BLEND_MODE), zw, g_dx.depthComparison, minA,
                (SceneProbe_Active() || g_dx.captureAlphaMask) ? 1 : 0, State_RaceVideoMode() ? 1 : 0);
        }
    }

    // SKY1 diagnostic (docs/findings/SKY1-horizon-slit.md): the first draw of
    // each texture in the sky pass (race, depth test "always", no depth
    // write): which slot the horizon ring and the dome layers bind, its
    // format and cutout classification, the clamp states and the UV range.
    // One line per slot and texture object.
    if (State_RaceVideoMode() && g_dx.depthComparison == 7 && !g_dx.depthWrite &&
        count >= 3 && topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
        static int s_skyLogs = 0;
        const int slot = Tex_BoundSlot();
        const TexSlot* ts = Tex_SlotAt(slot);
        if (ts && s_skyLogs < 64) {
            static void* s_skyLoggedTex[4096] = {};
            if (s_skyLoggedTex[slot] != (void*)ts->tex) {
                s_skyLoggedTex[slot] = (void*)ts->tex;
                ++s_skyLogs;
                float mnU = verts[0].u, mxU = verts[0].u, mnV = verts[0].v, mxV = verts[0].v;
                for (int i = 1; i < count; i++) {
                    mnU = verts[i].u < mnU ? verts[i].u : mnU; mxU = verts[i].u > mxU ? verts[i].u : mxU;
                    mnV = verts[i].v < mnV ? verts[i].v : mnV; mxV = verts[i].v > mxV ? verts[i].v : mxV;
                }
                Log("SKY1 draw slot=%d %dx%d fmt=%d cutout=%d clampST=%d/%d blend=%d n=%d "
                    "U[%.3f..%.3f] V[%.3f..%.3f] rhw0=%.6f z0=%.4f",
                    slot, ts->width, ts->height, ts->format, ts->cutout ? 1 : 0,
                    State_Peek(0x41), State_Peek(0x42), State_Peek(THRASH_STATE_BLEND_MODE), count,
                    mnU, mxU, mnV, mxV, verts[0].w, verts[0].z);
            }
        }
    }

    // bar5 tiling cutouts: the hard-cutout path forces the sampler to CLAMP
    // and the shader insets UVs into [0,1], so a tiling cutout (stone
    // barriers, U = 0..16) was squashed into one stretched copy. Detect by
    // UV range, restore the game's WRAP for this draw and give every
    // triangle its own UV rect: the shader's rect clamp spans the
    // triangle's UV box (the full tiled span), which bypasses the inset.
    bool tilingCutout = false;
    if (g_dx.hardCutoutActive && State_RaceVideoMode() && count >= 3 &&
        topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
        float mnU = 1e30f, mxU = -1e30f, mnV = 1e30f, mxV = -1e30f;
        for (int i = 0; i < count; i++) {
            if (verts[i].u < mnU) mnU = verts[i].u;
            if (verts[i].u > mxU) mxU = verts[i].u;
            if (verts[i].v < mnV) mnV = verts[i].v;
            if (verts[i].v > mxV) mxV = verts[i].v;
        }
        if (mxU > 1.01f || mnU < -0.01f || mxV > 1.01f || mnV < -0.01f) {
            tilingCutout = true;
            State_OverrideCutoutClamp(true);
            // MAT11: a material draw carries its smooth normal in rect and the
            // material shader has no UV inset: keep WRAP, leave rect alone.
            for (int t = 0; !g_materialDraw && t + 2 < count; t += 3) {
                DrawVertex* v = verts + t;
                float lo0 = v[0].u, hi0 = v[0].u, lo1 = v[0].v, hi1 = v[0].v;
                for (int i = 1; i < 3; i++) {
                    lo0 = v[i].u < lo0 ? v[i].u : lo0;
                    hi0 = v[i].u > hi0 ? v[i].u : hi0;
                    lo1 = v[i].v < lo1 ? v[i].v : lo1;
                    hi1 = v[i].v > hi1 ? v[i].v : hi1;
                }
                for (int i = 0; i < 3; i++) {
                    v[i].rect[0] = lo0; v[i].rect[1] = lo1;
                    v[i].rect[2] = hi0; v[i].rect[3] = hi1;
                }
            }
        }
    }

    SubmitVerts(verts, count, topo);
    if (tilingCutout) State_OverrideCutoutClamp(false);
    if (hardSuppressed) State_SuppressHardCutout(false);
}

// MAT7/MAT11: one material-shaded triangle list.
static void FlushMaterial(DrawVertex* verts, int count) {
    {
        PerfScope perf(PZ_MATCB);
        Materials_BeginShade();
    }
    if (g_perfOn) {
        Perf_Count(g_perfView, CT_MAT, 1);
        if (Materials_LastShadeWasCar()) Perf_Count(g_perfView, CT_CARMAT, 1);
    }
    // CAR1: the car's own normals already sit in rect (BuildTriMesh); the
    // screen-space weld would replace them and feed the road-sample map.
    if (!g_carNormals && !g_paintPageDraw) {
        PerfScope perf(PZ_SMOOTH);
        SmoothNormals(verts, count);
    }
    g_materialDraw = true;
    FlushVerts(verts, count, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_materialDraw = false;
    PerfScope perf(PZ_MATCB);
    Materials_EndShade();
}

static void DecodePaintPageNormals(DrawVertex* verts, int n);

// CP1: a single triangle / quad on a car paint page (a piece the game's
// clipper cut) is shaded like the rest of the car; anything else as before.
static bool ShadePaintPiece(DrawVertex* verts, int n) {
    g_lastTriMeshCar = false;
    if (!CarPaint_SlotSrv(Tex_BoundSlot()) || !Materials_ShouldShade()) return false;
    DecodePaintPageNormals(verts, n);
    FlushMaterial(verts, n);
    g_lastTriMeshCar = Materials_LastShadeWasCar();
    return true;
}

void Draw_Tri(THRASH_Vertex* v0, THRASH_Vertex* v1, THRASH_Vertex* v2) {
    DrawVertex verts[3] = {ConvertVertex(v0), ConvertVertex(v1), ConvertVertex(v2)};
    if (ShadePaintPiece(verts, 3)) return;
    FlushVerts(verts, 3, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Draw_Quad(THRASH_Vertex* v0, THRASH_Vertex* v1, THRASH_Vertex* v2, THRASH_Vertex* v3) {
    DrawVertex verts[6] = {
        ConvertVertex(v0), ConvertVertex(v1), ConvertVertex(v2),
        ConvertVertex(v0), ConvertVertex(v2), ConvertVertex(v3)
    };
    if (ShadePaintPiece(verts, 6)) return;
    FlushVerts(verts, 6, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

// CP1: the game's clipper drops the specular dword (= the car normal) of the
// vertices of a polygon it cut by more than one plane (the screen's bottom
// and side edges near the camera): they arrive with code 0 and shaded black.
// Such a corner takes the normal of the nearest corner of the same draw that
// has one (on screen; the cut corners lie on the polygon's own edges).
static void FillMissingCarNormals(DrawVertex* verts, int n) {
    int missing = 0, valid = 0;
    for (int i = 0; i < n; i++) (verts[i].rect[3] == 1.0f ? valid : missing)++;
    if (!missing || !valid) return;
    for (int i = 0; i < n; i++) {
        DrawVertex& v = verts[i];
        if (v.rect[3] == 1.0f) continue;
        float best = 3.4e38f;
        int bi = -1;
        for (int j = 0; j < n; j++) {
            const DrawVertex& o = verts[j];
            if (o.rect[3] != 1.0f) continue;
            const float dx = o.x - v.x, dy = o.y - v.y;
            const float d2 = dx * dx + dy * dy;
            if (d2 < best) { best = d2; bi = j; }
        }
        if (bi < 0) continue;
        v.rect[0] = verts[bi].rect[0]; v.rect[1] = verts[bi].rect[1];
        v.rect[2] = verts[bi].rect[2]; v.rect[3] = 1.0f;
    }
    static uint32_t s_filled = 0;
    if (++s_filled <= 3 || (s_filled % 5000) == 0)
        Log("CP1 clip: %d corner(s) without a car normal took the nearest one's (%d with) #%u", missing, valid, s_filled);
}

// CP1: the game's clipper appends the corners of the polygons it cuts to the
// EAGL vertex array after the model's own vertices (index >= E+4) and fills
// position, colour and UV, never the specular dword (0 on stock cars): those
// slots keep the normal codes of an earlier, larger batch - another corner's
// normal, the cut polygons shaded dark at the screen edge. A copy of a model
// corner takes that corner's code (same screen position); a corner the
// clipper made on the cut takes the nearest corner's normal later
// (FillMissingCarNormals; the model corners outside the screen are not
// projected, so the cut edge cannot be followed to its far end).
static uint32_t g_cornerCode[MAX_VERTS];
static bool g_cornerCodesValid = false;

static void ResolveClipCodes(int count, const void* indices, const THRASH_Vertex* vertices) {
    g_cornerCodesValid = false;
    const uint8_t* eagl = R_GuestPtrAt(0x0069E0D4u);   // nfs5-web: game memory through the runtime
    if (!eagl || (const uint8_t*)vertices != eagl + 0x434) return;
    const int orig = *(const int*)(eagl + 4);
    if (orig <= 0 || orig > 8192) return;
    const uint8_t* ib = (const uint8_t*)indices;
    const uint8_t* vb = (const uint8_t*)vertices;
    const int corners = count * 3 < MAX_VERTS ? count * 3 : MAX_VERTS;
    bool any = false;
    for (int i = 0; i < corners && !any; ++i) any = IdxAt(ib, i, g_indexStride) >= orig;
    if (!any) return;
    int copies = 0, made = 0;
    for (int i = 0; i < corners; ++i) {
        const int idx = IdxAt(ib, i, g_indexStride);
        const THRASH_Vertex* v = VertAt(vb, idx, g_vertexStride);
        g_cornerCode[i] = v->specular & 0xFFFFFF;
        if (idx < orig) continue;
        g_cornerCode[i] = 0;
        for (int j = 0; j < orig; ++j) {
            const THRASH_Vertex* o = VertAt(vb, j, g_vertexStride);
            if (o->x == v->x && o->y == v->y && o->z == v->z) {
                g_cornerCode[i] = o->specular & 0xFFFFFF;
                break;
            }
        }
        (g_cornerCode[i] ? copies : made)++;
    }
    g_cornerCodesValid = true;
    static uint32_t s_logs = 0;
    if (++s_logs <= 3 || (s_logs % 5000) == 0)
        Log("CP1 clip: batch of %d tris uses clipper corners past the %d model vertices: %d copies of a model corner, %d new on the cut #%u",
            count, orig, copies, made, s_logs);
}

// De-indexes a trimesh into g_scratch. maskUv (R3): count*3 diffuse (u, v)
// pairs in corner order, stored in rect.xy with rect.z = 1 so the reflection
// shader can read the diffuse texture's alpha there.
static int BuildTriMesh(int count, const void* indices, const THRASH_Vertex* vertices,
                        const float* maskUv, bool carNormalsToRect = false) {
    const uint8_t* idxBase = (const uint8_t*)indices;
    const uint8_t* vtxBase = (const uint8_t*)vertices;
    int iStride = g_indexStride;
    int vStride = g_vertexStride;

    int numVerts = count * 3;
    if (numVerts > MAX_VERTS) numVerts = MAX_VERTS;

    DrawVertex* verts = g_scratch;
    for (int i = 0; i < numVerts; i++) {
        uint16_t idx = IdxAt(idxBase, i, iStride);
        verts[i] = ConvertVertex(VertAt(vtxBase, idx, vStride));
        if (g_carNormals || carNormalsToRect) {
            const uint32_t code = g_cornerCodesValid ? g_cornerCode[i] : verts[i].specular & 0xFFFFFF;
            verts[i].specular = 0;
            if (code && carNormalsToRect) {
                float n[3];
                CarShading_DecodeNormal(code, n);
                verts[i].rect[0] = n[0]; verts[i].rect[1] = n[1]; verts[i].rect[2] = n[2]; verts[i].rect[3] = 1.0f;
            }
        }
        if (maskUv) {
            verts[i].rect[0] = maskUv[i * 2];
            verts[i].rect[1] = maskUv[i * 2 + 1];
            verts[i].rect[2] = 1.0f;
        }
    }
    return numVerts;
}

void Draw_TriMesh(int count, void* indices, THRASH_Vertex* vertices) {
    if (count <= 0 || !indices || !vertices) return;
    const bool shade = Materials_ShouldShade();
    // CP1: a batch the game clipped at the screen edge comes from its clip
    // buffer, not the vertex stream the light layer hook armed; on a car paint
    // page the specular dwords still carry the normals - decode them anyway.
    const bool paintPage = shade && CarPaint_SlotSrv(Tex_BoundSlot()) != nullptr;
    if (paintPage && !g_carNormals) {
        static uint32_t s_unarmed = 0;
        if (++s_unarmed <= 3 || (s_unarmed % 5000) == 0)
            Log("CP1 clip: paint page trimesh outside the armed stream #%u (%d tris), normals decoded", s_unarmed, count);
    }
    g_paintPageDraw = paintPage;
    const bool carCodes = (g_carNormals || paintPage) && shade;
    if (carCodes) ResolveClipCodes(count, indices, vertices);
    const int numVerts = BuildTriMesh(count, indices, vertices, nullptr, carCodes);
    g_cornerCodesValid = false;
    if (g_carNormals || paintPage) {   // CP3 diagnostics
        int ok = 0;
        if (carCodes) for (int i = 0; i < numVerts; i++) ok += g_scratch[i].rect[3] == 1.0f;
        CarPaint_NoteCarDraw(Tex_BoundSlot(), count, shade, ok, carCodes ? numVerts - ok : 0);
    }
    if (paintPage) FillMissingCarNormals(g_scratch, numVerts);
    DrawVertex* verts = g_scratch;
    // MAT7: material shading for track textures with non-neutral materials.
    g_lastTriMeshCar = false;
    if (shade) {
        FlushMaterial(verts, numVerts);
        g_lastTriMeshCar = Materials_LastShadeWasCar();
        return;
    }
    // Diagnostic: geometry statistics of untextured meshes (the sky dome).
    if (State_LoggingEnabled() && Tex_Bound() == nullptr) {
        float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f;
        float mnw = 1e30f, mxw = -1e30f, mnz = 1e30f, mxz = -1e30f;
        int badW = 0, nan = 0;
        for (int i = 0; i < numVerts; i++) {
            const DrawVertex& v = verts[i];
            if (!(v.x == v.x) || !(v.y == v.y) || !(v.z == v.z) || !(v.w == v.w)) { nan++; continue; }
            mnx = v.x < mnx ? v.x : mnx; mxx = v.x > mxx ? v.x : mxx;
            mny = v.y < mny ? v.y : mny; mxy = v.y > mxy ? v.y : mxy;
            mnw = v.w < mnw ? v.w : mnw; mxw = v.w > mxw ? v.w : mxw;
            mnz = v.z < mnz ? v.z : mnz; mxz = v.z > mxz ? v.z : mxz;
            if (v.w <= 0.0f) badW++;
        }
        Log("skygeo tris=%d x[%.0f..%.0f] y[%.0f..%.0f] rhw[%.5f..%.5f] z[%.3f..%.3f] w<=0:%d nan:%d",
            count, mnx, mxx, mny, mxy, mnw, mxw, mnz, mxz, badW, nan);
        // The actual GPU pipeline at this draw, independent of what the wrapper believes.
        ID3D11BlendState* bl = nullptr; float bf[4]; UINT sm = 0;
        ID3D11DepthStencilState* ds = nullptr; UINT sref = 0;
        ID3D11RasterizerState* rs = nullptr;
        ID3D11RenderTargetView* rtv = nullptr; ID3D11DepthStencilView* dsv = nullptr;
        ID3D11PixelShader* ps = nullptr; ID3D11VertexShader* vs = nullptr;
        ID3D11InputLayout* il = nullptr; ID3D11Buffer* pcb = nullptr; ID3D11Buffer* vcb = nullptr;
        ID3D11ShaderResourceView* srv = nullptr; ID3D11SamplerState* smp = nullptr;
        D3D11_VIEWPORT vp[16] = {}; UINT nvp = 16; D3D11_RECT sc[16] = {}; UINT nsc = 16;
        auto* c = g_dx.context;
        c->OMGetBlendState(&bl, bf, &sm); c->OMGetDepthStencilState(&ds, &sref); c->RSGetState(&rs);
        c->OMGetRenderTargets(1, &rtv, &dsv); c->PSGetShader(&ps, nullptr, nullptr); c->VSGetShader(&vs, nullptr, nullptr);
        c->IAGetInputLayout(&il); c->PSGetConstantBuffers(0, 1, &pcb); c->VSGetConstantBuffers(0, 1, &vcb);
        c->PSGetShaderResources(0, 1, &srv); c->PSGetSamplers(0, 1, &smp);
        c->RSGetViewports(&nvp, vp); c->RSGetScissorRects(&nsc, sc);
        D3D11_DEPTH_STENCIL_DESC dd = {}; if (ds) ds->GetDesc(&dd);
        D3D11_RASTERIZER_DESC rd = {}; if (rs) rs->GetDesc(&rd);
        D3D11_BLEND_DESC bd = {}; if (bl) bl->GetDesc(&bd);
        int blendMode = -1;
        for (int i = 0; i < 4; i++) if (bl == g_dx.blendModes[i]) blendMode = i;
        Log("gpu: blend=%d(en%d %d/%d) depth=en%d func%d w%d rs=cull%d sc%d dclip%d vp=%u(%.0f,%.0f,%.0f,%.0f z%.2f-%.2f) sr=%u(%ld,%ld,%ld,%ld) rtv=%s dsv=%s ps=%s vs=%s il=%d cb=%d/%d srv=%s smp=%p",
            blendMode, bd.RenderTarget[0].BlendEnable, bd.RenderTarget[0].SrcBlend, bd.RenderTarget[0].DestBlend,
            dd.DepthEnable, dd.DepthFunc, dd.DepthWriteMask, rd.CullMode, rd.ScissorEnable, rd.DepthClipEnable,
            nvp, vp[0].TopLeftX, vp[0].TopLeftY, vp[0].Width, vp[0].Height, vp[0].MinDepth, vp[0].MaxDepth,
            nsc, sc[0].left, sc[0].top, sc[0].right, sc[0].bottom,
            rtv == g_dx.rtv ? "main" : "OTHER", dsv == g_dx.dsv ? "main" : "OTHER",
            ps == g_dx.pixelShaderFlat ? "flat" : (ps == g_dx.pixelShaderTex ? "tex" : "OTHER"),
            vs == g_dx.vertexShader ? "main" : "OTHER", il == g_dx.inputLayout, pcb == g_dx.cbScreen, vcb == g_dx.cbScreen,
            srv ? "set" : "null", (void*)smp);
        if (bl) bl->Release(); if (ds) ds->Release(); if (rs) rs->Release(); if (rtv) rtv->Release(); if (dsv) dsv->Release();
        if (ps) ps->Release(); if (vs) vs->Release(); if (il) il->Release(); if (pcb) pcb->Release(); if (vcb) vcb->Release();
        if (srv) srv->Release(); if (smp) smp->Release();
    }
    FlushVerts(verts, numVerts, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Draw_TriMeshReflect(int count, void* indices, THRASH_Vertex* vertices,
                         const ReflectConstants& constants, bool alphaBlend,
                         ID3D11ShaderResourceView* cube,
                         const float* maskUv, ID3D11ShaderResourceView* diffuse,
                         ID3D11ShaderResourceView* paint) {
    if (count <= 0 || !indices || !vertices || !g_dx.context) return;
    if (!g_dx.reflectVS || !g_dx.reflectPS || !g_dx.cbReflect || !cube) {
        Draw_TriMesh(count, indices, vertices);
        return;
    }
    if (!diffuse) maskUv = nullptr;
    if (!maskUv) paint = nullptr;
    ReflectConstants cb = constants;
    cb.params[2] = alphaBlend ? 1.0f : 0.0f;
    cb.camRight[3] = paint ? 1.0f : 0.0f;   // CP1: paint map bound at t3
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(g_dx.context->Map(g_dx.cbReflect, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &cb, sizeof(cb));
        g_dx.context->Unmap(g_dx.cbReflect, 0);
    }
    // AA1: the reflection's alpha is an intensity, not a cutout; never let
    // alpha-to-coverage dither it.
    ID3D11BlendState* a2cBlend = g_dx.boundBlend;   // PERF5: no OMGetBlendState
    int a2cMode = -1;
    for (int i = 0; i < 4; i++) if (a2cBlend && a2cBlend == g_dx.blendModesA2C[i]) a2cMode = i;
    // (The reflect shader keeps its own alpha test and ignores emissive.y.)
    if (a2cMode >= 0) DX11_SetBlend(g_dx, g_dx.blendModes[a2cMode]);
    g_dx.context->VSSetShader(g_dx.reflectVS, nullptr, 0);
    g_dx.context->PSSetShader(g_dx.reflectPS, nullptr, 0);
    g_dx.context->VSSetConstantBuffers(1, 1, &g_dx.cbReflect);
    g_dx.context->PSSetConstantBuffers(1, 1, &g_dx.cbReflect);
    g_dx.context->PSSetShaderResources(1, 1, &cube);
    if (maskUv) g_dx.context->PSSetShaderResources(2, 1, &diffuse);
    if (paint) g_dx.context->PSSetShaderResources(3, 1, &paint);

    // R3: the mask UVs ride in rect.xy (rect.z = 1); the guard band and UI
    // rect passes leave them alone in the race.
    FlushVerts(g_scratch, BuildTriMesh(count, indices, vertices, maskUv),
               D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // G12: additive pass for the HDR excess (light sources brighter than the
    // paint). Same geometry and inputs, params.w = 1, blend SrcAlpha/ONE.
    if (g_emissiveGain > 1.0f && SceneProbe_CubemapHdr() && g_dx.blendModes[1]) {
        cb.params[3] = 1.0f;
        if (SUCCEEDED(g_dx.context->Map(g_dx.cbReflect, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, &cb, sizeof(cb));
            g_dx.context->Unmap(g_dx.cbReflect, 0);
        }
        ID3D11BlendState* prevBlend = g_dx.boundBlend;
        DX11_SetBlend(g_dx, g_dx.blendModes[1]);
        FlushVerts(g_scratch, BuildTriMesh(count, indices, vertices, maskUv),
                   D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        DX11_SetBlend(g_dx, prevBlend);
    }

    ID3D11ShaderResourceView* nullSrv = nullptr;
    g_dx.context->PSSetShaderResources(1, 1, &nullSrv);
    if (maskUv) g_dx.context->PSSetShaderResources(2, 1, &nullSrv);
    if (paint) g_dx.context->PSSetShaderResources(3, 1, &nullSrv);
    // PERF3: back to the normal draw path's shaders (no Get*Shader + Release).
    g_dx.context->VSSetShader(g_dx.vertexShader, nullptr, 0);
    g_dx.context->PSSetShader(g_dx.boundPS ? g_dx.boundPS : g_dx.pixelShaderTex, nullptr, 0);
    if (a2cMode >= 0) DX11_SetBlend(g_dx, a2cBlend);
}

// CP1: polygons the game clips at the near plane / guard band come as fans
// and strips; on a car paint page their specular dwords still carry the car
// normal (interpolated per byte by the clipper), decode it like the trimesh
// path does, otherwise the paint shades them with a faceted normal.
static void DecodePaintPageNormals(DrawVertex* verts, int n) {
    g_paintPageDraw = CarPaint_SlotSrv(Tex_BoundSlot()) != nullptr;
    if (!g_paintPageDraw) return;
    static uint32_t s_fans = 0;
    const uint32_t first = n > 0 ? (verts[0].specular & 0xFFFFFF) : 0;
    for (int i = 0; i < n; i++) {
        const uint32_t code = verts[i].specular & 0xFFFFFF;
        verts[i].specular = 0;
        if (!code) continue;
        float nrm[3];
        CarShading_DecodeNormal(code, nrm);
        verts[i].rect[0] = nrm[0]; verts[i].rect[1] = nrm[1]; verts[i].rect[2] = nrm[2]; verts[i].rect[3] = 1.0f;
    }
    FillMissingCarNormals(verts, n);
    if (++s_fans <= 3 || (s_fans % 5000) == 0)
        Log("CP1 clip: fan/strip on a paint page #%u (%d corners, first normal code %06X)", s_fans, n, first);
}

void Draw_TriFan(int count, void* indices, THRASH_Vertex* vertices) {
    if (count < 3 || !indices || !vertices) return;

    const uint8_t* idxBase = (const uint8_t*)indices;
    const uint8_t* vtxBase = (const uint8_t*)vertices;
    int iStride = g_indexStride;
    int vStride = g_vertexStride;

    int numTris = count - 2;
    int numVerts = numTris * 3;
    if (numVerts > MAX_VERTS) numVerts = MAX_VERTS;

    DrawVertex* verts = g_scratch;
    int vi = 0;
    DrawVertex center = ConvertVertex(VertAt(vtxBase, IdxAt(idxBase, 0, iStride), vStride));
    for (int i = 0; i < numTris && vi + 2 < MAX_VERTS; i++) {
        verts[vi++] = center;
        verts[vi++] = ConvertVertex(VertAt(vtxBase, IdxAt(idxBase, i + 1, iStride), vStride));
        verts[vi++] = ConvertVertex(VertAt(vtxBase, IdxAt(idxBase, i + 2, iStride), vStride));
    }
    g_lastTriMeshCar = false;
    if (Materials_ShouldShade()) {
        DecodePaintPageNormals(verts, vi);
        FlushMaterial(verts, vi);
        g_lastTriMeshCar = Materials_LastShadeWasCar();
    } else {
        FlushVerts(verts, vi, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
}

void Draw_TriStrip(int count, void* indices, THRASH_Vertex* vertices) {
    if (count < 3 || !indices || !vertices) return;

    const uint8_t* idxBase = (const uint8_t*)indices;
    const uint8_t* vtxBase = (const uint8_t*)vertices;
    int iStride = g_indexStride;
    int vStride = g_vertexStride;

    int numTris = count - 2;
    int numVerts = numTris * 3;
    if (numVerts > MAX_VERTS) numVerts = MAX_VERTS;

    DrawVertex* verts = g_scratch;
    int vi = 0;
    for (int i = 0; i < numTris && vi + 2 < MAX_VERTS; i++) {
        if (i & 1) {
            verts[vi++] = ConvertVertex(VertAt(vtxBase, IdxAt(idxBase, i + 1, iStride), vStride));
            verts[vi++] = ConvertVertex(VertAt(vtxBase, IdxAt(idxBase, i, iStride), vStride));
            verts[vi++] = ConvertVertex(VertAt(vtxBase, IdxAt(idxBase, i + 2, iStride), vStride));
        } else {
            verts[vi++] = ConvertVertex(VertAt(vtxBase, IdxAt(idxBase, i, iStride), vStride));
            verts[vi++] = ConvertVertex(VertAt(vtxBase, IdxAt(idxBase, i + 1, iStride), vStride));
            verts[vi++] = ConvertVertex(VertAt(vtxBase, IdxAt(idxBase, i + 2, iStride), vStride));
        }
    }
    g_lastTriMeshCar = false;
    if (Materials_ShouldShade()) {
        DecodePaintPageNormals(verts, vi);
        FlushMaterial(verts, vi);
        g_lastTriMeshCar = Materials_LastShadeWasCar();
    } else {
        FlushVerts(verts, vi, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
}

void Draw_Line(THRASH_Vertex* v0, THRASH_Vertex* v1) {
    DrawVertex verts[2] = {ConvertVertex(v0), ConvertVertex(v1)};
    FlushVerts(verts, 2, D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
}

void Draw_Point(THRASH_Vertex* v0) {
    DrawVertex verts[1] = {ConvertVertex(v0)};
    FlushVerts(verts, 1, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
}

void Draw_Sprite(THRASH_Vertex* v0, THRASH_Vertex* v1) {
    THRASH_Vertex tl = *v0;
    THRASH_Vertex tr = *v0; tr.x = v1->x; tr.u = v1->u;
    THRASH_Vertex bl = *v0; bl.y = v1->y; bl.v = v1->v;
    THRASH_Vertex br = *v1;

    Draw_Quad(&tl, &tr, &br, &bl);
}
