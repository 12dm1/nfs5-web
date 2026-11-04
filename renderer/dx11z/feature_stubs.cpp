// nfs5-web: the PC mod's feature modules, switched off (WEB2).
//
// The renderer core (dx11_device / draw / state / texture, copied from
// _mod/wrapper/src) calls into the mod's features: materials, car paint and
// reflections, scene probe (cube capture), motion blur, car mirror, animated
// textures, the profiler. Those modules hook PORSCHE.EXE in place and read
// game memory at absolute addresses, so they come over one by one in step 2
// (feature porting) through the runtime's hook sites. Until then every entry
// answers "feature off": the core draws the stock game.
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <cstddef>

#include "perf.h"
#include "tex_anim.h"
#include "car_mirror.h"
#include "car_paint.h"
#include "car_shading.h"
#include "scene_probe.h"
#include "motion_blur.h"
#include "materials.h"
#include "car_reflection.h"

// ---- perf (PERF1) ----
bool     g_perfOn = false;
int      g_perfView = 0;
uint64_t g_perfApiTicks = 0;
uint64_t g_perfLightTicks = 0;
void Perf_Count(int, int, uint32_t) {}
void Perf_Global(int, uint32_t) {}
void Perf_AddCall(int, uint64_t) {}
void Perf_AddZone(int, uint64_t, uint64_t) {}
void Perf_AddView(int, int, uint64_t, uint64_t) {}
void Perf_GpuMark(int, bool) {}
void Perf_FrameEnd() {}

// ---- animated textures (HZ4) ----
void  TexAnim_OnFree(int) {}
void  TexAnim_OnUpload(int, const char*, int, int) {}
void* TexAnim_Substitute(void* handle) { return handle; }
float TexAnim_VOffset(int) { return 0.0f; }

// ---- car mirror ----
bool CarMirror_Pass() { return false; }
bool CarMirror_Enabled() { return false; }
bool CarMirror_Plane(float[3], float*) { return false; }
void CarMirror_CountDraw(int) {}
void CarMirror_AddRoadSample(const float[3], const float[3]) {}

// ---- car paint (CP1) ----
void CarPaint_OnFree(int) {}
void CarPaint_OnUpload(int, const void*, int, int, int, const char*) {}
void CarPaint_OnUpdateRect(int, int, int, int, int) {}
void CarPaint_OnResetAll() {}
void CarPaint_NoteCarDraw(int, int, bool, int, int) {}
ID3D11ShaderResourceView* CarPaint_SlotSrv(int) { return nullptr; }

// ---- car shading (CAR1) ----
void  CarShading_DecodeNormal(uint32_t, float n[3]) { n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f; }
float CarShading_SectionAmbient() { return 1.0f; }

// ---- scene probe (R1 cube capture) ----
bool SceneProbe_Active() { return false; }
bool SceneProbe_MainViewActive() { return true; }
bool SceneProbe_CubemapHdr() { return false; }
void SceneProbe_NoteCaptureDraw(bool) {}

// ---- motion blur (MB1) ----
bool  MotionBlur_ForegroundMark() { return false; }
float MotionBlur_DistanceToDepth(float d) { return d; }

// ---- materials (MAT) ----
bool Materials_CompileShaders() { return true; }
void Materials_ReleaseShaders() {}
bool Materials_ShouldShade() { return false; }
void Materials_BeginShade() {}
void Materials_EndShade() {}
bool Materials_LastShadeWasCar() { return false; }
const char* Materials_SlotName(int) { return ""; }
bool Materials_ViewProj(float[4]) { return false; }
bool Materials_ViewBasis(float[3], float[3], float[3], float[3]) { return false; }
int  Materials_DebugView() { return 0; }
ID3D11ShaderResourceView* Materials_HdAlbedoSrv(int) { return nullptr; }
int  Materials_OnTexUpdate(int, const uint32_t*, int, int, int) { return -1; }
uint32_t Materials_CurrentFrame() { return 0; }
int  Materials_CutoutOverride(int) { return 0; }
float Materials_BoundReflection() { return 0.0f; }
bool Materials_SmoothNormalsEnabled() { return false; }

// ---- log counters (owned by the profiler in the PC mod) ----
uint64_t g_logTicks = 0;
uint32_t g_logLines = 0;
