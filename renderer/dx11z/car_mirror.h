#pragma once
#include <cstdint>

// MAT14: cars mirrored in the wet road.
//
// One mirror plane for the whole scene (MAT13) cannot serve a mountain road:
// every crest and hairpin moves the reflection away from the car that casts
// it. Here every car gets its own mirror plane, fitted to the reflective road
// vertices around the car's position, and the car's draws are mirrored in
// that plane on the CPU: the game hands over screen-space vertices, they are
// unprojected with the main camera, reflected, clipped at the near plane and
// projected again. This happens in an extra car pass right after the main
// view has begun (scene_probe), into a screen-aligned layer that is black
// with alpha 0 where no car is. A road pixel then finds the reflection of the
// cars at its own screen position; the environment keeps the R1 cubemap.

void CarMirror_Configure(bool enabled, float radius, float gain);
// PERF2: cars farther from the camera than this get no reflection (and are
// not drawn in the mirror pass at all); 0 = no limit.
void CarMirror_ConfigureMaxDistance(float meters);
bool CarMirror_Enabled();
float CarMirror_Gain();

// World position and unit world normal of a welded vertex of a reflective
// material draw in the main view; one frame's list feeds the plane fits of
// the next frame (the track is static).
void CarMirror_AddRoadSample(const float world[3], const float normal[3]);

// The extra car pass. BeginPass swaps the sample lists and takes the camera
// from the material view cache (refreshed by the caller); false when the
// camera is unknown, then nothing is drawn.
bool CarMirror_BeginPass();
void CarMirror_EndPass();
bool CarMirror_Pass();

// From the deferred-tree draw hook (motion_blur.cpp), around one car's draws.
// `physics` = [car+F4] (position at +330), null for an entry that is not a
// car of the chain. True when a mirror plane was found; the draw path drops
// the car's draws otherwise.
bool CarMirror_BeginCar(const uint8_t* physics, bool player);
void CarMirror_EndCar();
// The current car's plane in camera space: dot(n, P) = d on the plane, n
// pointing up (toward the car). False when the draws must be dropped.
bool CarMirror_Plane(float n[3], float* d);
void CarMirror_CountDraw(int triangles);

struct CarMirrorStats {
    int   cars, mirrored, occluded, draws, tris, samples;
    int   distant;                        // PERF2: beyond planar_max_distance
    int   fitCount;                       // last fitted car: samples used
    float fitRms, fitNy, fitHeight;       // residual, normal.y, car above plane
};
void CarMirror_Stats(CarMirrorStats* out);
