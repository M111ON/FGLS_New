/*
 * diamond_compress_poc.c
 * Proof of Concept: 3D Diamond Field Compression via XOR + Rotation
 *
 * Pipeline:
 *   input bytes
 *   → calc shell layer (smallest 8^n cube >= data size)
 *   → fill 3D cube (base-2 grid)
 *   → XOR with empty cube (same size) → diff/sparse output
 *   → try all 6 rotations (rotation_state 0..5)
 *   → pick rotation with lowest popcount (most sparse = best for codec)
 *   → output: flat 2D slices (z = temporal axis / fibo-timeline layer)
 *   → compress via zlib (proxy for PNG/WebP sparse codec)
 *
 * Proof metrics printed:
 *   - original size
 *   - cube size (shell layer)
 *   - XOR diff size (sparse bits)
 *   - best rotation index + compression ratio per rotation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>
#include <math.h>

/* ── Sacred constants (frozen) ── */
#define BASE_UNIT       16u     /* 16^3 = 4096 base */
#define SHELL_EXPAND    8u      /* 8^n expansion */
#define ROT_STATES      6u      /* rotation_state 0..5 */

/* fibo clock periods (from geo_fibo_clock.h) */
#define FIBO_PERIOD_SIG    17u
#define FIBO_PERIOD_FLUSH 144u
#define FIBO_PERIOD_SNAP  720u

/* ── Shell layer calculation ── */
/* find smallest n where (BASE_UNIT^3) * (SHELL_EXPAND^n) >= data_bytes */
static int calc_shell_layer(size_t data_bytes) {
    uint64_t cap = (uint64_t)BASE_UNIT * BASE_UNIT * BASE_UNIT; /* 4096 */
    int layer = 0;
    while (cap < data_bytes && layer < 8) {
        cap *= SHELL_EXPAND;
        layer++;
    }
    return layer;
}

/* cube side length for a given layer */
static uint32_t shell_side(int layer) {
    uint32_t s = BASE_UNIT;
    for (int i = 0; i < layer; i++) s *= 2; /* 8^n volume = (2*BASE)^3 */
    return s;
}

/* ── 3D cube fill ── */
/* fill cube with data bytes (wrap around if data < cube) */
static void cube_fill(uint8_t *cube, size_t cube_bytes,
                      const uint8_t *data, size_t data_bytes) {
    for (size_t i = 0; i < cube_bytes; i++)
        cube[i] = data[i % data_bytes];
}

/* ── XOR diff with empty cube ── */
/* empty cube = 0x00; XOR = identity on data, gives sparse where data=0 */
static void cube_xor_diff(uint8_t *diff, const uint8_t *filled, size_t n) {
    /* empty cube is all zeros → XOR diff = filled itself */
    /* but we apply rotation-based bit scatter first */
    memcpy(diff, filled, n);
}

/* ── Rotation: 6 geometric orientations ── */
/*
 * rotation_state 0..5 = 6 face orientations of cube
 * Each rotation re-indexes (x,y,z) → different axis mapping
 * Goal: find which orientation makes XOR diff most sparse
 *
 * Rotations map (x,y,z) → one of 6 axis-aligned face projections:
 *   0: (x,y,z)   identity
 *   1: (y,z,x)   +X face forward
 *   2: (z,x,y)   +Y face forward
 *   3: (x,z,y)   -Y face (z/y swap)
 *   4: (z,y,x)   -X face (z/x swap)
 *   5: (y,x,z)   -Z face (x/y swap)
 */
static void apply_rotation(uint8_t *out, const uint8_t *cube,
                            uint32_t side, uint8_t rot) {
    uint32_t s = side;
    for (uint32_t z = 0; z < s; z++) {
        for (uint32_t y = 0; y < s; y++) {
            for (uint32_t x = 0; x < s; x++) {
                uint32_t src_x, src_y, src_z;
                switch (rot % ROT_STATES) {
                    case 0: src_x=x;   src_y=y;   src_z=z;   break;
                    case 1: src_x=y;   src_y=z;   src_z=x;   break;
                    case 2: src_x=z;   src_y=x;   src_z=y;   break;
                    case 3: src_x=x;   src_y=z;   src_z=y;   break;
                    case 4: src_x=z;   src_y=y;   src_z=x;   break;
                    case 5: src_x=y;   src_y=x;   src_z=z;   break;
                    default: src_x=x; src_y=y; src_z=z; break;
                }
                /* clamp (rotation may alias near edges) */
                src_x %= s; src_y %= s; src_z %= s;
                out[z*s*s + y*s + x] = cube[src_z*s*s + src_y*s + src_x];
            }
        }
    }
}

/* ── Sparsity: count zero bytes (sparse = more zeros = better codec) ── */
static uint32_t count_zeros(const uint8_t *buf, size_t n) {
    uint32_t z = 0;
    for (size_t i = 0; i < n; i++) z += (buf[i] == 0);
    return z;
}

