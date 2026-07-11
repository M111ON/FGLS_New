/*
 * geofield_hilbert.h — Dual-Curve: Hilbert + Peano Overlay
 * ══════════════════════════════════════════════════════════
 *
 * CONCEPT:
 *   Grid: 12×12 = 144 cells
 *   Hilbert path  = 9 real lines   (วิ่งใน 144 cells)
 *   Peano overlay = 3 invert lines (ครอบอีก layer, วิ่งตาม phase 3:1)
 *   Convergence   = cell 12 = 144/12 → "positive invert event"
 *
 *   ทุก 12 steps:
 *     Hilbert (9 lines) + Peano (3 lines) บรรจบ → สร้าง positive invert
 *     → เส้น positive invert = 12 เส้นรวม (9+3) ณ จุดบรรจบ
 *
 *   Peano ไม่ใช่ invert ของ Hilbert — มัน OVERLAY อีก layer
 *   วิ่งพร้อมกัน, phase ต่างกัน 3:1, lock กันที่ cell 12
 *
 * RATIO:
 *   Hilbert : Peano = 9 : 3 = 3:1
 *   9 lines = real path
 *   3 lines = invert overlay (Peano)
 *   12 total = GEOF_CONVERGENCE (pentagon count, GCD(60°,72°))
 *
 * INVERT MEANING (TODO: confirm with owner):
 *   Options:
 *     A) bit-flip of cell index
 *     B) spatial reflection across grid center
 *     C) curve orientation flip (U-shape mirror)
 *   → กระทบ hp_decode() ทั้งหมด ต้องตัดสินใจก่อน implement
 *
 * ADDRESS SPACE:
 *   144 cells × 12 convergence events = 1728 = 12³
 *   12⁴ = 20736 = GEOF_ALIGN_POINT = full address space
 *
 * Wang edge integration:
 *   Wang edge = switch on Hilbert path
 *   ทุก convergence point (cell 12, 24, 36, ...) = potential Wang switch
 *   → geo_wang_edge.h ใช้ hp_convergence_pos() เป็น switch trigger
 *
 * ══════════════════════════════════════════════════════════
 */
#pragma once
#include <stdint.h>
#include "geofield_header.h"   /* GEOF_CONVERGENCE=12, GEOF_HP_RATIO=3 */

/* ── Grid constants ─────────────────────────────────────────── */

#define HP_GRID_SIZE        12u    /* grid side = GEOF_CONVERGENCE       */
#define HP_CELLS           144u    /* 12×12 total cells                  */
#define HP_HILBERT_LINES     9u    /* real Hilbert path lines            */
#define HP_PEANO_LINES       3u    /* Peano overlay lines                */
#define HP_TOTAL_LINES      12u    /* = HP_HILBERT_LINES + HP_PEANO_LINES */
#define HP_CONV_PERIOD      12u    /* steps between convergence events   */
#define HP_CONV_COUNT       12u    /* 144/12 = convergence events/round  */

/* ── Cell coordinate ────────────────────────────────────────── */
typedef struct {
    uint8_t x;   /* 0..11 */
    uint8_t y;   /* 0..11 */
} HpCell;

/* ── Cursor: tracks both curves simultaneously ──────────────── */
typedef struct {
    uint32_t step;          /* 0..143 current step in 144-cell grid     */
    HpCell   hilbert_pos;   /* current Hilbert position                  */
    HpCell   peano_pos;     /* current Peano position (phase offset 3:1) */
    uint8_t  conv_count;    /* convergence events fired so far (0..11)  */
    uint8_t  at_conv;       /* 1 if current step is convergence point   */
} HpCursor;

/* ── TODO: Hilbert index → cell (d2xy) ─────────────────────── */
/*
 * hp_hilbert_cell — decode Hilbert index to (x,y) in 12×12 grid
 * Standard d2xy for n=12 — NOT power of 2, needs generalized version
 * Reference: Skilling (2004) or adaptive Hilbert for non-power-2 grids
 *
 * TODO: implement generalized Hilbert for n=12
 */
static inline HpCell hp_hilbert_cell(uint32_t idx) {
    (void)idx;
    HpCell c = {0, 0};
    /* TODO */
    return c;
}

/* ── TODO: Peano index → cell ───────────────────────────────── */
/*
 * hp_peano_cell — decode Peano index to (x,y) in 12×12 grid
 * Peano curve naturally fits 3ⁿ grids — 12 = 4×3, needs adjustment
 * Phase offset: peano_idx = hilbert_idx / HP_HP_RATIO (3:1)
 *
 * TODO: decide invert type (A/B/C) before implementing
 */
static inline HpCell hp_peano_cell(uint32_t hilbert_idx) {
    uint32_t peano_idx = hilbert_idx / GEOF_HP_RATIO;  /* 3:1 phase */
    (void)peano_idx;
    HpCell c = {0, 0};
    /* TODO: implement after invert type confirmed */
    return c;
}

/* ── Cursor step ────────────────────────────────────────────── */
/*
 * hp_cursor_init — reset cursor to step 0
 */
static inline void hp_cursor_init(HpCursor *cur) {
    cur->step        = 0;
    cur->hilbert_pos = hp_hilbert_cell(0);
    cur->peano_pos   = hp_peano_cell(0);
    cur->conv_count  = 0;
    cur->at_conv     = 0;
}

/*
 * hp_cursor_advance — move one step
 * Sets at_conv=1 when step % HP_CONV_PERIOD == 0 (convergence event)
 * Returns 0 when full 144-cell round complete (step wraps)
 */
static inline int hp_cursor_advance(HpCursor *cur) {
    cur->step++;
    if (cur->step >= HP_CELLS) {
        cur->step = 0;
        cur->conv_count = 0;
    }
    cur->hilbert_pos = hp_hilbert_cell(cur->step);
    cur->peano_pos   = hp_peano_cell(cur->step);
    cur->at_conv     = (cur->step % HP_CONV_PERIOD == 0) ? 1 : 0;
    if (cur->at_conv && cur->conv_count < HP_CONV_COUNT)
        cur->conv_count++;
    return (cur->step > 0) ? 1 : 0;
}

/* ── Convergence position query ─────────────────────────────── */
/*
 * hp_convergence_pos — return cell index of n-th convergence event
 * n = 0..11 → cell 0, 12, 24, ..., 132
 * Used by geo_wang_edge.h as Wang switch trigger points
 */
static inline uint32_t hp_convergence_pos(uint8_t n) {
    return (uint32_t)n * HP_CONV_PERIOD;  /* 0,12,24,...,132 */
}

/* ── Address encode: (hilbert_step, peano_phase) → tick ────── */
/*
 * hp_encode_tick — map dual-curve position to fibo clock tick
 * tick range: 0..1439 (GEOF_TICKS_ROUND)
 *
 * TODO: define mapping formula after invert type confirmed
 * Draft: tick = (hilbert_step * 10) + peano_phase → needs validation
 */
static inline uint16_t hp_encode_tick(uint32_t hilbert_step,
                                       uint8_t  conv_count) {
    /* draft — not final */
    return (uint16_t)((hilbert_step * HP_TOTAL_LINES + conv_count)
                      % GEOF_TICKS_ROUND);
}
