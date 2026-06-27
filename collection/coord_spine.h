/*
 * coord_spine.h — Unified Constants for Bermuda/TW/POGLS
 * ═══════════════════════════════════════════════════════════════
 *
 * Purpose: Single source of truth for shared geometry constants.
 *   All systems include this — never redefine.
 *
 * Number chain (verified):
 *   720 = 12 × 60         (TRing base cycle)
 *   1440 = 720 × 2        (pentagon sync point)
 *   3456 = 6 × 576        (spokes × slots)
 *   6912 = 3456 × 2       (live + residual)
 *   20736 = 144 × 144     (full Y-triangle space)
 *   207360 = 20736 × 10   (TW fixed-point scale)
 *
 * Relationships:
 *   GEO_FULL = 6 × GEO_FULL_N = 6 × 3456 = 20736
 *   GEO_FULL_N = 6 × GEO_SLOTS = 6 × 576 = 3456
 *   GEO_SLOTS = 24² = 576
 *   TRING_TOTAL = 2 × GEO_FULL_N = 6912
 *   TRING_CYCLE = 720 (base) or 1440 (pentagon sync)
 *   TW_SCALE = GEO_FULL × 10 = 20736 × 10 = 207360
 *
 * Integration:
 *   #include "coord_spine.h"  // before bermuda_export.h, tw_capture_int.h, etc.
 *
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef COORD_SPINE_H
#define COORD_SPINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════
   CORE GEOMETRY CONSTANTS
   ═══════════════════════════════════════════════════════════════ */

/* TRing (Triwheel Ring) — base angular cycle */
#define TRING_CYCLE             720u    /* 12 × 60 = base cycle           */
#define TRING_PENTAGON_SYNC    1440u    /* 720 × 2 = pentagon sync point  */
#define TRING_TOTAL             6912u    /* 2 × 3456 = live + residual     */
#define TRING_LIVE_N            3456u    /* = GEO_FULL_N                   */
#define TRING_RESIDUAL_N        3456u    /* mirror zone                    */
#define TRING_COMPOUNDS         144u     /* fibo clock period              */
#define TRING_SPOKES              6u     /* dodeca faces / spokes          */
#define TRING_LEVELS              4u     /* slots per spoke                */
#define TRING_STRIDE             37u     /* prime, coprime to 720          */
#define TRING_PENT_SPAN          60u     /* positions per pentagon: 720/12 */
#define TRING_MIRROR_HALF        30u     /* polarity boundary              */

/* GEO (Geometry) — cylinder and Y-triangle space */
#define GEO_SPOKES               6u     /* 6 LADOS, 60° each              */
#define GEO_FACES_ICOS           9u     /* 8 outer + 1 center (icosphere) */
#define GEO_FACES_DODECA        12u     /* dodecahedron pentagon gates    */
#define GEO_FACE_UNITS          64u     /* units per face = 8²            */
#define GEO_SLOTS              576u     /* 9 × 64 = 24² per spoke        */
#define GEO_FULL_N            3456u     /* 576 × 6 = 144 × 24            */
#define GEO_FULL             20736u     /* 3456 × 6 = 144²               */
#define GEO_OUTER_SLOTS        512u     /* legacy 8-face (compat)         */
#define GEO_CENTER_BASE        512u     /* center face start              */
#define GEO_SIDE_FULL           54u     /* 6 × 9 sacred                  */

/* Block / Hilbert */
#define GEO_HILBERT_N          576u     /* = GEO_SLOTS                    */
#define GEO_BLOCK_BOUNDARY     288u     /* 576 / 2 = 2 × 144             */
#define GEO_GROUP_SIZE           8u     /* lines per audit group          */
#define GEO_BUNDLE_WORDS        8u     /* words per bundle               */

/* ThirdEye */
#define GEO_TE_CYCLE           144u     /* ops per snapshot               */
#define GEO_TE_FULL_CYCLES      24u     /* 144 × 24 = 3456               */
#define GEO_TE_SNAPS             6u     /* ring depth = GEO_SPOKES        */

