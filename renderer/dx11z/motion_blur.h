#pragma once
#include <cstdint>

// MB1: camera motion blur by depth reprojection.
//
// The race frame (004689C0) renders the main scene pass (00468BA0), then HUD
// part 1, the mirror and HUD part 2. The wrapper replaces the call at
// 00468A74 so that, once the main pass has returned, the finished 3D image is
// copied and blurred in place along each pixel's screen-space motion, before
// any HUD or mirror pixel exists. The motion comes from the depth buffer and
// the main view's current and previous camera (P6 contract: z = A - B/d;
// basis rows V+E8/+F4/+100, position V+10C, projection C+148/15C/168/16C,
// EAGL viewport E+2C/30/38/3C — the same readers as the R2 reflection pass).
// Nothing closer than the near mask distance (the player's car, the bonnet)
// is blurred, and such pixels never bleed into the scenery behind them.

struct MotionBlurConfig {
    bool  enabled       = false;
    float strength      = 0.6f;    // fraction of the per-frame motion that is blurred (shutter)
    float maxRadius     = 0.03f;   // longest blur, as a fraction of the GPU target height
    int   samples       = 12;      // taps along the velocity
    float nearStart     = 8.0f;    // world units: fully sharp closer than this
    float nearEnd       = 14.0f;   // fully blurred beyond this
    float jumpDistance  = 20.0f;   // camera cut: position change per frame that resets history
    float jumpAngleDeg  = 30.0f;   // camera cut: forward change per frame that resets history
    int   debugMode     = 0;       // 1 = draw the velocity field instead of the blur
    bool  carMask       = true;    // MB2: stencil-mark the player's car draws (sharp, no bleed)
};

void MotionBlur_Configure(const MotionBlurConfig& config);
const MotionBlurConfig& MotionBlur_Config();
bool MotionBlur_Enabled();

// Patch the main-pass call site after byte checks. Requires the camera hooks
// (executable identity). All-or-nothing.
bool MotionBlur_Install();
bool MotionBlur_Installed();

// The pass itself, on the current main render target. `view` is the game
// view that produced the scene (race renderer +4); the offscreen test drives
// it with explicit constants through MotionBlur_RunWithConstants.
void MotionBlur_AfterMainScene(const uint8_t* view);

struct ReflectConstants;
struct MotionBlurCamera {
    float right[3], up[3], forward[3], position[3];
    float proj[4];       // sX, sY, oX, oY
    float viewport[4];   // centerX, centerY, halfW, halfH (logical pixels)
};
// Fill a camera record from R2-style constants + position.
void MotionBlur_CameraFromConstants(const ReflectConstants& c, const float position[3],
                                    MotionBlurCamera* out);
// Blur the current target given both cameras (no history / jump logic).
bool MotionBlur_RunWithCameras(const MotionBlurCamera& current, const MotionBlurCamera& previous);
// CPU reference of the shader's reprojection: logical pixel (x, y) at depth z
// in `current` -> logical pixel in `previous`. Returns false when the point is
// behind the previous camera. Tests compare it with known geometry.
bool MotionBlur_Reproject(const MotionBlurCamera& current, const MotionBlurCamera& previous,
                          float x, float y, float z, float* outX, float* outY);
// Depth-buffer value <-> camera distance (P6: near 1, far 65536).
float MotionBlur_DepthToDistance(float z);
float MotionBlur_DistanceToDepth(float d);

// MB2: true while the player's car is being drawn (flush hook 00466728);
// the depth state then also writes stencil 1. Tests toggle it directly.
bool MotionBlur_ForegroundMark();
void MotionBlur_SetForegroundMark(bool on);
bool MotionBlur_CarMaskInstalled();
uint32_t MotionBlur_CarDraws();
// MAT14: install the deferred-tree draw hook on its own (the car mirror
// pass brackets each car's draws through it); idempotent.
bool MotionBlur_InstallCarDrawHook();

// Forget the previous camera (driver restore, race start).
void MotionBlur_ResetHistory();
// Release GPU resources (driver restore / shutdown).
void MotionBlur_ReleaseResources();
// Called from THRASH_pageflip: a frame without a main pass breaks the history.
void MotionBlur_OnPageflip(uint32_t frame);
// Counters for logs/tests.
uint32_t MotionBlur_Passes();
uint32_t MotionBlur_Skips();
