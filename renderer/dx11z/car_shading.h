#pragma once
// CAR1: the car's own smooth normals for the material shader.
//
// The game's R3DLightLayer (vtable 005B393C[0] -> 00446530) lights every car
// batch from the object normals (dependency 0 of the layer) before the diffuse
// drawtrimesh. Its hook here writes the camera-space normal of every output
// vertex, octahedron-encoded in 12+12 bits, into the vertex's `specular` dword
// (E+448 + j*stride), which the game leaves at zero for cars (CAR0: 0 of 20 M
// corners in a race). The following drawtrimesh of the EAGL array (E+434,
// E+10034) decodes it into the material shader's normal input (rect.xyz, w =
// 1, the MAT11 slot) and clears the dword for the ordinary shaders.
// The reflection-pass hook (car_reflection.cpp) asks CarShading_SkipReflect-
// Pass() to drop the game's second (sphere-map) pass when the diffuse pass
// was drawn with a car material, so the shading is not painted over.
#include <cstdint>

void CarShading_Configure(bool enabled);
bool CarShading_Enabled();
bool CarShading_Install();
bool CarShading_Installed();

// 24-bit encoding of a unit vector, one byte per component (clip-safe; the
// upper byte of the dword stays 0).
uint32_t CarShading_EncodeNormal(const float n[3]);
void     CarShading_DecodeNormal(uint32_t code, float n[3]);

// THRASH_drawtrimesh: true when this draw is the armed EAGL batch whose
// `specular` dwords carry normals. Clears the armed state either way.
bool CarShading_TakeNormals(const void* vertices, const void* indices);
// After a drawtrimesh of the EAGL array: whether it was shaded with a car
// material (so the game's reflection pass of the same batch is skipped).
void CarShading_NoteDraw(const void* vertices, bool carMaterial);
bool CarShading_SkipReflectPass(const void* vertices);

// CAR2: the world-space light direction the game's car light layer uses
// ([006115CC]+30, read in the hook); false until a car batch ran.
bool CarShading_LightDir(float out[3]);
// CAR2 fix 9: luminance of the game's section ambient for the car (S+18 row);
// -1 until a car batch ran. Dark sections = tunnels, night.
float CarShading_SectionAmbient();

// CAR2 diag ([materials] car_diag=1, live): every 60 frames one log line
// with the game's car light (direction, per-batch intensities, ambient /
// colour rows), a fit of the vertex light to ambient + direct * max(N.L, 0)
// for the player's car and for the others, the shader's sun, and the R1
// cube's texel in the sun direction (MAT16 sun share in alpha).
void CarShading_ConfigureDiag(bool on);
// MB2 flush-draw hook: the car whose light batches follow (inside = true
// before its draw, false after).
void CarShading_NoteCar(const void* obj, bool player, bool inside);
// Best signed axis permutation of v (world) that matches target: name is
// e.g. "+x+z-y" (= (v.x, v.z, -v.y)); with a camera basis the candidates
// are compared in camera space (z = -forward). Returns the angle in degrees.
float CarShading_BestSignedPerm(const float v[3], const float target[3],
                                const float* right, const float* up, const float* fwd, char name[16]);
void CarShading_OnPageflip(uint32_t frame);

// Diagnostics
uint32_t CarShading_Batches();
uint32_t CarShading_Decoded();
uint32_t CarShading_SkippedPasses();

// The probe (car_probe.cpp) chains behind this hook when both are enabled.
using CarLightApplyFn = uint32_t (__fastcall*)(uint8_t* layer, void* edx, uint8_t* streamA, uint8_t* streamB);
uint32_t __fastcall CarShading_HookLightApply(uint8_t* layer, void* edx, uint8_t* streamA, uint8_t* streamB);
