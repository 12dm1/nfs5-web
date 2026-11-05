#pragma once
#include <cmath>
#include <algorithm>

// Vertical walls: constrain XZ only, preserving suspension height and velocity.
// SimD's right vector may be banked; its XZ length scales the wall offsets too.
struct HsCorridorResult {
    bool valid = false;
    float lateral = 0, shift = 0, removedVelocity = 0;
};

inline HsCorridorResult HsCorridorConstrain(float* pos, float* vel,
        const float* center, const float* right, float left, float rightWall,
        float half, float lookahead) {
    HsCorridorResult out;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(pos[i]) || !std::isfinite(vel[i]) ||
            !std::isfinite(center[i]) || !std::isfinite(right[i])) return out;
    if (!std::isfinite(left) || !std::isfinite(rightWall) || left < 2 || rightWall < 2)
        return out;
    const float len = std::sqrt(right[0]*right[0] + right[2]*right[2]);
    if (len < 0.5f || len > 1.5f) return out;
    if (!std::isfinite(half) || half < 0.5f || half > 2.5f) half = 0.95f;
    const float rx = right[0]/len, rz = right[2]/len;
    const float lo = -std::max(1.0f, left*len - half);
    const float hi = std::max(1.0f, rightWall*len - half);
    out.lateral = (pos[0]-center[0])*rx + (pos[2]-center[2])*rz;
    const float target = std::clamp(out.lateral, lo, hi);
    out.shift = target - out.lateral;
    pos[0] += rx*out.shift;
    pos[2] += rz*out.shift;
    const float v = vel[0]*rx + vel[2]*rz;
    float allowed = v;
    if (v > 0 && (out.lateral >= hi || (lookahead > 0 && target+v*lookahead > hi)))
        allowed = lookahead > 0 ? std::max(0.0f, (hi-target)/lookahead) : 0;
    if (v < 0 && (out.lateral <= lo || (lookahead > 0 && target+v*lookahead < lo)))
        allowed = lookahead > 0 ? std::min(0.0f, (lo-target)/lookahead) : 0;
    out.removedVelocity = v-allowed;
    vel[0] -= rx*out.removedVelocity;
    vel[2] -= rz*out.removedVelocity;
    out.valid = true;
    return out;
}

// NFS4 walls are FRD road-polygon edges, not SimD leftWall/rightWall.
// A sidecar (.hsw) stores each edge and its display height. Collision is a
// circle in XZ; Y is only used to ignore a short overpass wall from the
// deck above it.
#pragma pack(push, 1)
struct HsWallSeg {
    float ax, ay, az, bx, by, bz, h;
};
#pragma pack(pop)
static_assert(sizeof(HsWallSeg) == 28, "HSW1 record is 7 floats");

// HZ7 (.hsw tail flag): every edge has its free side on the left of a->b in XZ, the
// road it bounds or the outside of a pole. A car whose centre is on the other
// side is not held by it: NFS4 keeps a car on its own road only, so a car
// below a deck or beside a shortcut flew straight through the deck's edge.
inline bool HsWallFacing(const HsWallSeg& s, const float* pos) {
    return (s.bx - s.ax) * (pos[2] - s.az) - (s.bz - s.az) * (pos[0] - s.ax) >= 0;
}

inline bool HsWallYHits(const HsWallSeg& s, float y) {
    const float lo = std::min(s.ay, s.by) - 1.0f;
    const float hi = std::max(s.ay, s.by) + s.h - 0.2f;
    return y >= lo && y <= hi;
}

// The car is its OBB (car+364/370/37C axes, car+3A8/3AC/3B0 half-extents),
// not a circle of the half-width: a circle let the nose and the corners sink
// into the wall by hl*sin(yaw), ~0.4 m at a 10-degree graze and >1 m head-on.
struct HsCarBox {
    float axis[3][3];   // right, up, forward (world)
    float half[3];      // matching half-extents
};

inline HsCarBox HsCarBoxCircle(float half) {
    HsCarBox b = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {half, 0, half}};
    return b;
}

// Half-width of the box projected on the horizontal unit direction (ux, uz).
inline float HsBoxSupport(const HsCarBox& b, float ux, float uz) {
    float s = 0;
    for (int i = 0; i < 3; ++i)
        s += b.half[i] * std::fabs(b.axis[i][0] * ux + b.axis[i][2] * uz);
    return s;
}

