/*
 * fibo_tick.h — Unified Fibo Tick System
 * ═══════════════════════════════════════════════════════════════════
 *
 * Integrates three orthogonal views of the same 20736-slot field:
 *
 *   rdh_capture(data) → enc (2B) → frame_at(enc) → frame
 *
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │  View 1: frame_seek  (geom) — face/slot/phase/ico          │
 *   │  View 2: fibo_spine  (spine) — pipe/tick/bridge/residual   │
 *   │  View 3: p5h_ribcage (flower) — flower/phase/texture/barrier│
 *   └─────────────────────────────────────────────────────────────┘
 *
 * All three views address the SAME position in the field.
 * The tick (0..11) is the common synchronization axis:
 *
 *   tick   | frame_seek    | fibo_spine          | p5h_ribcage
 *   ───────┼───────────────┼─────────────────────┼────────────────────
 *   0      | phase 0       | normal cycle start  | BARRIER_SYNC
 *   1      | phase 1       | normal              | BARRIER_ENTER (new flower)
 *   2..10  | phase 2..10   | normal              | pipe room (10 phases)
 *   11     | phase 11      | JET BRIDGE trigger  | pipe room phase 10
 *   12     | (phase 0 next)| skipped (bridge)    | barrier next flower
 *
 * Storage routing:
 *   - ticks 0..10: store at container[face][slot][phase] (main storage)
 *   - tick 11:     Jet Bridge → residual_space (bond_key, timeless)
 *   - tick 0 barrier: freeze convergence point
 *
 * All header-only, static inline, no malloc in hot path.
 * ═══════════════════════════════════════════════════════════════════
 */

#ifndef FIBO_TICK_H
#define FIBO_TICK_H

#include <stdint.h>
#include <string.h>
#include <stddef.h>

/* ── The four orthogonal systems ──────────────────────────── */
#include "geo_frame_seek.h"         /* frame_at, stride-37 timeline       */
#include "rdh_capture.h"            /* rdh_capture — data → enc           */

/* P5H ribcage: barrier sync + pipe room + flower field.
 * Opt-in with -DP5H_ENABLE for the flower/barrier system.
 * Without P5H_ENABLE, all p5h_* macros expand to 0 / no-ops.
 * Include path: -Icollection → collection/include/p5h_ribcage.h
 */
#include "include/p5h_ribcage.h"

/* Fibo Spine: 1728 pipes × 12 ticks + Jet Bridge
 * Provides pipe timeline, tick progression, and Jet Bridge mechanics.
 * All header-only, static inline, no malloc in hot path.
 */
#include "fibo_spine.h"

/* ══════════════════════════════════════════════════════════════
   CONSTANTS — shared across all three views
   ══════════════════════════════════════════════════════════════ */

/* The full field = 144 × 144 = 20736 = every position */
#define FT_FIELD_W          144u
#define FT_FIELD_H          144u
#define FT_GEO_FULL         (FT_FIELD_W * FT_FIELD_H)  /* 20736 */

/* Frame cycle = stride-37 timeline on 1440 */
#define FT_FRAME_CYCLE      1440u    /* ≡ FRAME_CYCLE    */
#define FT_FRAME_STRIDE     37u      /* ≡ FRAME_STRIDE   */

/* Spine: 1728 pipes × 12 ticks = 20736 */
#define FT_PIPES            1728u    /* ≡ FS_PIPES       */
#define FT_TICKS_PER_CYCLE  12u      /* ≡ FS_TICKS/CYCLE */

/* Flower: 1728 flowers × 12 phases = 20736 */
#define FT_FLOWERS_FULL     1728u    /* ≡ P5H_FLOWERS_FULL */
#define FT_TICK_SPAN        12u      /* ≡ P5H_TICK_SPAN   */
#define FT_PHASE_SLOTS      10u      /* ≡ P5H_PHASE_SLOTS */

/* Barrier states (mirrors P5H_BARRIER_*) */
#define FT_BARRIER_NONE     0u
#define FT_BARRIER_ENTER    1u
#define FT_BARRIER_SYNC     2u

/* Jet Bridge tick */
#define FT_BRIDGE_TICK      11u

/* ══════════════════════════════════════════════════════════════
   MAPPING: enc → pipe_id + tick (spine view)
   ══════════════════════════════════════════════════════════════
 *
 * Each enc value (0..1439) maps to:
 *   pipe_id = enc  (0..1439) — 1440 primary pipes
 *   tick    = phase          (0..11) from frame_at
 *
 * The spine has 1728 pipes total (1728 = 12 × 144).
 * Pipes 0..1439 = primary timeline slots.
 * Pipes 1440..1727 = reserve / Jet Bridge residual expansion.
 */

/* enc → pipe_id (primary: pipe = enc) */
static inline uint16_t ft_enc_to_pipe(uint16_t enc)
{
    return enc % FT_PIPES;  /* enc 0..1439, FT_PIPES=1728 → always enc */
}

/* enc → tick (≡ frame_at(enc).phase) */
static inline uint8_t ft_enc_to_tick(uint16_t enc)
{
    return (uint8_t)((enc / FRAME_EDGES) % 12);
}

