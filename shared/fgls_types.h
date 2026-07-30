/**
 * shared/fgls_types.h — Shared data types between FGLS core and visualization
 * 
 * This header defines the data structures that both projects use.
 * - FGLS_new/core: produces data in these formats
 * - FGLS_viz: consumes/visualizes data in these formats
 * 
 * UPDATE THIS FILE when data format changes.
 * Both projects import from here → automatic sync.
 */

#ifndef FGLS_TYPES_H
#define FGLS_TYPES_H

#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════
 * Silk Screen V4: 10 boxes × 6 dirs × 1440 ticks
 * ═══════════════════════════════════════════════════════════════════ */

#define SS_N_BOXES      10
#define SS_N_DIRS       6
#define SS_N_TICKS      1440
#define SS_SLOTS_PER_TICK  (SS_N_BOXES * SS_N_DIRS)  // 60

// Direction IDs (Bond pairing: A:a, B:b, C:c)
typedef enum {
    DIR_A = 0,  // +X
    DIR_a = 1,  // -X
    DIR_B = 2,  // +Y
    DIR_b = 3,  // -Y
    DIR_C = 4,  // +Z
    DIR_c = 5,  // -Z
    DIR_COUNT = 6
} SilkDir;

// Single weight slot
typedef struct {
    uint8_t box;    // 0-9
    uint8_t dir;    // 0-5 (SilkDir)
    uint16_t tick;  // 0-1439
    int8_t weight;  // the value
} SilkSlot;

// Silk Screen block (one chunk's worth of data)
typedef struct {
    int8_t filter[SS_N_BOXES][SS_N_DIRS][SS_N_TICKS];
    uint32_t n_encoded;  // how many slots were filled
} SilkBlock;

/* ═══════════════════════════════════════════════════════════════════
 * h-depth Variable Resolution Scaling
 * ═══════════════════════════════════════════════════════════════════ */

#define HDEPTH_LEVELS  3
#define HDEPTH_SLOTS_0 64
#define HDEPTH_SLOTS_1 128
#define HDEPTH_SLOTS_2 256

// Resolution thresholds (t = r_cut / R)
#define HDEPTH_T_HIGH  0.8
#define HDEPTH_T_MED   0.4

typedef struct {
    double h;       // intersection depth
    double t;       // capacity ratio t = r_cut / R
    int level;      // 0=64, 1=128, 2=256
    int slots;      // actual slot count
} HDepthResult;

/* ═══════════════════════════════════════════════════════════════════
 * GGUF Tensor Info (for reader)
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    const char *name;
    uint32_t n_dims;
    uint32_t dims[4];
    uint32_t type;      // GGUF type (e.g., 8 = Q8_0)
    uint64_t offset;    // data offset in file
    uint64_t size;      // data size in bytes
} GGUFTensorInfo;

/* ═══════════════════════════════════════════════════════════════════
 * Benchmark Results (for reporting)
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    double compute_ns;      // h → resolution time (ns)
    double throughput_mops; // million ops/sec
    long total_slots;       // total slots used
    long fixed_slots;       // what fixed resolution would use
    double savings_pct;     // percentage saved
} HDepthBenchmark;

#endif /* FGLS_TYPES_H */