inline bool HsCarBoxValid(const HsCarBox& b) {
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(b.half[i]) || b.half[i] < 0 || b.half[i] > 6.0f) return false;
        float l2 = 0;
        for (int k = 0; k < 3; ++k) {
            if (!std::isfinite(b.axis[i][k])) return false;
            l2 += b.axis[i][k] * b.axis[i][k];
        }
        if (l2 < 0.5f || l2 > 1.5f) return false;
    }
    return b.half[0] >= 0.3f && b.half[2] >= 0.3f;
}

// Signed XZ separation of box and segment (SAT over the segment normal and
// the box's horizontal right/forward axes); < 0 is penetration depth.
// (nx, nz) is the push direction from the wall towards the car.
inline float HsBoxSegSeparation(const HsCarBox& b, const float* pos,
        const HsWallSeg& s, float& nx, float& nz, int* axis = nullptr) {
    const float abx = s.bx - s.ax, abz = s.bz - s.az;
    const float len = std::sqrt(abx * abx + abz * abz);
    const float mx = 0.5f * (s.ax + s.bx), mz = 0.5f * (s.az + s.bz);
    const float cx = pos[0] - mx, cz = pos[2] - mz;
    float axes[3][2] = {{-abz / len, abx / len}, {0, 0}, {0, 0}};
    int count = 1;
    for (int i = 0; i < 3; i += 2) {
        const float ax = b.axis[i][0], az = b.axis[i][2];
        const float l = std::sqrt(ax * ax + az * az);
        if (l < 0.2f) continue;     // car on its side: that axis is vertical
        axes[count][0] = ax / l;
        axes[count][1] = az / l;
        ++count;
    }
    float best = -1e30f;
    for (int k = 0; k < count; ++k) {
        float ux = axes[k][0], uz = axes[k][1];
        float c = cx * ux + cz * uz;
        if (k == 0 && std::fabs(c) < 1e-4f) c = 0;
        if (c < 0) { ux = -ux; uz = -uz; c = -c; }
        const float seg = 0.5f * std::fabs(abx * ux + abz * uz);
        const float sep = c - HsBoxSupport(b, ux, uz) - seg;
        // Prefer the wall normal on near-ties so flat walls push straight out.
        if (sep > best + (k == 0 ? 0.0f : 1e-3f)) {
            best = sep;
            nx = ux;
            nz = uz;
            if (axis) *axis = k;
        }
    }
    return best;
}

// The segment's own normal, pointing to the car centre, and the box's
// separation along it (< 0 = overlap).
inline float HsWallNormalSeparation(const HsCarBox& b, const float* pos,
        const HsWallSeg& s, float& nx, float& nz) {
    const float abx = s.bx - s.ax, abz = s.bz - s.az;
    const float len = std::sqrt(abx * abx + abz * abz);
    float ux = -abz / len, uz = abx / len;
    float c = (pos[0] - 0.5f * (s.ax + s.bx)) * ux + (pos[2] - 0.5f * (s.az + s.bz)) * uz;
    if (c < 0) { ux = -ux; uz = -uz; c = -c; }
    nx = ux;
    nz = uz;
    return c - HsBoxSupport(b, ux, uz);
}

// HS19b: a vertex shared with another wall piece is not an end the car can
// hit with its own face: the chain continues there. Sliding along a wall
// with the side touching it, the nose reached the next piece's first vertex
// and the swept box reported the car's forward axis (hometown 14:30, slice
// 440, 54 m/s stopped and flipped). Checks the end of segs[self] nearer to p.
inline bool HsWallSharedEnd(const HsWallSeg* segs, int n, int self, const float* p) {
    const HsWallSeg& s = segs[self];
    const float abx = s.bx - s.ax, abz = s.bz - s.az;
    const bool endB = (p[0] - s.ax) * abx + (p[2] - s.az) * abz > 0.5f * (abx * abx + abz * abz);
    const float ex = endB ? s.bx : s.ax, ez = endB ? s.bz : s.az;
    const float tol2 = 0.05f * 0.05f;
    for (int i = 0; i < n; ++i) {
        if (i == self) continue;
        const HsWallSeg& o = segs[i];
        const float d1x = o.ax - ex, d1z = o.az - ez, d2x = o.bx - ex, d2z = o.bz - ez;
        if (d1x * d1x + d1z * d1z < tol2 || d2x * d2x + d2z * d2z < tol2) return true;
    }
    return false;
}