/* enc → (face, slot, ico_idx) — passthrough to frame_at */
static inline void ft_enc_to_frame(uint16_t enc,
                                    uint8_t *face,
                                    uint8_t *slot,
                                    uint8_t *ico_idx)
{
    DualFrame f = frame_at(enc);
    if (face)    *face    = f.face;
    if (slot)    *slot    = f.slot;
    if (ico_idx) *ico_idx = f.ico_idx;
}

/* ══════════════════════════════════════════════════════════════
   MAPPING: enc → flower_id + phase (p5h flower view)
   ══════════════════════════════════════════════════════════════
 *
 * Each tick position in the 20736-slot field maps to a flower:
 *   flower_id = (t * FRAME_STRIDE) % FT_FLOWERS_FULL
 *   phase_in_flower = tick % FT_TICK_SPAN
 *   texture = (phase_in_flower & 1) ? inner : outer
 *
 * Where t = enc (the time index) scaled by stride gives
 * a deterministic flower scatter across the full field.
 */

/* enc → flower_id (0..1727) — which flower */
static inline uint16_t ft_enc_to_flower(uint16_t enc)
{
    /* flower_id = (enc * FRAME_STRIDE) % FT_FLOWERS_FULL */
    return (uint16_t)(((uint32_t)enc * FRAME_STRIDE) % FT_FLOWERS_FULL);
}

/* enc → phase in flower (0..9 or 255 if barrier) */
static inline uint8_t ft_enc_to_phase_in_flower(uint16_t enc)
{
    uint8_t tick = ft_enc_to_tick(enc);
    if (tick == 0 || tick > FT_PHASE_SLOTS) return 255; /* barrier */
    return tick - 1;  /* 1→0, 2→1, ..., 10→9 */
}

/* enc → texture (inner/outer) */
static inline uint8_t ft_enc_to_texture(uint16_t enc)
{
    uint8_t phase = ft_enc_to_phase_in_flower(enc);
    if (phase > 9) return 0;
    return (phase & 1) ? P5H_TEX_INNER : P5H_TEX_OUTER;
}

/* ══════════════════════════════════════════════════════════════
   BARRIER DETECTION
   ══════════════════════════════════════════════════════════════ */

/* Is enc at a barrier boundary? (tick == 0) */
static inline int ft_is_barrier(uint16_t enc)
{
    return ft_enc_to_tick(enc) == 0;
}

/* Is enc at Jet Bridge trigger? (tick == 11) */
static inline int ft_is_bridge(uint16_t enc)
{
    return ft_enc_to_tick(enc) == FT_BRIDGE_TICK;
}

/* Is enc inside pipe room? (tick 2..11, i.e. not barrier/enter) */
static inline int ft_is_pipe_room(uint16_t enc)
{
    uint8_t t = ft_enc_to_tick(enc);
    return t >= 2 && t <= 11;
}

/* ══════════════════════════════════════════════════════════════
   TIMELINE NAVIGATION — extended from frame_seek
   ══════════════════════════════════════════════════════════════ */

/* next enc in stride-37 walk */
static inline uint16_t ft_next(uint16_t enc)
{
    return frame_next(enc);
}

/* prev enc in walk */
static inline uint16_t ft_prev(uint16_t enc)
{
    return frame_prev(enc);
}

/* enc at time t */
static inline uint16_t ft_enc(uint32_t t)
{
    return frame_enc(t);
}

/* ══════════════════════════════════════════════════════════════
   DATA ROUTING — based on tick position
   ══════════════════════════════════════════════════════════════
 *
 * ft_store_action() returns how data at this enc should be stored:
 *
 *   FT_STORE_MAIN    — store at container[face][slot][ico]  (ticks 0..10)
 *   FT_STORE_BRIDGE  — route to residual_space via bond_key (tick 11)
 *   FT_STORE_PIPE    — in pipe room, use inner/outer texture routing
 *   FT_STORE_FREEZE  — barrier sync point, data converges (tick 0)
 */

#define FT_STORE_MAIN    0u
#define FT_STORE_BRIDGE  1u
#define FT_STORE_PIPE    2u
#define FT_STORE_FREEZE  3u

static inline uint8_t ft_store_action(uint16_t enc)
{
    uint8_t t = ft_enc_to_tick(enc);
    if (t == 11) return FT_STORE_BRIDGE;   /* Jet Bridge */
    if (t == 0)  return FT_STORE_FREEZE;   /* barrier sync */
    if (t >= 2)  return FT_STORE_PIPE;     /* pipe room */
    return FT_STORE_MAIN;                  /* tick 1 — normal */
}

/* ══════════════════════════════════════════════════════════════
   ENC ←→ FIELD POSITION
   ══════════════════════════════════════════════════════════════
 *
 * Map between enc (0..1439, frame timeline) and
 * full field positions (ring, wedge) 0..143 each.
 * Not a bijection — enc covers the timeline, field covers space.
 * Use for spatial layout of the timeline positions.
 */

