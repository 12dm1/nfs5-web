/* nfs5-web: the walls of the converted High Stakes tracks, runtime/ext/hs_walls.cpp */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    void *ctx;
    void (*hit)(void *ctx, const float normal[3], const float point[3], const float push[3]);
    void (*rebuild)(void *ctx);
} HsWallGame;
int  hs_walls_load(const char *host_path);      /* segments, or < 0 */
void hs_walls_clear(void);
int  hs_walls_near(float x, float z, void *out, int cap);     /* HsWallSeg out[cap] */
void hs_walls_step(uint8_t *car, int before, const HsWallGame *game);
#ifdef __cplusplus
}
#endif
