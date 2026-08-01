// arrange_compare.c
// Compare 4 candidate ARRANGEMENTS of the Contour Mask geometric
// weight-storage structure against the geo_jump address space (20736),
// using a REAL GGUF model for the roundtrip payload and speed bench.
//
// geo_jump space: 20736 = 2^8 x 3^4 = 256 x 81
//                 2^8 = 256 = Q8_0 weight values = a 16x16 face
//                 3^4 = 81  = 3 levels x 3 shells x 3 faces x 3 positions
//
// The 4 arrangements:
//   A) "Cube 16^3, 6-face" : 4096 units x 10 slots = 40,960 values
//                            addr = unit(12b) + slot(4b) = 16 bits
//                            6 dirs = free viewpoint rule (not stored)
//   B) "Face-as-channel"   : face 16x16 = 256 = Q8_0 channels,
//                            depth 81 = 3^4 geo slots
//                            256 x 81 = 20,736 cells = EXACTLY 1:1
//                            addr = channel(8b) + geo(7b) = 15 bits
//                            (THE LOCK: fills geo_jump 100%)
//   C) "Cube 16^3, ring16" : 4096 units x 16 slots = 65,536 = 2^16
//                            addr = 12b + 4b = 16 bits, zero waste
//   D) "Flat 256x81 grid"  : plain 2D array, no cube, baseline minimal
//
// Compile:
//   gcc -Wall -Werror -Wno-error=unused-function -O2 -std=c11 -I.
//       -o runner/explore/arrange_compare.exe runner/explore/arrange_compare.c
// Run (Windows path mandatory, MinGW fopen fails on /i/...):
//   runner/explore/arrange_compare.exe 'I:\model\Qwen3-0.6B-Q8_0.gguf'

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <inttypes.h>
#include "beam_addressing/gguf_reader.h"

#define PAT_LEN    65536   /* power-of-2 pattern length for bench */
#define N_BENCH    10000000
#define MAX_RT     40000   /* roundtrip N = min(capacity, MAX_RT) */
#define GEO_CELLS  20736   /* geo_jump full address space = 256 x 81 */

/* ================= ARRANGEMENT A: Cube 16^3, 6-face ================= */
/* 4096 units (16^3) x 10 slots (ring). 6 dirs = rule, NOT stored.     */
#define A_UNITS  4096
#define A_SLOTS  10
#define A_CAP    (A_UNITS * A_SLOTS)   /* 40,960 values */
static int8_t a_mask[A_UNITS][A_SLOTS];

/* READ: addr = unit(12b)<<4 | slot(4b). f(time) rotates ring start.   */
static inline int8_t a_decode(uint32_t addr, int t) {
    int unit  = (int)(addr >> 4);            /* 1 shift        */
    int slot  = (int)(addr & 15);            /* 1 and          */
    int start = (t / 1440) % A_SLOTS;        /* 1 div + 1 mod  */
    return a_mask[unit][(start + slot) % A_SLOTS];  /* 1 add + 1 mod + load */
}

/* WRITE: fills all 10 slots of a unit, ring rotated by start.         */
static void a_encode(const int8_t *raw, int n) {
    for (int i = 0; i < n; i++) {
        int u = i / A_SLOTS;
        int p = i % A_SLOTS;
        int start = u % A_SLOTS;
        a_mask[u][(start + p) % A_SLOTS] = raw[i];
    }
}

/* inverse: which (addr, t) retrieves raw[i]?                          */
static void a_rt_addr(int i, uint32_t *addr, int *t) {
    int u = i / A_SLOTS;
    int p = i % A_SLOTS;
    int start = u % A_SLOTS;
    *addr = (uint32_t)((u << 4) | p);
    *t    = start * 1440 + p;
}

/* bench pattern: spread over units and slots                           */
static int a_gen(uint32_t h) {
    int u = (int)(h % A_UNITS);
    int s = (int)((h >> 16) % A_SLOTS);
    return (u << 4) | s;
}

