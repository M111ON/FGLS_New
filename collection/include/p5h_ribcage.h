/*
 * p5h_ribcage.h — P5H Pipe Domain (Mario Pipe / Cookie Run Bonus Time)
 *
 * OPTIONAL — compile with -DP5H_ENABLE to activate.
 * When disabled: all macros expand to 0, functions are no-ops,
 * structs have zero size. Zero cost when not opted in.
 *
 * Think of it like Mario jumping down a pipe, or Cookie Run bonus time:
 *
 *   ───────────── main level (ticks 1-11) ─────────────▶
 *      │                                      │
 *      │  PIPE ROOM (bonus domain)            │  PIPE ROOM
 *      │  ticks 1-10: 10-phase window         │  ticks 13-22: ...
 *      │  - cross-pipe peek/borrow            │
 *      │  - normally unreachable values       │
 *      │  tick 12: EXIT PIPE -> back to main  │  tick 24: EXIT
 *      ▼                                      ▼
 *
 * Main level = normal FiboClock routing.
 * Pipe room = opt-in - jump in at any pipe (every 12th tick),
 * spend up to 10 phases cross-pollinating with other pipes,
 * then emerge at the barrier with merged state.
 *
 * Skip the pipe? No problem - just run past it.
 *
 * Geometry:
 *   1728 pipes x 12 ticks = 20736 = GEO_FULL
 *   120  pipes x 12 ticks = 1440  = FiboClock cycle
 *   12   pipes x 12 ticks = 144   = tower / FLUSH boundary
 *
 * Inside each pipe (10 phases):
 *   phase 0,2,4,6,8 = outer pentagon edge (outward routing)
 *   phase 1,3,5,7,9 = inner pentagon edge (inward routing)
 */
#ifndef P5H_RIBCAGE_H
#define P5H_RIBCAGE_H

#include <stdint.h>
#include <string.h>

/* -- Opt-in guard -------------------------------------------------- */
#ifdef P5H_ENABLE
#define P5H_ACTIVE 1

/* -- Spacing ------------------------------------------------------- */
#define P5H_TICK_SPAN       12u
#define P5H_FLOWERS_FULL   1728u
#define P5H_FLOWERS_CYCLE   120u
#define P5H_FLOWERS_TOWER    12u
#define P5H_FLOWERS_BLOCK     4u

/* -- Per-flower phases -------------------------------------------- */
#define P5H_PHASE_SLOTS     10u
#define P5H_PETALS           5u

/* Phase texture: even=outer edge, odd=inner edge (+36 deg rotation) */
#define P5H_TEX_OUTER       0u
#define P5H_TEX_INNER       1u

/* -- Barrier sync flags ------------------------------------------- */
#define P5H_BARRIER_NONE    0u
#define P5H_BARRIER_ENTER   1u
#define P5H_BARRIER_SYNC    2u

/* -- One flower ---------------------------------------------------- */
typedef struct {
    uint16_t branch_id;
    uint8_t  phase;
    uint8_t  texture;
    uint8_t  resolved;
    uint8_t  pad[3];
} P5HFlower;

/* -- Field of 1728 flowers --------------------------------------- */
typedef struct {
    P5HFlower flowers[P5H_FLOWERS_FULL];
    uint16_t  tick_now;
    uint16_t  flower_now;
    uint8_t   tick_in_flower;
    uint8_t   barrier_flag;
    uint8_t   pad[2];
} P5HField;

static inline void p5h_flower_init(P5HFlower *f)
{
    f->branch_id = 0;
    f->phase = 0;
    f->texture = P5H_TEX_OUTER;
    f->resolved = 0;
}

static inline void p5h_field_init(P5HField *f)
{
    for (uint32_t i = 0; i < P5H_FLOWERS_FULL; i++) {
        p5h_flower_init(&f->flowers[i]);
    }
    f->tick_now = 0;
    f->flower_now = 0;
    f->tick_in_flower = 0;
    f->barrier_flag = P5H_BARRIER_ENTER;
}

