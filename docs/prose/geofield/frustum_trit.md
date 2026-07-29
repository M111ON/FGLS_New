# frustum_trit.h

> 🟡 **[STALE]** — This file may be inactive or pending removal. Check before relying on it.

**Module:** `collection`  
**Path:** `collection/geopixel/geofield/frustum_trit.h`  
**Status:** `stale`  
**Note:** last modified 65d ago  
**Generated:** 2026-07-29 15:50  

## Description

* frustum_trit.h — Trit Decomposition Engine
* ════════════════════════════════════════════════════════════════
* Pure computational layer — no geometry deps, no GEO_WALK, no tetra.
* Virtual apex is computed and subtracted; nothing maps into 5-tetra.
* One trit (0..26 = 3³) encodes three structural roles simultaneously:
*   trit  = (addr ^ value) % 27      [3³ address, 0..26]
*   coset = trit / 3                 [GiantCube zone, 0..8  = 3²]
*   face  = trit % 6                 [cube direction, 0..5]
*   level = trit % 4                 [core depth,     0..3  = 2²]
*   letter= addr  % 26               [LetterPair A..Z, 0..25]
*   slope = fibo_seed ^ addr         [apex fingerprint, XOR-reversible]
* Cycle properties:
*   lcm(3,6,4) = 12  → pattern repeats every 12 trits
*   27 = 2×12 + 3    → two full cycles + one extra coset
*   storing trit alone recovers all three roles (zero redundancy)
* Slope / apex fingerprint:
*   slope_A XOR slope_B = addr_A XOR addr_B
*   collision: slope equality ↔ same fibo-space position
*   recovery:  addr = slope XOR fibo_seed  (1 op)
*   security:  without fibo_seed → slope leaks nothing about addr

## Structures

- `typedef struct`

## API Functions

- `static inline TritAddr trit_decompose(uint64_t addr,`
- `static inline uint64_t trit_recover_addr(uint64_t slope, uint64_t fibo_seed)`
- `static inline int trit_collision(const TritAddr *a, const TritAddr *b)`
- `static inline int trit_coset_silent(uint16_t reserved_mask, uint8_t coset)`
- `static inline char trit_letter_char(uint8_t letter)`

## Constants

- `#define FRUSTUM_TRIT_H`
- `#define TRIT_MOD       27u   /* 3³ */`
- `#define COSET_COUNT     9u   /* 3² */`
- `#define FACE_COUNT      6u   /* cube faces */`
- `#define LEVEL_COUNT     4u   /* 2²  */`
- `#define LETTER_COUNT   26u   /* A..Z */`
- `#define FIBO_SEED_DEFAULT  1696631ULL  /* PHI_UP constant */`