/* ================= ARRANGEMENT B: Face-as-channel (THE LOCK) ======== */
/* face 16x16 = 256 channels = Q8_0 channel space; depth 81 = 3^4.     */
/* 256 x 81 = 20,736 = EXACTLY the geo_jump space. 1:1, no time axis.  */
#define B_CH   256
#define B_GEO  81
#define B_CAP  (B_CH * B_GEO)   /* 20,736 values */
/* row padded to 128 so any 7-bit geo index is in-bounds               */
static int8_t b_mask[B_CH][128];

/* READ: addr = channel(8b)<<7 | geo(7b). t unused (no ring in B).     */
static inline int8_t b_decode(uint32_t addr, int t) {
    (void)t;
    return b_mask[addr >> 7][addr & 127];    /* 1 shift + 1 and + load */
}

static void b_encode(const int8_t *raw, int n) {
    for (int i = 0; i < n; i++)
        b_mask[i / B_GEO][i % B_GEO] = raw[i];
}

static void b_rt_addr(int i, uint32_t *addr, int *t) {
    *addr = (uint32_t)(((i / B_GEO) << 7) | (i % B_GEO));
    *t    = 0;
}

static int b_gen(uint32_t h) {
    int ch = (int)(h % B_CH);
    int g  = (int)((h >> 16) % B_GEO);
    return (ch << 7) | g;
}

/* ================= ARRANGEMENT C: Cube 16^3, ring 16 ================ */
/* 4096 units x 16 slots = 65,536 = 2^16 exact, zero address waste.    */
#define C_UNITS  4096
#define C_SLOTS  16
#define C_CAP    (C_UNITS * C_SLOTS)   /* 65,536 values */
static int8_t c_mask[C_UNITS][C_SLOTS];

static inline int8_t c_decode(uint32_t addr, int t) {
    int unit  = (int)(addr >> 4);            /* 1 shift        */
    int slot  = (int)(addr & 15);            /* 1 and          */
    int start = (t / 1440) % C_SLOTS;        /* 1 div + 1 and  */
    return c_mask[unit][(start + slot) % C_SLOTS];  /* 1 add + 1 and + load */
}

static void c_encode(const int8_t *raw, int n) {
    for (int i = 0; i < n; i++) {
        int u = i / C_SLOTS;
        int p = i % C_SLOTS;
        int start = u % C_SLOTS;
        c_mask[u][(start + p) % C_SLOTS] = raw[i];
    }
}

static void c_rt_addr(int i, uint32_t *addr, int *t) {
    int u = i / C_SLOTS;
    int p = i % C_SLOTS;
    int start = u % C_SLOTS;
    *addr = (uint32_t)((u << 4) | p);
    *t    = start * 1440 + p;
}

static int c_gen(uint32_t h) {
    int u = (int)(h % C_UNITS);
    int s = (int)((h >> 16) % C_SLOTS);
    return (u << 4) | s;
}

/* ================= ARRANGEMENT D: Flat 256x81 grid ================== */
/* plain 2D array, no cube at all. Baseline minimal implementation.    */
#define D_CH   256
#define D_GEO  81
#define D_CAP  (D_CH * D_GEO)   /* 20,736 values */
static int8_t d_mask[D_CH][D_GEO];

/* READ: flat address decomposed by division. t unused.                */
static inline int8_t d_decode(uint32_t addr, int t) {
    (void)t;
    return d_mask[addr / D_GEO][addr % D_GEO];  /* 1 div + 1 mod + load */
}

static void d_encode(const int8_t *raw, int n) {
    for (int i = 0; i < n; i++)
        d_mask[i / D_GEO][i % D_GEO] = raw[i];
}

static void d_rt_addr(int i, uint32_t *addr, int *t) {
    *addr = (uint32_t)i;
    *t    = 0;
}

static int d_gen(uint32_t h) {
    return (int)(h % D_CAP);
}

/* ================= driver ================= */
typedef struct {
    const char *name;
    uint64_t    capacity;
    int         addr_bits;
    int         index_ops_read;   /* integer index ops per read (code metric) */
    int       (*gen)(uint32_t h);
    int8_t    (*decode)(uint32_t addr, int t);
    void      (*encode)(const int8_t *raw, int n);
    void      (*rt_addr)(int i, uint32_t *addr, int *t);
    /* results filled by run_arrangement */
    uint64_t    blocks;
    double      fit_pct;          /* capacity / 20736 */
    double      util_pct;         /* valid addrs / 2^addr_bits */
    int         rt_n;
    int         rt_mism;
    double      ns_op;
    double      mops;
    int         checksum;
} Arr;

