/*
 * fibo_shell_walk.h — FiboClock × Shell Traversal Connector
 * ═══════════════════════════════════════════════════════════════
 *
 * Wires the 144-tick FiboClock into:
 *   1. CUBE   — shell layer selection (onion walk)
 *   2. CYLINDER — sector angle (angular sweep)
 *   3. TRING  — enc 0..1439 ↔ clock phase 0..143 (×10 expansion)
 *
 * ── Clock → Geometry mapping ─────────────────────────────────
 *
 *   CUBE shell walk:
 *     tick 0..143 → shell_n = tick / FSW_TICKS_PER_SHELL
 *     FSW_TICKS_PER_SHELL = 144 / 8 = 18  (8 sacred shells in N=8 boundary)
 *     tick  0..17  → shell 0 (core)
 *     tick 18..35  → shell 1 (N mod 3 = 1 → sacred ✓)
 *     tick 36..53  → shell 2
 *     tick 54..71  → shell 3
 *     tick 72..89  → shell 4 (N mod 3 = 1 → sacred ✓)
 *     tick 90..107 → shell 5
 *     tick 108..125→ shell 6
 *     tick 126..143→ shell 7
 *     drain boundary (shell 8) reached at tick_end = 144 (full cycle close)
 *
 *   CYLINDER sector walk:
 *     tick % 4 = sector (0..3) — angular sweep, wraps every 4 ticks
 *     tick / 4 = angular_lap  (0..35) — full ring crossed 36 times per cycle
 *     Fibo alignment: 144 / 4 = 36 laps, 36 = 4 × 9 = digit_sum(36)=9 ✓
 *
 *   TRING enc expansion:
 *     clock_phase = enc / FSW_ENC_PER_PHASE   (= enc / 10)
 *     enc_in_phase = enc % FSW_ENC_PER_PHASE  (= enc % 10)
 *     1440 / 144 = 10 enc positions per clock tick
 *     → tick selects a band of 10 consecutive TRing positions
 *
 * ── Reverse mapping (geometry → clock) ──────────────────────
 *
 *   shell_n    → tick_base = shell_n * FSW_TICKS_PER_SHELL
 *   sector     → tick % 4 == sector  (use fsw_tick_for_sector)
 *   enc 0..1439→ clock_phase = enc / 10
 *
 * ── Spoke alignment (tring_walk ↔ clock) ─────────────────────
 *
 *   spoke = enc / 240        (6 spokes × 240 each)
 *   clock_phase = enc / 10   (144 phases × 10 each)
 *   spoke changes every 240 enc = 24 clock ticks
 *   → 6 spokes × 24 ticks = 144 ticks ✓ full clock cycle
 *
 * Sacred: FSW_CLOCK_CYCLE=144, FSW_ENC_CYCLE=1440 — FROZEN
 * No malloc. No float. No heap. Stateless O(1).
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef FIBO_SHELL_WALK_H
#define FIBO_SHELL_WALK_H

#include <stdint.h>
#include <stdbool.h>
#include "frustum_coord.h"    /* fc_cube_is_sacred_shell, fc_cube_shell_count */
#include "geo_tring_walk.h"   /* tring_walk_enc, TRING_WALK_CYCLE            */

/* ══════════════════════════════════════════════════════════════
 * FROZEN CONSTANTS
 * ══════════════════════════════════════════════════════════════ */

#define FSW_CLOCK_CYCLE       144u   /* FiboClock ticks per full cycle        */
#define FSW_ENC_CYCLE        1440u   /* TRing enc positions (= 10 × clock)    */
#define FSW_ENC_PER_PHASE      10u   /* enc positions per clock tick          */
#define FSW_SECTOR_COUNT        4u   /* cylinder sectors (0..3)               */
#define FSW_SHELL_COUNT         8u   /* shells 0..7 within drain boundary N=8 */
#define FSW_TICKS_PER_SHELL    18u   /* 144 / 8 = 18 ticks per shell layer    */
#define FSW_TICKS_PER_SPOKE    24u   /* 144 / 6 = 24 ticks per tring spoke    */