// HS19: swept SAT for the box translated by (dx, dz) this step. The contact
// time is the latest entry over all axes, its axis the normal; the box only
// meets the segment if every axis overlaps at once. The largest current
// separation alone is not a prediction: for the next piece of a wall running
// beside the car it is the car's forward axis (nose to the shared vertex), so
// a car 1.3 m off the wall was "hit" head-on at full speed and flipped.
// False when not approaching or already overlapping (HsWallNearest's case).
// gap is the current separation along the returned normal (>= 0).
inline bool HsBoxSegSweep(const HsCarBox& b, const float* pos, float dx, float dz,
        const HsWallSeg& s, float& t, float& nx, float& nz, float& gap,
        int* axis = nullptr) {
    const float abx = s.bx - s.ax, abz = s.bz - s.az;
    const float len = std::sqrt(abx * abx + abz * abz);
    const float mx = 0.5f * (s.ax + s.bx), mz = 0.5f * (s.az + s.bz);
    const float cx = pos[0] - mx, cz = pos[2] - mz;
    float axes[3][2] = {{-abz / len, abx / len}, {0, 0}, {0, 0}};
    int count = 1;
    for (int i = 0; i < 3; i += 2) {
        const float ax = b.axis[i][0], az = b.axis[i][2];
        const float l = std::sqrt(ax * ax + az * az);
        if (l < 0.2f) continue;
        axes[count][0] = ax / l;
        axes[count][1] = az / l;
        ++count;
    }
    float enter = -1e30f, exit = 1e30f;
    for (int k = 0; k < count; ++k) {
        float ux = axes[k][0], uz = axes[k][1];
        float c = cx * ux + cz * uz;
        float d = dx * ux + dz * uz;
        if (c < 0 || (c == 0 && d > 0)) { ux = -ux; uz = -uz; c = -c; d = -d; }
        const float ext = HsBoxSupport(b, ux, uz) + 0.5f * std::fabs(abx * ux + abz * uz);
        float tIn, tOut;
        if (std::fabs(d) < 1e-6f) {
            if (c >= ext) return false;
            continue;
        }
        if (d < 0) {
            tIn = (c - ext) / -d;
            tOut = (c + ext) / -d;
        } else {
            if (c >= ext) return false;
            tIn = -1e30f;
            tOut = (ext - c) / d;
        }
        // Prefer the wall normal on near-ties so flat walls push straight out.
        if (tIn > enter + (k == 0 ? 0.0f : 1e-4f)) {
            enter = tIn;
            nx = ux;
            nz = uz;
            gap = c - ext;
            if (axis) *axis = k;
        }
        exit = std::min(exit, tOut);
    }
    if (enter < 0 || enter > 1 || enter > exit) return false;
    t = enter;
    gap = std::max(0.0f, gap);
    return true;
}

inline bool HsWallUsable(const HsWallSeg& s, float y) {
    if (!std::isfinite(s.ax) || !std::isfinite(s.bx) || !std::isfinite(s.h)) return false;
    if (s.h < 0.3f || s.h > 40.0f) return false;
    if (!HsWallYHits(s, y)) return false;
    const float abx = s.bx - s.ax, abz = s.bz - s.az;
    return abx * abx + abz * abz >= 1e-6f;
}

// The nearest wall the box touches or approaches: its push normal and the
// signed separation (< 0 = penetration). Used to hand the contact to the
// game's own wall response before the position is corrected.
struct HsWallContact {
    bool valid = false;
    float nx = 0, nz = 0, sep = 0;
    float t = 0;    // HsWallSweep: fraction of the step at the contact
};

// HS19: the first wall the box meets while moving vel * dt (XZ), or invalid.
inline HsWallContact HsWallSweep(const float* pos, const float* vel,
        const HsWallSeg* segs, int n, const HsCarBox& box, float dt) {
    HsWallContact out;
    if (!segs || n <= 0 || n > 4096 || !(dt > 0)) return out;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(pos[i]) || !std::isfinite(vel[i])) return out;
    const HsCarBox b = HsCarBoxValid(box) ? box : HsCarBoxCircle(0.95f);
    const float dx = vel[0] * dt, dz = vel[2] * dt;
    for (int i = 0; i < n; ++i) {
        if (!HsWallUsable(segs[i], pos[1])) continue;
        float t = 0, hx = 0, hz = 0, gap = 0;
        int axis = 0;
        if (!HsBoxSegSweep(b, pos, dx, dz, segs[i], t, hx, hz, gap, &axis)) continue;
        if (axis != 0) {
            const float q[3] = {pos[0] + dx * t, pos[1], pos[2] + dz * t};
            if (HsWallSharedEnd(segs, n, i, q)) {
                gap = std::max(0.0f, HsWallNormalSeparation(b, pos, segs[i], hx, hz));
                if (vel[0] * hx + vel[2] * hz >= 0) continue;   // sliding along it
            }
        }
        if (!out.valid || t < out.t) {
            out.valid = true;
            out.t = t;
            out.nx = hx;
            out.nz = hz;
            out.sep = gap;
        }
    }
    return out;
}

