#pragma once
#include "thrash_api.h"

void Draw_Init();
// G4: Glide-style guard band (see dx11_draw.cpp).
void Draw_ConfigureGuardBand(bool enabled, float screens);
void Draw_GuardBandStats(int* tris, int* draws, bool reset);
// G10: clamp bilinear sampling of 2D frontend triangles to their own UV rect.
void Draw_ConfigureUiUvClamp(bool enabled);
// G12: gain applied to additive draws while an HDR cubemap face is captured.
void Draw_ConfigureEmissive(float gain, float threshold, float power);
// CAR2 fix 9: gain / threshold / power for opaque draws of the track and car
// capture phases (lamp panels); gain 1 = off.
void Draw_ConfigureEmissiveOpaque(float gain, float threshold, float power);
void Draw_ConfigureEmissiveOpaqueGates(bool unlitOnly, bool darkOnly);
void Draw_Shutdown();
void Draw_Tri(THRASH_Vertex* v0, THRASH_Vertex* v1, THRASH_Vertex* v2);
void Draw_Quad(THRASH_Vertex* v0, THRASH_Vertex* v1, THRASH_Vertex* v2, THRASH_Vertex* v3);
void Draw_TriMesh(int count, void* indices, THRASH_Vertex* vertices);
// CAR1: the next Draw_TriMesh carries octahedron-encoded camera-space
// normals in the vertices' specular dwords (car_shading.cpp); they go to
// rect.xyz (w = 1) and the dword is cleared. Reset after the draw.
void Draw_SetCarNormals(bool on);
// CAR1: whether the last Draw_TriMesh was shaded with a car material.
bool Draw_LastTriMeshCarMaterial();
// R2: same mesh, drawn with the reflection shaders. The UV of every vertex
// is the encoded camera-space normal; the cube SRV is bound at t1 for this
// draw only and the caller's pixel shader/texture binding is restored after.
struct ReflectConstants;
struct ID3D11ShaderResourceView;
// R3: maskUv = count*3 diffuse (u, v) pairs in corner order and the diffuse
// texture's SRV (bound at t2); the reflection is multiplied by its alpha.
// Null = no mask. CP1: paint = the page's paint map (t3); the reflection is
// also multiplied by 1 - paint (the paint shader reflected there already).
void Draw_TriMeshReflect(int count, void* indices, THRASH_Vertex* vertices,
                         const ReflectConstants& constants, bool alphaBlend,
                         ID3D11ShaderResourceView* cube,
                         const float* maskUv = nullptr,
                         ID3D11ShaderResourceView* diffuse = nullptr,
                         ID3D11ShaderResourceView* paint = nullptr);
void Draw_TriFan(int count, void* indices, THRASH_Vertex* vertices);
void Draw_TriStrip(int count, void* indices, THRASH_Vertex* vertices);
void Draw_Line(THRASH_Vertex* v0, THRASH_Vertex* v1);
void Draw_Point(THRASH_Vertex* v0);
void Draw_Sprite(THRASH_Vertex* v0, THRASH_Vertex* v1);