/* ══════════════════════════════════════════════════════════════
 * CLOCK → CUBE SHELL
 * ══════════════════════════════════════════════════════════════ */

/*
 * fsw_tick_to_shell: which cube shell layer does tick address?
 *   tick 0..143 → shell 0..7
 *   shell 8 (drain boundary) reached at tick == FSW_CLOCK_CYCLE (cycle end)
 */
static inline uint8_t fsw_tick_to_shell(uint8_t tick)
{
    return (uint8_t)((uint32_t)tick / FSW_TICKS_PER_SHELL);
}

/*
 * fsw_shell_tick_base: first tick that addresses shell_n
 *   shell_n = 0..7 → tick 0, 18, 36, ..., 126
 */
static inline uint8_t fsw_shell_tick_base(uint8_t shell_n)
{
    return (uint8_t)((uint32_t)shell_n * FSW_TICKS_PER_SHELL);
}

/*
 * fsw_tick_in_shell: position within shell layer (0..17)
 */
static inline uint8_t fsw_tick_in_shell(uint8_t tick)
{
    return (uint8_t)((uint32_t)tick % FSW_TICKS_PER_SHELL);
}

/*
 * fsw_is_shell_entry: true on first tick of each shell layer (tick_in_shell==0)
 */
static inline bool fsw_is_shell_entry(uint8_t tick)
{
    return (tick % FSW_TICKS_PER_SHELL) == 0u;
}

/*
 * fsw_shell_is_sacred: combines clock + sacred shell check
 *   true when tick enters a sacred shell (shell_n mod 3 == 1)
 */
static inline bool fsw_shell_is_sacred(uint8_t tick)
{
    return fc_cube_is_sacred_shell(fsw_tick_to_shell(tick));
}

/* ══════════════════════════════════════════════════════════════
 * CLOCK → CYLINDER SECTOR
 * ══════════════════════════════════════════════════════════════ */

/*
 * fsw_tick_to_sector: angular sector for cylinder traversal
 *   tick % 4 = sector (0..3)
 *   wraps every 4 ticks → 36 full sweeps per 144-tick cycle
 */
static inline uint8_t fsw_tick_to_sector(uint8_t tick)
{
    return (uint8_t)((uint32_t)tick % FSW_SECTOR_COUNT);
}

/*
 * fsw_angular_lap: how many full sector sweeps completed by tick
 *   tick / 4 = 0..35
 */
static inline uint8_t fsw_angular_lap(uint8_t tick)
{
    return (uint8_t)((uint32_t)tick / FSW_SECTOR_COUNT);
}

/*
 * fsw_tick_for_sector: first tick >= tick_base that lands on sector s
 *   returns tick_base + offset (0..3) to reach sector s
 *   used for scheduling: "next time we hit sector s after tick t"
 */
static inline uint8_t fsw_tick_for_sector(uint8_t tick_base, uint8_t sector)
{
    uint8_t cur = fsw_tick_to_sector(tick_base);
    uint8_t delta = (uint8_t)((sector + FSW_SECTOR_COUNT - cur) % FSW_SECTOR_COUNT);
    return (uint8_t)((tick_base + delta) % FSW_CLOCK_CYCLE);
}

/* ══════════════════════════════════════════════════════════════
 * CLOCK ↔ TRING ENC
 * ══════════════════════════════════════════════════════════════ */

/*
 * fsw_enc_to_phase: TRing enc → clock phase (0..143)
 *   enc / 10 = clock tick that enc falls in
 */
static inline uint8_t fsw_enc_to_phase(uint16_t enc)
{
    return (uint8_t)(enc / FSW_ENC_PER_PHASE);
}

/*
 * fsw_phase_to_enc_base: clock tick → first TRing enc in that phase
 *   tick * 10 = start of 10-enc band
 */
static inline uint16_t fsw_phase_to_enc_base(uint8_t tick)
{
    return (uint16_t)((uint32_t)tick * FSW_ENC_PER_PHASE);
}