static void run_arrangement(Arr *a, const int8_t *raw, int n_avail,
                            uint64_t model_values, uint32_t *pat, int *tpat)
{
    int N = (int)(a->capacity < (uint64_t)n_avail ? a->capacity : (uint64_t)n_avail);

    printf("\n=== %s ===\n", a->name);
    printf("  capacity : %" PRIu64 " values = %" PRIu64 " bytes int8 (%.1f KB per block)\n",
           (unsigned long long)a->capacity, (unsigned long long)a->capacity,
           (double)a->capacity / 1024.0);
    a->blocks = (model_values + a->capacity - 1) / a->capacity;
    printf("  blocks   : %" PRIu64 " for %" PRIu64 " weights\n",
           (unsigned long long)a->blocks, (unsigned long long)model_values);
    a->fit_pct  = (double)a->capacity / (double)GEO_CELLS * 100.0;
    a->util_pct = (double)a->capacity /
                  (double)(1u << a->addr_bits) * 100.0;
    printf("  fit      : %6.1f%% of geo_jump 20736   addr-space util: %5.1f%% "
           "(addr_bits=%d)\n", a->fit_pct, a->util_pct, a->addr_bits);

    /* roundtrip: encode raw[0..N) into arrangement, decode back */
    a->encode(raw, N);
    int mism = 0;
    for (int i = 0; i < N; i++) {
        uint32_t addr;
        int t;
        a->rt_addr(i, &addr, &t);
        if (a->decode(addr, t) != raw[i]) mism++;
    }
    a->rt_n = N;
    a->rt_mism = mism;
    printf("  roundtrip: %d values encoded/decoded, %d mismatches -> %s\n",
           N, mism, mism == 0 ? "PASS (100% lossless)" : "FAIL");

    /* speed: 10M reads of decode(addr,time), DCE-protected volatile sink */
    for (int i = 0; i < PAT_LEN; i++) {
        pat[i]  = (uint32_t)a->gen((uint32_t)i * 2654435761u);
        tpat[i] = (i * 3) % 14400;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    volatile int8_t sink = 0;
    for (int i = 0; i < N_BENCH; i++)
        sink ^= a->decode(pat[i & (PAT_LEN - 1)], tpat[i & (PAT_LEN - 1)]);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 +
                (double)(t1.tv_nsec - t0.tv_nsec);
    a->ns_op = ns / (double)N_BENCH;
    a->mops  = 10000.0 / a->ns_op;
    a->checksum = (int)sink;
    printf("  speed    : %6.2f ns/op  (%8.1f M reads/s)  checksum=%d  index_ops/read=%d\n",
           a->ns_op, a->mops, a->checksum, a->index_ops_read);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "I:\\model\\Qwen3-0.6B-Q8_0.gguf";

    printf("=== Contour Mask arrangement comparison (real GGUF) ===\n");
    printf("  Model: %s\n", path);

    GGUF_File *gf = gguf_open(path);
    if (!gf) {
        printf("  FAIL: cannot open GGUF (must use Windows path, e.g. 'I:\\model\\...')\n");
        return 1;
    }
    printf("  Tensors: %" PRIu64 "\n", (unsigned long long)gf->tensor_count);

    uint64_t model_values = 0;
    for (uint64_t i = 0; i < gf->tensor_count; i++)
        model_values += gf->tensors[i].n_weights;
    printf("  Total weights: %" PRIu64 " (expect 596,049,920)\n",
           (unsigned long long)model_values);

    /* first tensor with >= MAX_RT weights */
    int tidx = -1;
    for (uint64_t i = 0; i < gf->tensor_count; i++) {
        if (gf->tensors[i].n_weights >= (uint64_t)MAX_RT) { tidx = (int)i; break; }
    }
    if (tidx < 0) {
        printf("  FAIL: no tensor with >= %d weights\n", MAX_RT);
        gguf_close(gf);
        return 1;
    }
    GGUF_Tensor *T = &gf->tensors[tidx];
    printf("  Tensor[%d]: %s  type=%u  n_weights=%" PRIu64 "\n",
           tidx, T->name, T->type, (unsigned long long)T->n_weights);

    /* read raw bytes (Q8_0: 1B weight bytes; same raw-payload approach
       as spec_test_contour_mask.c) */
    uint64_t n_read = MAX_RT;
    int8_t *raw = (int8_t*)malloc((size_t)n_read);
    if (!raw) { printf("  FAIL: malloc\n"); gguf_close(gf); return 1; }
    fseek(gf->fp, (long)(gf->tensor_data_start + T->offset), SEEK_SET);
    size_t got = fread(raw, 1, (size_t)n_read, gf->fp);
    if (got != n_read) {
        n_read = got;
        printf("  Read %" PRIu64 " bytes (truncated)\n", (unsigned long long)n_read);
    }
    int n_avail = (int)n_read;

    uint32_t *pat  = (uint32_t*)malloc(PAT_LEN * sizeof(uint32_t));
    int      *tpat = (int*)malloc(PAT_LEN * sizeof(int));
    if (!pat || !tpat) { printf("  FAIL: malloc pat\n"); return 1; }

    Arr arrs[4];
    arrs[0] = (Arr){ "A  Cube 16^3, 6-face (10-slot ring)",
                     A_CAP, 16, 6, a_gen, a_decode, a_encode, a_rt_addr, 0,0,0,0,0,0,0,0 };
    arrs[1] = (Arr){ "B  Face-as-channel 256x81 (THE LOCK)",
                     B_CAP, 15, 2, b_gen, b_decode, b_encode, b_rt_addr, 0,0,0,0,0,0,0,0 };
    arrs[2] = (Arr){ "C  Cube 16^3, ring 16 (2^16 exact)",
                     C_CAP, 16, 6, c_gen, c_decode, c_encode, c_rt_addr, 0,0,0,0,0,0,0,0 };
    arrs[3] = (Arr){ "D  Flat 256x81 grid (baseline)",
                     D_CAP, 15, 2, d_gen, d_decode, d_encode, d_rt_addr, 0,0,0,0,0,0,0,0 };

    for (int k = 0; k < 4; k++)
        run_arrangement(&arrs[k], raw, n_avail, model_values, pat, tpat);

    /* ---- summary table (ASCII) ---- */
    printf("\n===== SUMMARY (geo_jump space = 20736 = 2^8 x 3^4) =====\n");
    printf("%-34s %8s %8s %7s %7s %5s %8s %9s %5s\n",
           "arrangement", "cap", "blocks", "fit%", "util%",
           "rt", "ns/op", "Mreads/s", "idxop");
    printf("--------------------------------------------------------------\n");
    for (int k = 0; k < 4; k++)
        printf("%-34s %8" PRIu64 " %8" PRIu64 " %6.1f%% %6.1f%% %5s %7.2f %9.1f %5d\n",
               arrs[k].name,
               (unsigned long long)arrs[k].capacity,
               (unsigned long long)arrs[k].blocks,
               arrs[k].fit_pct, arrs[k].util_pct,
               arrs[k].rt_mism == 0 ? "PASS" : "FAIL",
               arrs[k].ns_op, arrs[k].mops, arrs[k].index_ops_read);
    printf("\nSpatial footprint on 20736 (units/channels):\n");
    printf("  A: 4096 cube units        = 4096/20736 = %.1f%% of the space\n",
           4096.0 / GEO_CELLS * 100.0);
    printf("  B: 256 channels x 81      = 20736/20736 = 100%% (1:1 lock)\n");
    printf("  C: 4096 units x 16 deep   = 65536 values = %.1f%% (over-subscribed)\n",
           65536.0 / GEO_CELLS * 100.0);
    printf("  D: 256 channels x 81 flat = 20736/20736 = 100%%\n");

    free(raw); free(pat); free(tpat);
    gguf_close(gf);
    return 0;
}
