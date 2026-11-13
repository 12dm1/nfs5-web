/* CARS1: more cars per race.  The engine sizes its per-car tables for 9 cars (player + opponents + cops +
 * traffic, docs/findings/CARS1-car-limit.md).  These tables move to new guest memory with CARS_MAX slots:
 * tools/split_reloc.py reads the CARS_MOVES list below at lift time and turns every relocated operand that
 * points into an old table into the same offset in the new one, so the lifted code never touches the old place.
 *
 * The new tables live in the image's .reloc section (0x6C3000..0x6E3000, 128 KB): the game never reads its own
 * relocations and the loader (mem_load_pe) leaves that section zero.
 *
 * X(name, old address, old length, new address, new length): keep one move per line (the lifter parses them).
 */
#ifndef PU_CARS_H
#define PU_CARS_H

#define CARS_MAX        16          /* slots in every moved table */
#define CARS_REGION     0x006C3000u
#define CARS_REGION_END 0x006E3000u

#define CARS_MOVES(X) \
    X(INFO,     0x0065808Cu, 0x3180u, 0x006C3000u, 0x5800u) /* hand-off block, stride 0x580 */ \
    X(L_COPS,   0x005E8AC4u, 0x24u,   0x006C8800u, 0x40u)   /* cop list, count 5E8ABC */ \
    X(L_AI,     0x005E8AECu, 0x24u,   0x006C8840u, 0x40u)   /* AI-driven (racers, traffic, cops), count 5E8E28 */ \
    X(L_PURSUE, 0x005E8B10u, 0x24u,   0x006C8880u, 0x40u)   /* pursuable, count 5E8AB8 */ \
    X(L_CARS,   0x005E8B34u, 0x24u,   0x006C88C0u, 0x40u)   /* every car, count 5E8B58 */ \
    X(L_TRAFFIC,0x005E8B60u, 0x24u,   0x006C8900u, 0x40u)   /* traffic, count 5E8E24 */ \
    X(L_CARS2,  0x005E8B88u, 0x24u,   0x006C8940u, 0x40u)   /* every car (copy) */ \
    X(L_CARS3,  0x005E8BACu, 0x24u,   0x006C8980u, 0x40u)   /* every car (copy) */ \
    X(L_AIRACER,0x005E8BD4u, 0x24u,   0x006C89C0u, 0x40u)   /* AI racers, count 5E8BF8 */ \
    X(L_HUMAN,  0x005E8C00u, 0x24u,   0x006C8A00u, 0x40u)   /* humans, count 5E8BFC */ \
    X(L_RACER,  0x005E8E2Cu, 0x24u,   0x006C8A40u, 0x40u)   /* racers (humans + AI racers), count 5E8AE8 */ \
    X(ROSTER,   0x005E75A0u, 0x1518u, 0x006C8A80u, 0x2580u) /* per racer 0x258 bytes (race list built by 004EDED0/004B68F0) */ \
    X(HUD_PLACE,0x0060C9C8u, 0x20u,   0x006CB000u, 0x40u)   /* HUD per racer (00426A80, 00427660) */ \
    X(AI_HIGH,  0x005E6774u, 0x24u,   0x006CB040u, 0x40u)   /* AI high-level object per car (by car+0x520) */ \
    X(MAP_BLIP, 0x0060FDA8u, 0x24u,   0x006CB080u, 0x40u)   /* minimap blip clock per car (00433110) */ \
    X(FE_RACE,  0x00690AB0u, 0x24u,   0x006CB0C0u, 0x40u)   /* frontend race list per racer (004EDF00) */ \
    X(AI_POOL,  0x005E6E38u, 0x640u,  0x006CB100u, 0xF00u)  /* AI state objects, 0xA0 each: 10 -> 24 (allocator 0040B530) */ \
    X(AI_POOLF, 0x005E7484u, 0x28u,   0x006CC000u, 0x60u)   /* their in-use flags */ \
    X(FINISH,   0x00608B38u, 0x80u,   0x006CC060u, 0x100u)  /* finish order, 0x10 per racer (00416310, 00415FC0, 00416AC0) */ \
    X(ENG_LOAD, 0x006563C8u, 0x20u,   0x006CC160u, 0x80u)   /* engine sound load per car id (004AA050), 32 ids; cleared by 004A8660 */ \
    /* end */

/* instructions whose operands stay although they name an old table: the end of the table before it */
#define CARS_KEEP(K) \
    K(0x004068C4u) /* cmp eax, 5E75A0: the end of 5E7550[20] */ \
    K(0x004A8731u) K(0x004A8934u) K(0x004A8B2Du) K(0x004AB184u) K(0x004AB2E5u) K(0x004AB413u) K(0x004AB521u) \
        /* cmp reg, 6563C8/6563CC: loops over the sound channel table 656180[73] that ends there */ \
    /* end */

/* instructions whose operand equal to an old table's end is that table's end: it becomes the new table's end */
#define CARS_END(E) \
    E(0x0040B540u) E(0x0040B585u) E(0x0040B632u) E(0x0040CC83u) E(0x0040DB82u) E(0x0040DF90u) /* AI pool loops */ \
    /* end */

/* instructions that reach one table through the base of another (stock layout): F(ip, operand, table, delta)
 * makes the operand the moved table's new address + delta */
#define CARS_FIX(F) \
    /* engine sound, player channels 0x12/0x13: L_AI[0x12 + i] is L_CARS[i] (5E8AEC + 0x48 = 5E8B34) */ \
    F(0x004A9590u, 0x005E8AECu, L_CARS, -0x48) F(0x004A9740u, 0x005E8AECu, L_CARS, -0x48) \
    F(0x004A9810u, 0x005E8AECu, L_CARS, -0x48) \
    /* end */

#define CARS_DEF(n, o, ol, a, al) enum { CARS_##n = a, CARS_##n##_OLD = o, CARS_##n##_LEN = al };
CARS_MOVES(CARS_DEF)
#undef CARS_DEF

#define CARS_INFO_STRIDE 0x580u

/* the race setup block 6573E8 (0x3E24 bytes) ends with the old carInfo (9 slots at +0xCA4): it is cleared by
 * 004B4DA0/004B6A50 and copied to and from the replay buffer 5E9A20 as one block.  pu_cars_setup_hook clears the
 * moved carInfo with it and carries slots 0..8 in the replay buffer at their old offset, slots 9.. in CARS_INFO_X */
#define CARS_SETUP      0x006573E8u
#define CARS_SETUP_LEN  0x3E24u
#define CARS_REPLAY     0x005E9A20u
#define CARS_INFO_X     0x006CC200u     /* replay shadow of carInfo slots 9..CARS_MAX-1 */
#define CARS_INFO_OLDN  9u

/* counters that stay where they are */
#define CARS_N_CARS     0x005E8B58u
#define CARS_N_RACERS   0x005E8AE8u
#define CARS_NUMCARS    0x00658080u

#endif