/*
 * fsw_enc_in_phase: position within the 10-enc band (0..9)
 */
static inline uint8_t fsw_enc_in_phase(uint16_t enc)
{
    return (uint8_t)(enc % FSW_ENC_PER_PHASE);
}

/*
 * fsw_tile_to_phase: tile index i → clock phase via tring_walk_enc
 *   pipeline: tile_i → enc → phase
 */
static inline uint8_t fsw_tile_to_phase(uint32_t tile_i)
{
    return fsw_enc_to_phase(tring_walk_enc(tile_i));
}

/*
 * fsw_tile_to_shell: tile index i → cube shell via clock phase
 *   pipeline: tile_i → enc → phase → shell
 */
static inline uint8_t fsw_tile_to_shell(uint32_t tile_i)
{
    return fsw_tick_to_shell(fsw_tile_to_phase(tile_i));
}

/*
 * fsw_tile_to_sector: tile index i → cylinder sector via clock phase
 *   pipeline: tile_i → enc → phase → sector
 */
static inline uint8_t fsw_tile_to_sector(uint32_t tile_i)
{
    return fsw_tick_to_sector(fsw_tile_to_phase(tile_i));
}

/* ══════════════════════════════════════════════════════════════
 * SPOKE ↔ CLOCK ALIGNMENT
 * ══════════════════════════════════════════════════════════════ */

/*
 * fsw_tick_to_spoke: which TRing spoke does this tick address?
 *   tick / 24 = spoke (0..5)
 *   each spoke active for 24 consecutive ticks
 */
static inline uint8_t fsw_tick_to_spoke(uint8_t tick)
{
    return (uint8_t)((uint32_t)tick / FSW_TICKS_PER_SPOKE);
}

/*
 * fsw_spoke_tick_base: first tick of spoke s
 *   s * 24 = 0, 24, 48, 72, 96, 120
 */
static inline uint8_t fsw_spoke_tick_base(uint8_t spoke)
{
    return (uint8_t)((uint32_t)spoke * FSW_TICKS_PER_SPOKE);
}

/* ══════════════════════════════════════════════════════════════
 * FULL WALK DESCRIPTOR — one tick's complete geometry position
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  tick;          /* 0..143  FiboClock position               */
    uint8_t  shell_n;       /* 0..7    cube shell layer                  */
    uint8_t  tick_in_shell; /* 0..17   position within shell             */
    uint8_t  sector;        /* 0..3    cylinder angular sector           */
    uint8_t  angular_lap;   /* 0..35   full sector sweeps completed      */
    uint8_t  spoke;         /* 0..5    TRing spoke                       */
    uint16_t enc_base;      /* first TRing enc in this tick's phase band */
    bool     shell_entry;   /* true = first tick of a new shell layer    */
    bool     sacred;        /* true = entering a sacred shell (mod3=1)   */
} FswWalkPos;

static inline FswWalkPos fsw_walk_pos(uint8_t tick)
{
    FswWalkPos p;
    p.tick          = tick;
    p.shell_n       = fsw_tick_to_shell(tick);
    p.tick_in_shell = fsw_tick_in_shell(tick);
    p.sector        = fsw_tick_to_sector(tick);
    p.angular_lap   = fsw_angular_lap(tick);
    p.spoke         = fsw_tick_to_spoke(tick);
    p.enc_base      = fsw_phase_to_enc_base(tick);
    p.shell_entry   = fsw_is_shell_entry(tick);
    p.sacred        = fsw_shell_is_sacred(tick);
    return p;
}

/* ══════════════════════════════════════════════════════════════
 * VERIFY
 * ══════════════════════════════════════════════════════════════ */

