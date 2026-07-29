# geo_frame_seek_wang.h

> 🟡 **[STALE]** — This file may be inactive or pending removal. Check before relying on it.

**Module:** `geopixel`  
**Path:** `geopixel/include/geo_frame_seek_wang.h`  
**Status:** `stale`  
**Note:** included by only 1 file(s)  
**Generated:** 2026-07-29 13:39  

## Description

* geo_frame_seek_wang.h — Wang Tile Layer for geo_frame_seek
* 1440 frames → 120 windows × 12 frames (1 phase cycle per window)
* Wang edge = Fibonacci 2&7 chord on 9-point clock
* Edge derivation (per enc):
*   edge_A = (enc × 2) % 9   — World A (binary multiplier)
*   edge_B = (enc × 7) % 9   — World B (complement)
*   invariant: edge_A + edge_B == 9  (enc%9 != 0), both 0 if enc%9==0
*   → tamper detect: sum != 9 → corrupt
* 369 self-reference: enc%9 ∈ {0,3,6} → Tesla loop = skip boundary
* Tamper: edge_bot[w] == edge_top[w+1]
* Parity: XOR of enc per window → reconstruct 1 missing

## Structures

- `typedef struct`
- `typedef struct`

## API Functions

- `static inline uint8_t _fwang_chord_a(uint16_t enc)`
- `static inline uint8_t _fwang_chord_b(uint16_t enc)`
- `static inline bool _fwang_is_369(uint16_t enc)`
- `static inline bool _fwang_chord_valid(uint16_t enc)`
- `static inline void _fwang_set_dirty(FrameWangLayer *wl, uint16_t win)`
- `static inline bool _fwang_is_dirty(const FrameWangLayer *wl, uint16_t win)`
- `static inline void _fwang_clear_dirty(FrameWangLayer *wl, uint16_t win)`
- `static inline void fwang_compute_win(FrameWangLayer *wl, uint16_t win)`
- `static inline void fwang_init(FrameWangLayer *wl)`
- `static inline void fwang_flush_dirty(FrameWangLayer *wl)`
- `static inline bool fwang_edge_valid(const FrameWangLayer *wl, uint16_t win)`
- `static inline bool fwang_edge_valid_wrap(const FrameWangLayer *wl, uint16_t win)`
- `static inline bool fwang_tamper_check(const FrameWangWindow *w)`
- `static inline FrameWangDecision fwang_seek_gate(FrameWangLayer *wl,`
- `static inline bool fwang_reconstruct_enc(const FrameWangWindow *w,`
- `static inline int fwang_verify(const FrameWangLayer *wl)`

## Constants

- `#define GEO_FRAME_SEEK_WANG_H`
- `#define WANG_WIN_SIZE    12u`
- `#define WANG_WIN_COUNT   120u   /* 1440 / 12 */`

