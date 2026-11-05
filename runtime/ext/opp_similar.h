/* nfs5-web: Quick Race opponents close to the player's car, runtime/ext/opp_similar.cpp */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { OPP_MODE_MIXED = 0, OPP_MODE_STOCK = 1, OPP_MODE_TUNED = 2, OPP_MODES_OURS = 3, OPP_MODE_COUNT = 8 };
typedef struct {
    uint8_t *mem;                       /* host address of guest address 0 */
    const uint8_t *player_rec, *owned_obj;      /* the player's car record; the career car object, or NULL */
    const uint32_t *list; int list_size;        /* candidate car records (guest addresses) */
    const uint8_t *part_table; int part_count;
    const char *sim_dir;
    int packages;
    void *ctx;
    void (*set_slot)(void *ctx, int slot, const uint8_t *rec, uint32_t ext, uint32_t stripe, uint32_t trim, uint32_t extra, int package, int number);
    void (*install)(void *ctx, int slot, const int16_t *ids, int count);
} OppGame;
int opp_similar_run(const OppGame *game, int mode);     /* 0: nothing done, use the game's generator */
#ifdef __cplusplus
}
#endif