/* enc → (ring, wedge) on the 144×144 field */
static inline void ft_enc_to_field(uint16_t enc,
                                    uint16_t *ring,
                                    uint16_t *wedge)
{
    /* scatter timeline across field: ring = enc / 12, wedge = enc * stride % 144 */
    uint32_t t = enc;
    *ring   = (uint16_t)(t / 12);
    *wedge  = (uint16_t)((t * FRAME_STRIDE) % 144);
}

/* pipe_id + tick → linear slot index (0..20735) */
static inline uint32_t ft_slot_index(uint16_t pipe_id, uint8_t tick)
{
    return (uint32_t)pipe_id * FT_TICKS_PER_CYCLE + tick;
}

/* linear slot → (pipe_id, tick) */
static inline void ft_from_slot_index(uint32_t idx,
                                       uint16_t *pipe_id,
                                       uint8_t *tick)
{
    if (pipe_id) *pipe_id = (uint16_t)(idx / FT_TICKS_PER_CYCLE);
    if (tick)    *tick    = (uint8_t)(idx % FT_TICKS_PER_CYCLE);
}

/* ══════════════════════════════════════════════════════════════
   P5H FIELD SYNC — advance P5HField in sync with enc
   ══════════════════════════════════════════════════════════════
 *
 * Use p5h_field_observe() to sync a P5HField to the enc timeline.
 * Example:
 *   P5HField field;
 *   p5h_field_init(&field);
 *   for each enc:
 *     p5h_field_observe(&field, enc);
 *     if (p5h_is_barrier(&field)) { ... freeze sync ... }
 *     P5HFlower *fl = p5h_field_peek(&field, field.flower_now);
 *     // fl->phase, fl->texture determine storage method
 */

/* ══════════════════════════════════════════════════════════════
   VERIFY — call once at init, returns 0 on pass
   ══════════════════════════════════════════════════════════════
 *
 * Tests:
 *   T1: ft_enc_to_pipe — 0..1439 maps within FT_PIPES
 *   T2: ft_enc_to_tick — all 12 ticks appear
 *   T3: ft_enc_to_flower — 0..1727 range
 *   T4: barrier detection — tick 0 only
 *   T5: bridge detection — tick 11 only
 *   T6: store_action — all 4 actions used
 *   T7: timeline walk — stride-37 full cycle
 */

static inline int fibo_tick_verify(void)
{
    /* T1: pipe mapping — all enc map within FT_PIPES */
    for (uint16_t enc = 0; enc < FT_FRAME_CYCLE; enc++) {
        uint16_t pipe = ft_enc_to_pipe(enc);
        if (pipe >= FT_PIPES) return -1;
    }

    /* T2: tick distribution — 12 ticks × 120 each */
    uint32_t tick_counts[FT_TICKS_PER_CYCLE] = {0};
    for (uint16_t enc = 0; enc < FT_FRAME_CYCLE; enc++) {
        uint8_t t = ft_enc_to_tick(enc);
        if (t >= FT_TICKS_PER_CYCLE) return -2;
        tick_counts[t]++;
    }
    for (uint8_t t = 0; t < FT_TICKS_PER_CYCLE; t++) {
        if (tick_counts[t] != FT_FRAME_CYCLE / FT_TICKS_PER_CYCLE) return -3;
    }

    /* T3: flower range */
    uint32_t flower_used[FT_FLOWERS_FULL] = {0};
    for (uint16_t enc = 0; enc < FT_FRAME_CYCLE; enc++) {
        uint16_t fl = ft_enc_to_flower(enc);
        if (fl >= FT_FLOWERS_FULL) return -4;
        flower_used[fl]++;
    }

    /* T4: barrier detection — only tick 0 */
    for (uint16_t enc = 0; enc < FT_FRAME_CYCLE; enc++) {
        int is_bar = ft_is_barrier(enc);
        uint8_t t = ft_enc_to_tick(enc);
        if (is_bar && t != 0) return -5;
        if (t == 0 && !is_bar) return -6;
    }

    /* T5: bridge detection — only tick 11 */
    for (uint16_t enc = 0; enc < FT_FRAME_CYCLE; enc++) {
        int is_bridge = ft_is_bridge(enc);
        uint8_t t = ft_enc_to_tick(enc);
        if (is_bridge && t != 11) return -7;
        if (t == 11 && !is_bridge) return -8;
    }

    /* T6: all 4 store actions used */
    uint8_t actions_used[4] = {0};
    for (uint16_t enc = 0; enc < FT_FRAME_CYCLE; enc++) {
        uint8_t a = ft_store_action(enc);
        if (a > 3) return -9;
        actions_used[a] = 1;
    }
    for (int i = 0; i < 4; i++) {
        if (!actions_used[i]) return -10;
    }

    /* T7: stride-37 full cycle on 1440 */
    uint16_t visited[FT_FRAME_CYCLE] = {0};
    uint16_t e = 0;
    for (uint32_t i = 0; i < FT_FRAME_CYCLE; i++) {
        if (visited[e]) return -11;
        visited[e] = 1;
        e = ft_next(e);
    }
    if (e != 0) return -12;  /* must return to start */

    return 0;
}

#endif /* FIBO_TICK_H */