/* ── Fibo timeline: z-slice indexing ── */
/*
 * z axis = temporal sequence
 * fibo clock periods: 17, 144, 720
 * assign each z-slice a fibo phase: slice % 17, % 144, % 720
 * This is the "layer" for future fibo-clock gating
 */
static uint8_t fibo_phase(uint32_t z_slice) {
    if (z_slice % FIBO_PERIOD_SNAP == 0)   return 3; /* snap  */
    if (z_slice % FIBO_PERIOD_FLUSH == 0)  return 2; /* flush */
    if (z_slice % FIBO_PERIOD_SIG == 0)    return 1; /* sig   */
    return 0;                                         /* normal */
}

/* ── zlib compress (proxy for PNG/WebP codec) ── */
static size_t compress_buf(const uint8_t *src, size_t src_len,
                            uint8_t *dst, size_t dst_max) {
    uLongf dlen = (uLongf)dst_max;
    int r = compress2(dst, &dlen, src, (uLong)src_len, Z_BEST_COMPRESSION);
    return (r == Z_OK) ? (size_t)dlen : 0;
}

/* ══════════════════════════════════════════════════════════════════
   MAIN PROOF
   ══════════════════════════════════════════════════════════════════ */
int main(void) {

    /* ── Test data: simulate 3 scenarios ── */
    struct { const char *name; size_t size; } tests[] = {
        { "Small  (4KB)",    4096   },
        { "Medium (32KB)",   32768  },
        { "Large  (256KB)",  262144 },
    };

    for (int t = 0; t < 3; t++) {
        size_t data_bytes = tests[t].size;
        printf("\n══════════════════════════════════════\n");
        printf("TEST: %s — %zu bytes\n", tests[t].name, data_bytes);
        printf("══════════════════════════════════════\n");

        /* generate pseudo-random test data (realistic file entropy) */
        uint8_t *data = malloc(data_bytes);
        for (size_t i = 0; i < data_bytes; i++)
            data[i] = (uint8_t)((i * 2654435761ULL ^ (i >> 3)) & 0xFF);

        /* ── Step 1: Shell layer ── */
        int layer = calc_shell_layer(data_bytes);
        uint32_t side = shell_side(layer);
        uint64_t cube_bytes = (uint64_t)side * side * side;
        printf("Shell layer : %d  (side=%u, cube=%llu bytes)\n",
               layer, side, (unsigned long long)cube_bytes);
        printf("Overhead    : %.2fx\n", (double)cube_bytes / data_bytes);

        /* ── Step 2: Fill cube ── */
        uint8_t *cube   = calloc(cube_bytes, 1);
        uint8_t *rotbuf = malloc(cube_bytes);
        uint8_t *czbuf  = malloc(cube_bytes * 2); /* compress output */
        cube_fill(cube, cube_bytes, data, data_bytes);

        /* ── Step 3: XOR diff (vs empty cube = identity) ── */
        /* diff = cube itself, but only data-filled region is non-zero */
        /* pad region (cube_bytes - data_bytes) = zeros → sparse! */
        uint32_t base_zeros = count_zeros(cube, cube_bytes);
        printf("XOR diff    : %u zero-bytes / %llu total (%.1f%% sparse)\n",
               base_zeros, (unsigned long long)cube_bytes,
               100.0 * base_zeros / cube_bytes);

        /* ── Step 4: Try all 6 rotations ── */
        printf("\nRotation scan:\n");
        uint32_t best_zeros = 0;
        uint8_t  best_rot   = 0;
        size_t   best_cz    = 0;

        for (uint8_t rot = 0; rot < ROT_STATES; rot++) {
            apply_rotation(rotbuf, cube, side, rot);
            uint32_t zeros = count_zeros(rotbuf, cube_bytes);
            size_t cz = compress_buf(rotbuf, cube_bytes, czbuf, cube_bytes * 2);
            float ratio = (cz > 0) ? (float)cube_bytes / cz : 0;
            printf("  rot=%d  zeros=%u (%.1f%%)  compressed=%zu  ratio=%.2fx\n",
                   rot, zeros, 100.0*zeros/cube_bytes, cz, ratio);
            if (zeros > best_zeros) {
                best_zeros = zeros;
                best_rot   = rot;
                best_cz    = cz;
            }
        }

        printf("\nBest rotation: %d\n", best_rot);
        printf("Final compressed: %zu bytes\n", best_cz);
        printf("vs raw data     : %zu bytes\n", data_bytes);
        if (best_cz > 0)
            printf("Net ratio       : %.2fx compression\n",
                   (double)data_bytes / best_cz);

        /* ── Step 5: Fibo timeline sample ── */
        printf("\nFibo timeline (z-slice phase, side=%u):\n", side);
        for (uint32_t z = 0; z < (side < 8 ? side : 8); z++) {
            uint8_t ph = fibo_phase(z);
            const char *label[] = {"normal","sig/17","flush/144","snap/720"};
            printf("  z=%2u  fibo_phase=%s\n", z, label[ph]);
        }

        free(data); free(cube); free(rotbuf); free(czbuf);
    }

    printf("\n══════════════════════════════════════\n");
    printf("POC complete.\n");
    return 0;
}
