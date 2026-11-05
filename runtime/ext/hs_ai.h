/* nfs5-web: racing AI on the converted tracks, runtime/ext/hs_ai.cpp. car = host pointer to the car object. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void  hs_ai_init(uint8_t *mem);
void  hs_ai_set_converted(int converted);
float hs_ai_speed_lookahead(void);
float hs_ai_zone(const uint8_t *car);
void  hs_ai_target(uint8_t *car, int idx);
void  hs_ai_guard(const uint8_t *car, float *target, const float *pos);
void  hs_ai_throttle(uint8_t *car);
int   hs_ai_stuck(uint8_t *car);
void  hs_ai_stuck_done(uint8_t *car);
#ifdef __cplusplus
}
#endif