static inline int fsw_verify(void)
{
    /* clock × enc alignment: 144 × 10 = 1440 */
    if (FSW_CLOCK_CYCLE * FSW_ENC_PER_PHASE != FSW_ENC_CYCLE) return -1;

    /* shell ticks: 8 shells × 18 = 144 */
    if (FSW_SHELL_COUNT * FSW_TICKS_PER_SHELL != FSW_CLOCK_CYCLE) return -2;

    /* spoke ticks: 6 spokes × 24 = 144 */
    if (6u * FSW_TICKS_PER_SPOKE != FSW_CLOCK_CYCLE) return -3;

    /* sector cycle: 4 sectors × 36 laps = 144 */
    if (FSW_SECTOR_COUNT * 36u != FSW_CLOCK_CYCLE) return -4;

    /* tick_to_shell: shell boundaries */
    if (fsw_tick_to_shell(0)   != 0u) return -5;
    if (fsw_tick_to_shell(17)  != 0u) return -6;  /* still shell 0 */
    if (fsw_tick_to_shell(18)  != 1u) return -7;  /* shell 1 entry */
    if (fsw_tick_to_shell(143) != 7u) return -8;  /* last shell */

    /* sacred shells at n mod 3 == 1: shell 1 and 4 */
    if (!fsw_shell_is_sacred(18))  return -9;   /* tick 18 → shell 1 sacred ✓ */
    if (!fsw_shell_is_sacred(72))  return -10;  /* tick 72 → shell 4 sacred ✓ */
    if ( fsw_shell_is_sacred(0))   return -11;  /* shell 0, not sacred */
    if ( fsw_shell_is_sacred(36))  return -12;  /* shell 2, not sacred */

    /* sector wrap: tick 3→sector 3, tick 4→sector 0 */
    if (fsw_tick_to_sector(3)  != 3u) return -13;
    if (fsw_tick_to_sector(4)  != 0u) return -14;
    if (fsw_tick_to_sector(143)!= 3u) return -15; /* 143 % 4 = 3 */

    /* enc ↔ phase roundtrip */
    if (fsw_enc_to_phase(0)    != 0u)  return -16;
    if (fsw_enc_to_phase(9)    != 0u)  return -17; /* band 0: enc 0..9 */
    if (fsw_enc_to_phase(10)   != 1u)  return -18; /* band 1 starts at 10 */
    if (fsw_enc_to_phase(1439) != 143u)return -19;
    if (fsw_phase_to_enc_base(0)  != 0u)   return -20;
    if (fsw_phase_to_enc_base(143)!= 1430u)return -21;

    /* spoke ↔ tick */
    if (fsw_tick_to_spoke(0)   != 0u) return -22;
    if (fsw_tick_to_spoke(23)  != 0u) return -23;
    if (fsw_tick_to_spoke(24)  != 1u) return -24;
    if (fsw_tick_to_spoke(143) != 5u) return -25;

    /* tick_for_sector: from tick 5 (sector 1) → next sector 3 = tick 7 */
    if (fsw_tick_for_sector(5, 3) != 7u) return -26;
    /* from tick 3 (sector 3) → next sector 0 = tick 4 */
    if (fsw_tick_for_sector(3, 0) != 4u) return -27;
    /* from tick 3 (sector 3) → next sector 3 = tick 3 (same) */
    if (fsw_tick_for_sector(3, 3) != 3u) return -28;

    /* FswWalkPos spot check at tick 18 (shell 1 entry, sacred) */
    FswWalkPos p = fsw_walk_pos(18);
    if (p.shell_n       != 1u)  return -29;
    if (p.tick_in_shell != 0u)  return -30;
    if (!p.shell_entry)         return -31;
    if (!p.sacred)              return -32;
    if (p.enc_base      != 180u)return -33; /* 18 * 10 = 180 */
    if (p.spoke         != 0u)  return -34; /* 18 / 24 = 0 */

    /* FswWalkPos at tick 72 (shell 4 sacred, spoke 3) */
    FswWalkPos p2 = fsw_walk_pos(72);
    if (p2.shell_n != 4u) return -35;
    if (!p2.sacred)       return -36;
    if (p2.spoke   != 3u) return -37; /* 72 / 24 = 3 */

    return 0;
}

#endif /* FIBO_SHELL_WALK_H */
