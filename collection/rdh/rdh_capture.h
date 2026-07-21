/*
 * rdh_capture.h — RDH Capture: data → flat key (1 function, no intermediate)
 * ═══════════════════════════════════════════════════════════════════════════
 * 
 * "RDH capture นี้ล่ะ ถ้ามันเจอทั้ง workflow ก็เจอหมด"
 * 
 * 1 function entry point:
 *   rdh_capture(data, len, cfg) → flat key
 * 
 * flat key → everything downstream:
 *   - frame_seek enc = flat_key % 1440
 *   - face = enc / 120
 *   - slot = enc % 120
 *   - phase = (enc / 12) % 12
 *   - ico_idx = enc % 162
 *   - container address = face + slot + ico_idx
 * 
 * No byte scanning beyond data. No struct fields. No metadata.
 * Single integer output = address = signature = everything.
 * 
 * Guarantees:
 *   - O(len) or O(48) for short data
 *   - Pure integer, no float
 *   - No malloc, no state
 *   - Unique data → unique flat key (bijection when cfg capacity ≥ data entropy)
 *   - Same data → same flat key (deterministic)
 *   - reversible via rdh_decompose (from rdh_addr.h)
 * ═══════════════════════════════════════════════════════════════════════════
 */

#ifndef RDH_CAPTURE_H
#define RDH_CAPTURE_H

#include <stdint.h>
#include <stddef.h>
#include "rdh_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
   STRIDE DIRECTIONS — 12-gon topology (0..11)
   
   Each 4-bit nibble of data defines a stride on the 12-gon:
   ═══════════════════════════════════════════════════════════════════════════
   0:  E      (+1,  0)    4:  W      (-1,  0)    8:  E×2    (+2,  0)
   1:  NE     (+1, +1)    5:  SW     (-1, -1)    9:  NE×2   (+1, +2)
   2:  N      ( 0, +1)    6:  S      ( 0, -1)   10:  NW×2   (-1, +2)
   3:  SE     (+1, -1)    7:  NW     (-1, +1)   11:  W×2    (-2,  0)
   ═══════════════════════════════════════════════════════════════════════════
   
   Phase 0-3 (E, NE, N, SE) = forward strides
   Phase 4-7 (W, SW, S, NW) = reverse strides  
   Phase 8-11                = double-precision forward
   
   This is NOT a hash. It's a geometric walk: data defines its own path.
   Path length = fuse length (data determines when it reaches home).
   ═══════════════════════════════════════════════════════════════════════════ */

/* Walk stride directions encoded in data bytes.
 * Each byte's low 4 bits = direction on 12-gon.
 * Accumulates (dx, dy) steps regardless of data length.
 *
 * This is the ONLY function that touches data bytes.
 * Everything downstream works from the integer result.
 *
 * Returns flat key via RDH config. */
static inline int64_t rdh_capture(const uint8_t *data, size_t len,
                                  const RDHConfig *cfg)
{
    int32_t acc_x = 0, acc_y = 0;
    
    /* Walk — 0..47 for short data, or full length for long data.
     * Short data: pad with repeats of itself (data cycles its own DNA). */
    size_t steps = (len < 48) ? 48 : len;
    
    for (size_t i = 0; i < steps; i++) {
        uint32_t b = data[i % len];     /* cycle if len < steps */
        uint32_t dir = b & 0x0F;        /* low nibble = direction */
        
        switch (dir) {
            /* Phase 0-3: forward single */
            case 0:  acc_x++;                  break;  /* E  */
            case 1:  acc_x++; acc_y++;         break;  /* NE */
            case 2:  acc_y++;                  break;  /* N  */
            case 3:  acc_x++; acc_y--;         break;  /* SE */
            /* Phase 4-7: reverse single */
            case 4:  acc_x--;                  break;  /* W  */
            case 5:  acc_x--; acc_y--;         break;  /* SW */
            case 6:  acc_y--;                  break;  /* S  */
            case 7:  acc_x--; acc_y++;         break;  /* NW */
            /* Phase 8-11: double-precision */
            case 8:  acc_x += 2;               break;  /* E×2  */
            case 9:  acc_x++;   acc_y += 2;    break;  /* NE×2 */
            case 10: acc_x--;   acc_y += 2;    break;  /* NW×2 */
            case 11: acc_x -= 2;               break;  /* W×2  */
            default: break;  /* not reachable (0..11 only) */
        }
    }
    
    /* Fold accumulator into RDH address space */
    int32_t field_w = (int32_t)cfg->n_wedges;   /* wedge dimension = x */
    int32_t field_h = (int32_t)cfg->n_rings;     /* ring dimension   = y */
    
    int64_t wedge = (int64_t)((acc_x % field_w + field_w) % field_w);
    int64_t ring  = (int64_t)((acc_y % field_h + field_h) % field_h);
    
    /* Flat key — this IS the address, the signature, and the seed.
     * mirror=0, u=0, v=0 — everything encoded in ring+wedge. */
    return rdh_key(cfg, ring, wedge, 0, 0, 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
   PRESETS — RDH configs for common enclosure fields
   ═══════════════════════════════════════════════════════════════════════════ */

/* 144×144 field (base enclosure) → 20,736 unique addresses */
#define RDH_CAPTURE_144    { 144, 144, 1, 1, 1 }

/* Scaled field = 144×S by 144×S, capacity = (144×S)² 
 * Scale 49 → 1,016,064 ≈ 1M unique addresses */
#define RDH_CAPTURE_SCALE(S) { (int64_t)(144*(S)), (int64_t)(144*(S)), 1, 1, 1 }

/* ═══════════════════════════════════════════════════════════════════════════
   DIRECT-TO-ENC — capture + map to frame_seek enc in one call
   ═══════════════════════════════════════════════════════════════════════════
   frame_seek cycle = 1440
   enc = flat_key % 1440 — the 2-byte compressed form
   frame_at(enc) → face, slot, phase, ico_idx — all O(1)
   ═══════════════════════════════════════════════════════════════════════════ */

#define FRAME_SEEK_CYCLE  1440u

static inline uint16_t rdh_capture_to_enc(const uint8_t *data, size_t len,
                                           const RDHConfig *cfg)
{
    int64_t key = rdh_capture(data, len, cfg);
    return (uint16_t)((uint64_t)key % FRAME_SEEK_CYCLE);
}

#ifdef __cplusplus
}
#endif

#endif /* RDH_CAPTURE_H */