/* QRPN thresholds */
#define GEO_HOT_THRESH          64u     /* 8² = 1 face                    */
#define GEO_IMBAL_THRESH       144u     /* TE_CYCLE = single-spoke seed   */
#define GEO_ANOMALY_HOT         96u     /* 576 / 6 = 1/6 spoke           */

/* ═══════════════════════════════════════════════════════════════
   POGLS CONSTANTS
   ═══════════════════════════════════════════════════════════════ */

/* PHI constants (frozen V3.4/V3.5) */
#define PHI_SCALE            1048576u   /* 2²⁰                            */
#define PHI_UP               1696631u   /* floor(φ × 2²⁰)                 */
#define PHI_DOWN              648055u   /* floor(φ⁻¹ × 2²⁰)              */

/* Icosphere */
#define NODE_MAX               162u     /* Icosphere L2                   */
#define FACES_RAW               32u     /* 5-bit FACE_ID                  */
#define FACES_LOGICAL          256u     /* after 2 folds                  */

/* Diamond Block */
#define DIAMOND_BLOCK_SIZE      64u     /* 1 CPU cache line — FROZEN      */
#define CORE_SLOT_SIZE           8u     /* 6B data + 2B reserved          */
#define INVERT_SIZE              8u     /* NOT(Core Slot)                 */
#define ACTIVE_SIZE             16u     /* Core + Invert                  */
#define QUAD_MIRROR_SIZE        32u     /* 4 × rotated Core (AVX2)       */
#define FOLD_SIZE               48u     /* Quad Mirror + Fold3 reserved   */

/* World / Twin */
#define TWIN_INVERT_MASK       0x40u    /* flip bit6 of ENGINE_ID         */
#define WORLD_A_BIT            0x00u    /* bit6 = 0 → 2^n binary          */
#define WORLD_B_BIT            0x40u    /* bit6 = 1 → 3^n ternary         */

/* ═══════════════════════════════════════════════════════════════
   BERMUDA CONSTANTS
   ═══════════════════════════════════════════════════════════════ */

#define BERMUDA_STRIDE           37u    /* prime, coprime to 720          */
#define BERMUDA_N_ZONES          12u    /* 12 zones (dodecahedron)        */
#define BERMUDA_TRING_SLOTS     720u    /* = TRING_CYCLE                  */

/* Gear table: 128×n slots, 2^k aligned */
#define BERMUDA_GEAR1_SLOTS     512u
#define BERMUDA_GEAR2_SLOTS    1024u
#define BERMUDA_GEAR3_SLOTS    2048u
#define BERMUDA_GEAR4_SLOTS    4096u

/* Shadow ring (frozen: 2⁴×3²) */
#define BERMUDA_SHADOW_RING     144u    /* ring capacity                  */
#define BERMUDA_SHADOW_HOT        0u    /* structured → ROUTE             */
#define BERMUDA_SHADOW_COLD       1u    /* float/dense → GROUND           */
#define BERMUDA_CHUNK            64u    /* DiamondBlock size              */

/* Shadow thresholds */
#define BERMUDA_COLD_RANGE_THR   64u    /* byte range ≥ 64 → candidate   */
#define BERMUDA_COLD_TRANS_THR   96u    /* transitions ≥ 96 → COLD       */

/* ═══════════════════════════════════════════════════════════════
   TW (Triwheel) CONSTANTS
   ═══════════════════════════════════════════════════════════════ */

#define TW_SCALE             207360u    /* 12⁴ × 10 = GEO_FULL × 10     */
#define TW_N_SECTORS             10u    /* 10 sectors (pentagon-pair)     */
#define TW_SLOTS_PER              6u    /* 6 child slots per sector       */
#define TW_N_SLOTS               60u    /* 10 × 6 = 60 total             */
#define TW_COMBINED_SLOTS        12u    /* hex(0..5) + tri(6..11)        */

/* Margin band (permille of |v|) */
#define TW_MARGIN_NUM             9u    /* ~0.5deg ~ 8.7e-3 rad          */
#define TW_MARGIN_DEN          1000u    /* sin(0.5deg) ≈ 9/1000          */

