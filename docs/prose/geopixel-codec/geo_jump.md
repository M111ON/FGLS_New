# geo_jump.h

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `geopixel`  
**Path:** `geopixel/include/geo_jump.h`  
**Status:** `active`  
**Note:** modified 24d ago; included by 37 file(s)  
**Generated:** 2026-07-29 10:11  

## Structures

- `typedef struct`
- `typedef struct`
- `typedef struct`
- `typedef struct`

## API Functions

- `static inline uint32_t _hilbert_idx(uint32_t x, uint32_t y, uint32_t n)`
- `static inline uint32_t _peano_idx(uint32_t x, uint32_t y, uint32_t cols __attribute__((unused)), uint32_t rows)`
- `static inline uint32_t geo_pentagon_id(uint32_t node_id)`
- `static inline uint32_t geo_shell_level(uint32_t node_id)`
- `static inline uint32_t geo_clock_tick(uint32_t node_id)`
- `static inline uint32_t _jump_hilbert(uint32_t node, uint32_t col, uint32_t row, uint32_t floor)`
- `return GEO_WRAP(tower * GEO_TOWER + offset)`
- `static inline uint32_t _jump_peano(uint32_t node, uint32_t col, uint32_t row, uint32_t floor)`
- `return GEO_WRAP(tower * GEO_TOWER + offset)`
- `static inline uint32_t _jump_ground(uint32_t node, uint32_t col, uint32_t row)`
- `return GEO_WRAP(tower * GEO_TOWER + g_idx)`
- `static inline uint32_t _jump_pentagon(uint32_t node, uint32_t layer)`
- `return GEO_WRAP(face * face_stride + layer * GEO_TOWER)`
- `static inline uint32_t _jump_mod(uint32_t node, uint32_t mult)`
- `return GEO_WRAP((uint64_t)node * mult)`
- `static inline uint32_t _jump_invert(uint32_t node, uint32_t tower_off)`
- `return GEO_WRAP(next_t * GEO_BLOCK + mirror)`
- `uint32_t geo_jump(uint32_t node_id, GeoJumpType type, uint32_t param)`
- `uint32_t geo_jump_r(uint32_t node_id, const GeoJumpRouter *r)`
- `return GEO_WRAP(face * face_stride + layer * GEO_TOWER)`

## Constants

- `#define GEO_JUMP_H`
- `#define GEO_METATRON_COLS     4u`
- `#define GEO_METATRON_ROWS     4u`
- `#define GEO_METATRON_FLOORS   3u`
- `#define GEO_METATRON_CELLS    (GEO_METATRON_COLS * GEO_METATRON_ROWS)`
- `#define GEO_BLOCK             (GEO_METATRON_CELLS * GEO_METATRON_FLOORS)`
- `#define GEO_TOWER             (GEO_BLOCK * GEO_METATRON_FLOORS)`
- `#define GEO_FULL              (GEO_TOWER * GEO_TOWER)`
- `#define GEO_PENTAGONS        12u`
- `#define GEO_PENT_RING        10u`
- `#define GEO_FIBO_CLOCK      1440u`
- `#define GEO_SHELL_TICK       12u`
- `#define GEO_MOD_PRIME       162u`
- `#define GEO_WRAP(x)    ((uint32_t)(x) % GEO_FULL)`
- `#define GEO_CLIMATE_TROPICAL    0u`