static inline uint8_t p5h_field_tick(P5HField *f)
{
    uint16_t t = ++f->tick_now % 20736;
    f->tick_now = t;

    f->tick_in_flower = t % P5H_TICK_SPAN;

    if (f->tick_in_flower == 0) {
        f->barrier_flag = P5H_BARRIER_SYNC;
        f->flower_now = (f->flower_now + 1) % P5H_FLOWERS_FULL;
    } else if (f->tick_in_flower == 1) {
        f->barrier_flag = P5H_BARRIER_ENTER;
        p5h_flower_init(&f->flowers[f->flower_now]);
        f->flowers[f->flower_now].phase = 0;
    } else {
        f->barrier_flag = P5H_BARRIER_NONE;
    }

    if (f->tick_in_flower >= 1 && f->tick_in_flower <= P5H_PHASE_SLOTS) {
        P5HFlower *fl = &f->flowers[f->flower_now];
        fl->phase = f->tick_in_flower - 1;
        fl->texture = (fl->phase & 1) ? P5H_TEX_INNER : P5H_TEX_OUTER;
    }

    return f->barrier_flag;
}

static inline int p5h_is_barrier(const P5HField *f)
{
    return f->barrier_flag == P5H_BARRIER_SYNC;
}

static inline int p5h_is_flower_start(const P5HField *f)
{
    return f->barrier_flag == P5H_BARRIER_ENTER;
}

static inline uint32_t p5h_flower_id(uint32_t node)
{
    return node / P5H_TICK_SPAN;
}

static inline uint8_t p5h_phase_at_tick(uint16_t tick)
{
    uint8_t t = tick % P5H_TICK_SPAN;
    if (t == 0 || t > P5H_PHASE_SLOTS) return 255;
    return t - 1;
}

static inline P5HFlower *p5h_field_peek(P5HField *f, uint32_t flower_id)
{
    if (flower_id >= P5H_FLOWERS_FULL) return (P5HFlower *)0;
    return &f->flowers[flower_id];
}

static inline void p5h_field_observe(P5HField *f, uint16_t external_tick)
{
    uint16_t t = external_tick % 20736;
    f->tick_now = t;

    f->tick_in_flower = t % P5H_TICK_SPAN;

    if (f->tick_in_flower == 0) {
        f->barrier_flag = P5H_BARRIER_SYNC;
        f->flower_now = (f->flower_now + 1) % P5H_FLOWERS_FULL;
    } else if (f->tick_in_flower == 1) {
        f->barrier_flag = P5H_BARRIER_ENTER;
        p5h_flower_init(&f->flowers[f->flower_now]);
        f->flowers[f->flower_now].phase = 0;
    } else {
        f->barrier_flag = P5H_BARRIER_NONE;
    }

    if (f->tick_in_flower >= 1 && f->tick_in_flower <= P5H_PHASE_SLOTS) {
        P5HFlower *fl = &f->flowers[f->flower_now];
        fl->phase = f->tick_in_flower - 1;
        fl->texture = (fl->phase & 1) ? P5H_TEX_INNER : P5H_TEX_OUTER;
    }
}

static inline uint8_t p5h_phase_to_vertex(uint8_t phase)
{
    uint8_t p = phase / 2;
    if (phase & 1) p += 5;
    return p;
}

#else /* P5H_ENABLE not defined - zero-cost stubs */

#define P5H_ACTIVE               0
#define P5H_TICK_SPAN            1
#define P5H_FLOWERS_FULL         0
#define P5H_FLOWERS_CYCLE        0
#define P5H_FLOWERS_TOWER        0
#define P5H_FLOWERS_BLOCK        0
#define P5H_PHASE_SLOTS          0
#define P5H_PETALS               0
#define P5H_TEX_OUTER            0
#define P5H_TEX_INNER            0
#define P5H_BARRIER_NONE         0
#define P5H_BARRIER_ENTER        0
#define P5H_BARRIER_SYNC         0

typedef int P5HFlower;
typedef int P5HField;

static inline void p5h_flower_init(P5HFlower *f)        { (void)f; }
static inline void p5h_field_init(P5HField *f)          { (void)f; }
static inline uint8_t p5h_field_tick(P5HField *f)       { (void)f; return 0; }
static inline void p5h_field_observe(P5HField *f, uint16_t t) { (void)f; (void)t; }
static inline int p5h_is_barrier(const P5HField *f)     { (void)f; return 0; }
static inline int p5h_is_flower_start(const P5HField *f){ (void)f; return 0; }
static inline uint32_t p5h_flower_id(uint32_t node)     { return node; }
static inline uint8_t p5h_phase_at_tick(uint16_t tick)   { (void)tick; return 255; }
static inline P5HFlower *p5h_field_peek(P5HField *f, uint32_t fid) { (void)f; (void)fid; return (P5HFlower *)0; }
static inline uint8_t p5h_phase_to_vertex(uint8_t phase) { (void)phase; return 0; }

#endif /* P5H_ENABLE */

#endif /* P5H_RIBCAGE_H */