inline HsWallContact HsWallNearest(const float* pos, const HsWallSeg* segs, int n,
        const HsCarBox& box, float within) {
    HsWallContact out;
    if (!segs || n <= 0 || n > 4096) return out;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(pos[i])) return out;
    const HsCarBox b = HsCarBoxValid(box) ? box : HsCarBoxCircle(0.95f);
    for (int i = 0; i < n; ++i) {
        const HsWallSeg& s = segs[i];
        if (!std::isfinite(s.ax) || !std::isfinite(s.bx) || !std::isfinite(s.h)) continue;
        if (s.h < 0.3f || s.h > 40.0f) continue;
        if (!HsWallYHits(s, pos[1])) continue;
        const float abx = s.bx - s.ax, abz = s.bz - s.az;
        if (abx * abx + abz * abz < 1e-6f) continue;
        float hx = 0, hz = 0;
        int axis = 0;
        float sep = HsBoxSegSeparation(b, pos, s, hx, hz, &axis);
        if (sep > within) continue;
        if (sep < 0 && axis != 0 && HsWallSharedEnd(segs, n, i, pos))
            sep = std::min(0.0f, HsWallNormalSeparation(b, pos, s, hx, hz));
        if (!out.valid || sep < out.sep) {
            out.valid = true;
            out.sep = sep;
            out.nx = hx;
            out.nz = hz;
        }
    }
    return out;
}

// Box corner deepest along -n (n horizontal): the point that meets the wall.
inline void HsBoxSupportPoint(const HsCarBox& b, const float* pos,
        float nx, float nz, float* outPoint) {
    for (int k = 0; k < 3; ++k) outPoint[k] = pos[k];
    for (int i = 0; i < 3; ++i) {
        const float d = b.axis[i][0] * nx + b.axis[i][2] * nz;
        if (std::fabs(d) < 1e-4f) continue;
        const float sgn = d > 0 ? -1.0f : 1.0f;
        for (int k = 0; k < 3; ++k) outPoint[k] += b.axis[i][k] * b.half[i] * sgn;
    }
}

inline HsCorridorResult HsWallConstrain(float* pos, float* vel,
        const HsWallSeg* segs, int n, const HsCarBox& box, float lookahead) {
    HsCorridorResult out;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(pos[i]) || !std::isfinite(vel[i])) return out;
    if (!segs || n < 0 || n > 4096) return out;
    const HsCarBox b = HsCarBoxValid(box) ? box : HsCarBoxCircle(0.95f);
    out.valid = true;
    for (int pass = 0; pass < 4; ++pass) {
        float bestPen = 0, nx = 0, nz = 0;
        bool have = false;
        for (int i = 0; i < n; ++i) {
            const HsWallSeg& s = segs[i];
            if (!HsWallUsable(s, pos[1])) continue;
            float hx = 0, hz = 0;
            int axis = 0;
            float pen = -HsBoxSegSeparation(b, pos, s, hx, hz, &axis);
            if (pen > 0 && axis != 0 && HsWallSharedEnd(segs, n, i, pos))
                pen = std::max(0.0f, -HsWallNormalSeparation(b, pos, s, hx, hz));
            if (pen > 0 && (!have || pen > bestPen)) {
                bestPen = pen;
                nx = hx;
                nz = hz;
                have = true;
            }
        }
        if (!have) {
            // Not inside any wall: limit the step to the first contact of
            // the swept box (HS19), not to the axis of largest separation.
            if (lookahead > 0) {
                const HsWallContact c = HsWallSweep(pos, vel, segs, n, b, lookahead);
                if (c.valid) {
                    const float vn = vel[0] * c.nx + vel[2] * c.nz;
                    const float allowed = std::max(vn, -c.sep / lookahead);
                    if (allowed != vn) {
                        out.removedVelocity += vn - allowed;
                        vel[0] -= c.nx * (vn - allowed);
                        vel[2] -= c.nz * (vn - allowed);
                    }
                }
            }
            break;
        }
        pos[0] += nx * bestPen;
        pos[2] += nz * bestPen;
        out.shift += bestPen;
        const float vn = vel[0] * nx + vel[2] * nz;
        if (vn < 0) {
            out.removedVelocity += vn;
            vel[0] -= nx * vn;
            vel[2] -= nz * vn;
        }
    }
    return out;
}