/* ═══════════════════════════════════════════════════════════════
   SHARED DERIVED CONSTANTS
   ═══════════════════════════════════════════════════════════════ */

/* TRing derived */
#define TRING_WALK_SPOKE_SZ   (TRING_CYCLE / TRING_SPOKES)   /* 120    */
#define TRING_WALK_SPOKES     TRING_SPOKES                    /* 6      */
#define TRING_COMP(enc)       ((enc) / TRING_PENT_SPAN)       /* /60    */
#define TRING_CPAIR(enc)      (((enc) + 360u) % TRING_CYCLE) /* +360%720 */

/* GEO derived */
#define GEO_WRAP(x)           ((uint32_t)(x) % GEO_FULL)      /* mod 20736 */
#define GEO_PENTAGONS         GEO_FACES_DODECA                /* 12     */

/* TW derived */
#define TW_REWIND_SLOTS       GEO_FULL                        /* 20736  */

/* Shadow zones — sector 10, 11 of dodecahedron (2/12 = 1/6 space) */
#define SHADOW_ZONE_A           10u
#define SHADOW_ZONE_B           11u
#define SHADOW_ZONES             2u
#define SHADOW_N_SLOTS          (GEO_FULL / GEO_PENTAGONS)
#define SHADOW_BUFFER_SIZE      (SHADOW_N_SLOTS * DIAMOND_BLOCK_SIZE)
#define SHADOW_NODE_BASE(zone)  ((uint32_t)(zone) * SHADOW_N_SLOTS)

/* Runtime base calculation */
#define SHADOW_ZONE_A_BASE      ((uint32_t)SHADOW_ZONE_A * SHADOW_N_SLOTS)
#define SHADOW_ZONE_B_BASE      ((uint32_t)SHADOW_ZONE_B * SHADOW_N_SLOTS)

/* ═══════════════════════════════════════════════════════════════
   COMPILE-TIME VERIFICATION
   ═══════════════════════════════════════════════════════════════ */

#if (GEO_SPOKES * GEO_SLOTS != GEO_FULL_N)
#  error "COORD_SPINE: GEO_SPOKES * GEO_SLOTS != GEO_FULL_N"
#endif
#if (GEO_TE_CYCLE * GEO_TE_FULL_CYCLES != GEO_FULL_N)
#  error "COORD_SPINE: GEO_TE_CYCLE * GEO_TE_FULL_CYCLES != GEO_FULL_N"
#endif
#if (GEO_FACES_ICOS * GEO_FACE_UNITS != GEO_SLOTS)
#  error "COORD_SPINE: GEO_FACES_ICOS * GEO_FACE_UNITS != GEO_SLOTS"
#endif
#if (GEO_BLOCK_BOUNDARY * 2 != GEO_SLOTS)
#  error "COORD_SPINE: GEO_BLOCK_BOUNDARY * 2 != GEO_SLOTS"
#endif
#if (TRING_CYCLE * 2 != TRING_PENTAGON_SYNC)
#  error "COORD_SPINE: TRING_CYCLE * 2 != TRING_PENTAGON_SYNC"
#endif
#if (GEO_FULL_N * 2 != TRING_TOTAL)
#  error "COORD_SPINE: GEO_FULL_N * 2 != TRING_TOTAL"
#endif
#if (TW_SCALE != GEO_FULL * 10)
#  error "COORD_SPINE: TW_SCALE != GEO_FULL * 10"
#endif
#if (BERMUDA_TRING_SLOTS != TRING_CYCLE)
#  error "COORD_SPINE: BERMUDA_TRING_SLOTS != TRING_CYCLE"
#endif
#if (20736 / 12 != 1728)
#  error "COORD_SPINE: GEO_FULL/12 should be 1728"
#endif
#if (11 * 1728 + 1728 > 20736)
#  error "COORD_SPINE: shadow zones overflow GEO_FULL"
#endif
#if (SHADOW_BUFFER_SIZE != 110592)
#  error "COORD_SPINE: SHADOW_BUFFER_SIZE should be 110592"
#endif

#ifdef __cplusplus
}
#endif

#endif /* COORD_SPINE_H */
