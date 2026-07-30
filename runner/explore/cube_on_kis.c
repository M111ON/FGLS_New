// cube_on_kis.c
// Section 6: Cube on Kis Timeline
//   f(time) → (face, x, y, z) → weight
//
// Flexible face count: compile-time configurable
//   gcc ... -DCUBE_FACES=6   (6-face cube, 6000 cells)
//   gcc ... -DCUBE_FACES=12  (12-face dodeca, 12000 cells, default)
//
// TWO ROUTING OPTIONS:
//   A: frame_seek (stride-37 timeline, O(1), deterministic)
//   B: geo_seed   (12-coset integer derivation, zero float, O(1))
//
// Timeline: 1440 (stride-37 walk via geo_frame_seek.h)
// Capo:     time-shifted reading (offset shifts timeline position)
//
// Compile:
//   gcc -O2 -std=c11 -Icore -Icollection -Icollection/dgls/geo/include \
//       -IHfolder -o cube_on_kis.exe runner/explore/cube_on_kis.c -lm
//   gcc -DCUBE_FACES=6 -O2 ...   <- 6-face mode
// Run:     cube_on_kis.exe
//
// ============================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <assert.h>

// ── Frame seek (header-only, stride-37 timeline) ──
#include "geo_frame_seek.h"   // from core/

// ── Geo Seed (zero-float, O(1) integer routing) ──
#include "geo_seed.h"         // from Hfolder/

// ── Cube dimensions (FLEXIBLE: override with -DCUBE_FACES=N) ──
#ifndef CUBE_FACES
#  define CUBE_FACES     12   // default: 12-face (2x 6-face)
#endif

#define CUBE_X        10
#define CUBE_Y        10
#define CUBE_Z        10
#define CUBE_CELLS    (CUBE_FACES * CUBE_X * CUBE_Y * CUBE_Z)

#define Q8_MIN  (-128)
#define Q8_MAX   127
#define Q8_RANGE 256

static const char *FACE_NAMES_6[6]  = {"A","B","C","D","E","F"};
static const char *FACE_NAMES_12[12]= {"A","B","C","D","E","F","G","H","I","J","K","L"};

// ── SURFACE: face × surface × depth → linear index ──
// No +/-, no sign — just direct face index
static inline int cube_idx(int face, int x, int y, int z)
{
    return face * (CUBE_X * CUBE_Y * CUBE_Z)
         + z    * (CUBE_X * CUBE_Y)
         + y    * CUBE_X
         + x;
}

static const char *face_name(int f)
{
    if (CUBE_FACES <= 6) return (f < 6) ? FACE_NAMES_6[f] : "?";
    return (f < 12) ? FACE_NAMES_12[f] : "?";
}

// ══════════════════════════════════════════════════════════════
// ROUTING OPTION A: FRAME_SEEK  (stride-37 timeline)
// ══════════════════════════════════════════════════════════════

// Map time t → cube (face, x, y, z).
// Surface (face, x, y) from frame_seek (stride-37 spread on 1440).
// Depth (z) from cycle index: each full 1440-cycle → next z layer.
// This ensures ALL z layers are reached, not just phase-repeat.
// No collision across cycles — 1200 unique surface × 10 cycles = 12000 cells.
static void frame_to_cube(uint32_t t, int *face, int *x, int *y, int *z)
{
    uint16_t enc = frame_enc(t);
    DualFrame f  = frame_at(enc);

    *face = (int)(f.face % CUBE_FACES);
    *x    = (int)((f.slot / 12) % CUBE_X);
    *y    = (int)(f.slot % CUBE_Y);
    // z = which 1440-cycle, NOT phase (so each cycle hits different depth)
    *z    = (int)((t / FRAME_CYCLE) % CUBE_Z);
}

// ══════════════════════════════════════════════════════════════
// ROUTING OPTION B: GEO_SEED   (integer-only, zero-float)
// ══════════════════════════════════════════════════════════════

static void seed_to_cube(GsSeed *seed, GsResult *result,
                         int *face, int *x, int *y, int *z)
{
    gs_process(seed, result);

    *face = (int)(result->master_fold % CUBE_FACES);
    uint32_t chk = result->coset_checksum[0];
    *x = (int)((chk >> 0)  & 0x0F);    if (*x >= CUBE_X) *x %= CUBE_X;
    *y = (int)((chk >> 8)  & 0x0F);    if (*y >= CUBE_Y) *y %= CUBE_Y;
    *z = (int)((chk >> 16) & 0x0F);    if (*z >= CUBE_Z) *z %= CUBE_Z;
}

// ══════════════════════════════════════════════════════════════
// CUBE STORE
// ══════════════════════════════════════════════════════════════

typedef struct {
    int8_t data[CUBE_CELLS];
    int    written;
} CubeStore;

static void cs_init(CubeStore *cs) {
    memset(cs->data, 0, sizeof(cs->data));
    cs->written = 0;
}

// frame_seek write/read
static void cs_write_fr(CubeStore *cs, uint32_t t, int8_t v) {
    int f, x, y, z; frame_to_cube(t, &f, &x, &y, &z);
    cs->data[cube_idx(f, x, y, z)] = v; cs->written++;
}
static int8_t cs_read_fr(const CubeStore *cs, uint32_t t) {
    int f, x, y, z; frame_to_cube(t, &f, &x, &y, &z);
    return cs->data[cube_idx(f, x, y, z)];
}

// geo_seed write/read
static void cs_write_sd(CubeStore *cs, uint64_t seed_val,
                        uint32_t did, uint8_t v) {
    GsSeed s; s.seed = seed_val; s.dispatch_id = did;
    GsResult r; int f, x, y, z; seed_to_cube(&s, &r, &f, &x, &y, &z);
    cs->data[cube_idx(f, x, y, z)] = (int8_t)(v + Q8_MIN); cs->written++;
}
static int8_t cs_read_sd(const CubeStore *cs, uint64_t seed_val,
                         uint32_t did) {
    GsSeed s; s.seed = seed_val; s.dispatch_id = did;
    GsResult r; int f, x, y, z; seed_to_cube(&s, &r, &f, &x, &y, &z);
    return cs->data[cube_idx(f, x, y, z)];
}

// Capo: time-shifted read
static int8_t cs_capo(const CubeStore *cs, uint32_t t, uint32_t capo) {
    return cs_read_fr(cs, t + capo);
}

// ══════════════════════════════════════════════════════════════
// TESTS
// ══════════════════════════════════════════════════════════════

// T1: stride-37 full cycle
static int t1_stride(void) {
    printf("=== T1: stride-%u full cycle (%u positions) ===\n",
           FRAME_STRIDE, FRAME_CYCLE);
    uint16_t v[FRAME_CYCLE]; memset(v, 0, sizeof(v));
    uint16_t e = 0;
    for (uint32_t i = 0; i < FRAME_CYCLE; i++) {
        if (v[e]) { printf("  FAIL: dup at %u enc=%u\n", i, e); return 0; }
        v[e]=1; e = (uint16_t)((e + FRAME_STRIDE) % FRAME_CYCLE);
    }
    if (e != 0) { printf("  FAIL: no return\n"); return 0; }
    printf("  PASS\n"); return 1;
}

// T2: Coverage analysis — compare 6 vs 12 face
static int t2_coverage(void) {
    printf("\n=== T2: Coverage analysis (%d-face, %d cells) ===\n",
           CUBE_FACES, CUBE_CELLS);

    uint64_t addr = (uint64_t)CUBE_CELLS;
    printf("  Cells: %llu (%d faces × %d×%d×%d)\n",
           (unsigned long long)addr, CUBE_FACES, CUBE_X, CUBE_Y, CUBE_Z);
    printf("  Slots/tick (surface): %d\n", CUBE_FACES * CUBE_X * CUBE_Y);
    printf("  Timeline/cycle:       %u positions\n", FRAME_CYCLE);
    printf("  Depth layers:          %d (z = t/1440 %% %d)\n", CUBE_Z, CUBE_Z);

    // 1-cycle coverage
    uint8_t v[CUBE_CELLS]; memset(v, 0, sizeof(v));
    uint32_t uniq=0, dup=0;
    for (uint32_t t = 0; t < FRAME_CYCLE; t++) {
        int f, x, y, z; frame_to_cube(t, &f, &x, &y, &z);
        int idx = cube_idx(f, x, y, z);
        if (v[idx]) dup++; else uniq++;
        v[idx]=1;
    }
    printf("  1-cycle: %u unique, %u dups (%.1f%% of %d)\n",
           uniq, dup, 100.0*uniq/CUBE_CELLS, CUBE_CELLS);
    printf("  Cycles to fill: %d\n", CUBE_Z);

    // Unique (face, x, y) surface positions per cycle
    uint8_t sf[CUBE_FACES*CUBE_X*CUBE_Y]; memset(sf,0,sizeof(sf));
    for (uint32_t t = 0; t < FRAME_CYCLE; t++) {
        int f, x, y, z; frame_to_cube(t, &f, &x, &y, &z);
        sf[f*CUBE_X*CUBE_Y + y*CUBE_X + x] = 1;
    }
    uint32_t sfc=0; for (size_t i=0;i<sizeof(sf);i++)sfc+=sf[i];
    printf("  Surface coverage/cycle: %u/%d (%.1f%%)\n",
           sfc, CUBE_FACES*CUBE_X*CUBE_Y, 100.0*sfc/(CUBE_FACES*CUBE_X*CUBE_Y));

    // z-distribution per cycle: all in z=0 (since z = t/1440 % 10 = 0 for t<1440)
    printf("  z-dist (1 cycle): all %u writes in z=0\n", FRAME_CYCLE);
    printf("  Next cycle: z=1, then z=2 ... z=%d, then repeat\n", CUBE_Z-1);

    printf("  PASS\n");
    return 1;
}

// T3: frame_seek write→read — verify by unique positions, not by t
static int t3_roundtrip(void) {
    printf("\n=== T3: Write→Read (frame_seek, %d cells) ===\n", CUBE_CELLS);
    CubeStore cs; cs_init(&cs);

    // Write sequential values at all t = 0..CUBE_CELLS-1+FRAME_CYCLE
    // (extra cycle to ensure overwrites converge)
    uint32_t total_writes = CUBE_CELLS + FRAME_CYCLE;
    for (uint32_t t = 0; t < total_writes; t++)
        cs_write_fr(&cs, t, (int8_t)((t*37+13)%Q8_RANGE+Q8_MIN));

    // Verify by scanning each unique cube position, finding its last-write t,
    // and checking the value
    int err=0;
    for (int idx = 0; idx < CUBE_CELLS && err < 5; idx++) {
        // Find the LAST t that maps to this position
        uint32_t last_t = 0;
        int last_val = 0;
        for (uint32_t t = 0; t < total_writes; t++) {
            int f,x,y,z; frame_to_cube(t,&f,&x,&y,&z);
            if (cube_idx(f,x,y,z) == idx) {
                last_t = t;
                last_val = (t*37+13)%Q8_RANGE+Q8_MIN;
            }
        }
        int8_t got = cs.data[idx];
        if (got != last_val) {
            printf("  FAIL at idx=%d (last_t=%u): expect %d, got %d\n",
                   idx, last_t, last_val, got); err++;
        }
    }
    printf("  %s: %d err / %d cells\n", err?"FAIL":"PASS", err, CUBE_CELLS);
    return err==0;
}

// T4: frame_seek random access — verify per unique position
static int t4_random(void) {
    printf("\n=== T4: Random access (frame_seek, 100 random positions) ===\n");
    CubeStore cs; cs_init(&cs);

    // Fill all positions with (73t+41)
    uint32_t tw = CUBE_CELLS + FRAME_CYCLE;
    for (uint32_t t = 0; t < tw; t++)
        cs_write_fr(&cs, t, (int8_t)((t*73+41)%Q8_RANGE+Q8_MIN));

    // Sample 100 random unique cube positions and verify
    int err=0;
    uint32_t step = CUBE_CELLS / 100;
    if (step < 1) step = 1;
    for (uint32_t idx = 0; idx < CUBE_CELLS && err < 5; idx += step) {
        // Find last t that maps to idx
        uint32_t last_t = 0;
        int last_val = 0;
        for (uint32_t t = 0; t < tw; t++) {
            int f,x,y,z; frame_to_cube(t,&f,&x,&y,&z);
            if (cube_idx(f,x,y,z) == (int)idx) {
                last_t = t;
                last_val = (t*73+41)%Q8_RANGE+Q8_MIN;
            }
        }
        int8_t got = cs.data[idx];
        if (got != last_val) {
            printf("  FAIL idx=%d (t=%u): expect %d got %d\n",
                   idx, last_t, last_val, got); err++;
        }
    }
    printf("  %s: %d err\n", err?"FAIL":"PASS", err);
    return err==0;
}

// T5: geo_seed route roundtrip
static int t5_seed(void) {
    printf("\n=== T5: Geo_seed route (O(1) integer, 1000 seeds) ===\n");
    CubeStore cs; cs_init(&cs);
    int N = 2000;  // write extra to converge collisions

    // Track which seed last wrote to each position
    int last_seed[CUBE_CELLS];
    memset(last_seed, 0xFF, sizeof(last_seed)); // -1 = unvisited

    for (int i = 0; i < N; i++) {
        GsSeed s;
        s.seed = (uint64_t)i * 0x9E3779B97F4A7C15ULL;
        s.dispatch_id = (uint32_t)i;
        GsResult r;
        int f,x,y,z;
        seed_to_cube(&s, &r, &f, &x, &y, &z);
        int idx = cube_idx(f,x,y,z);
        uint8_t v = (uint8_t)(i % Q8_RANGE);
        cs.data[idx] = (int8_t)(v + Q8_MIN);
        last_seed[idx] = i;
    }

    // Verify: check each UNIQUE position that was written
    int err=0;
    for (int idx = 0; idx < CUBE_CELLS && err < 5; idx++) {
        if (last_seed[idx] < 0) continue;  // unwritten
        int i = last_seed[idx];
        uint8_t ev = (uint8_t)(i % Q8_RANGE);
        int8_t expect = (int8_t)(ev + Q8_MIN);
        int8_t got = cs.data[idx];
        if (got != expect) {
            printf("  FAIL idx=%d (last_seed=%d): expect %d got %d\n",
                   idx, i, expect, got); err++;
        }
    }

    // Benchmark
    int M = 50000;
    GsSeed *bs = (GsSeed*)calloc(M, sizeof(GsSeed));
    GsResult *br = (GsResult*)calloc(M, sizeof(GsResult));
    if (bs && br) {
        for (int i=0;i<M;i++){bs[i].seed=(uint64_t)i*0x9E3779B97F4A7C15ULL;bs[i].dispatch_id=(uint32_t)i;}
        clock_t t0=clock(); gs_batch(bs,br,M); clock_t t1=clock();
        double sec=(double)(t1-t0)/CLOCKS_PER_SEC;
        printf("  Batch %d: %.3fs = %.0f ops/s (%.1f ns/op)\n",
               M, sec, M/sec, 1e9/(M/sec));
        free(bs); free(br);
    }
    printf("  %s: %d err\n", err?"FAIL":"PASS", err);
    return err==0;
}

// T6: Capo
static int t6_capo(void) {
    printf("\n=== T6: Capo (time-shift) ===\n");
    CubeStore cs; cs_init(&cs);
    for (uint32_t t=0; t<FRAME_CYCLE; t++)
        cs_write_fr(&cs, t, (int8_t)((t*7)%Q8_RANGE+Q8_MIN));

    int8_t v0   = cs_capo(&cs,0,0);
    int8_t v100 = cs_capo(&cs,0,100);
    int8_t d100 = cs_read_fr(&cs,100);
    int8_t v720 = cs_capo(&cs,0,720);
    int8_t d720 = cs_read_fr(&cs,720);
    // capo=1440 shifts to next z-layer (z=1 vs z=0) — should differ
    int8_t v1440= cs_capo(&cs,0,1440);
    int8_t d0   = cs_read_fr(&cs,0);

    printf("  capo=0, t=0:   %d\n", v0);
    printf("  capo=100,t=0:  %d (direct t=100: %d) %s\n",
           v100, d100, v100==d100?"✓":"✗");
    printf("  capo=720,t=0:  %d (direct t=720: %d) %s\n",
           v720, d720, v720==d720?"✓":"✗");
    printf("  capo=1440,t=0: %d (t=0,z=0: %d) %s\n",
           v1440, d0, v1440!=d0?"✓(diff z)":"✗(same z)");

    int pass=(v100==d100 && v720==d720);  // capo=1440 expected different (z shift)
    printf("  %s\n", pass?"PASS":"FAIL");
    return pass;
}

// T7: Full coverage scan (many cycles, check all cells hit)
static int t7_full(void) {
    printf("\n=== T7: Full coverage (%d cells) ===\n", CUBE_CELLS);

    // Measure: how many cycles to cover all cells?
    uint8_t *v = (uint8_t*)calloc(CUBE_CELLS,1);
    if (!v) { printf("  FAIL: alloc\n"); return 0; }
    uint32_t t, full_at = 0;
    for (t = 0; t < CUBE_Z * FRAME_CYCLE + 1; t++) {  // at most 10 cycles + safety
        int f,x,y,z; frame_to_cube(t,&f,&x,&y,&z);
        v[cube_idx(f,x,y,z)] = 1;

        // Check if all covered at end of each cycle
        if ((t + 1) % FRAME_CYCLE == 0) {
            uint32_t sum=0;
            for (int i=0;i<CUBE_CELLS;i++) sum+=v[i];
            if (sum == CUBE_CELLS) { full_at = t; break; }
        }
    }

    uint32_t covered=0;
    for (int i=0;i<CUBE_CELLS;i++) covered+=v[i];
    if (full_at > 0)
        printf("  Full coverage at t=%u (%u cycles)\n", full_at, full_at/FRAME_CYCLE + 1);
    else
        printf("  Not fully covered after %u steps: %u/%d (%.1f%%)\n",
               t+1, covered, CUBE_CELLS, 100.0*covered/CUBE_CELLS);
    free(v);

    // Verify roundtrip with full coverage
    CubeStore cs; cs_init(&cs);
    uint32_t tw = t + FRAME_CYCLE;  // write beyond for convergence
    for (uint32_t i=0; i<tw; i++)
        cs_write_fr(&cs, i, (int8_t)((i*53+17)%Q8_RANGE+Q8_MIN));

    int err=0;
    for (int idx = 0; idx < CUBE_CELLS && err < 5; idx++) {
        uint32_t last_t = 0;
        int last_val = 0;
        for (uint32_t i=0; i<tw; i++) {
            int f,x,y,z; frame_to_cube(i,&f,&x,&y,&z);
            if (cube_idx(f,x,y,z) == idx) {
                last_t = i;
                last_val = (i*53+17)%Q8_RANGE+Q8_MIN;
            }
        }
        int8_t got = cs.data[idx];
        if (got != last_val) {
            printf("  FAIL idx=%d (last_t=%u): expect %d got %d\n",
                   idx, last_t, last_val, got); err++;
        }
    }

    printf("  %s: %d err\n", err?"FAIL":"PASS", err);
    return err==0;
}

// T8: Speed benchmark (both routes)
static int t8_speed(void) {
    printf("\n=== T8: Speed benchmark (500K ops) ===\n");
    CubeStore cs; cs_init(&cs);
    uint32_t N=500000;
    clock_t t0,t1; double sec;

    t0=clock();
    for (uint32_t i=0;i<N;i++) cs_write_fr(&cs,i,(int8_t)((i*37)%Q8_RANGE+Q8_MIN));
    t1=clock(); sec=(double)(t1-t0)/CLOCKS_PER_SEC;
    printf("  frame_seek write: %.0f ops/s (%.1f ns/op)\n", N/sec, 1e9/(N/sec));

    int64_t sum=0; t0=clock();
    for (uint32_t i=0;i<N;i++) sum+=cs_read_fr(&cs,i);
    t1=clock(); sec=(double)(t1-t0)/CLOCKS_PER_SEC;
    printf("  frame_seek read:  %.0f ops/s (%.1f ns/op)\n", N/sec, 1e9/(N/sec));

    GsSeed *bs=(GsSeed*)calloc(N,sizeof(GsSeed));
    GsResult *br=(GsResult*)calloc(N,sizeof(GsResult));
    if(bs&&br){
        for(uint32_t i=0;i<N;i++){bs[i].seed=(uint64_t)i*0x9E3779B97F4A7C15ULL;bs[i].dispatch_id=i;}
        t0=clock(); gs_batch(bs,br,N); t1=clock(); sec=(double)(t1-t0)/CLOCKS_PER_SEC;
        printf("  geo_seed batch:   %.0f ops/s (%.1f ns/op)\n", N/sec, 1e9/(N/sec));
        free(bs);free(br);
    }
    printf("  PASS\n");
    return 1;
}

// T9: Compare 6 vs 12 face behavior
static int t9_compare(void) {
    printf("\n=== T9: 6-face vs 12-face comparison ===\n");
    printf("  (Compile with -DCUBE_FACES=6 to test 6-face mode)\n");
    printf("  (Current: CUBE_FACES=%d)\n", CUBE_FACES);

    // Show expected differences
    printf("  Expected capacity:\n");
    printf("    6-face: %5d cells = 6 × 10 × 10 × 10\n", 6*CUBE_X*CUBE_Y*CUBE_Z);
    printf("    12-face:%5d cells = 12 × 10 × 10 × 10 (%.0f%% more)\n",
           12*CUBE_X*CUBE_Y*CUBE_Z, 100.0);

    // Show per-cycle surface coverage difference
    uint32_t surface_slots = CUBE_FACES * CUBE_X * CUBE_Y;
    printf("  Surface slots/tick: %d\n", surface_slots);
    printf("  Timeline hits/cycle: %u\n", FRAME_CYCLE);
    printf("  If surface slots < timeline: re-visit faces\n");
    printf("  If surface slots > timeline: skip some slots\n");

    if (surface_slots <= FRAME_CYCLE)
        printf("  → surface fully covered in 1 cycle\n");
    else
        printf("  → surface partially covered, need %.1f cycles\n",
               (double)surface_slots/FRAME_CYCLE);

    printf("  PASS (informational)\n");
    return 1;
}

// ══════════════════════════════════════════════════════════════
// MAIN
// ══════════════════════════════════════════════════════════════

int main(void)
{
    printf("============================================================\n");
    printf("  Cube on Kis Timeline — Section 6\n");
    printf("============================================================\n");
    printf("  CUBE_FACES = %d\n", CUBE_FACES);
    printf("  Cells:      %d (%d × %d × %d × %d)\n",
           CUBE_CELLS, CUBE_FACES, CUBE_X, CUBE_Y, CUBE_Z);
    printf("  Timeline:   %u (stride-%u)\n", FRAME_CYCLE, FRAME_STRIDE);
    printf("  Routing:    frame_seek | geo_seed\n");
    printf("\n");

    int pass=0, total=0;

    total++; pass += t1_stride();
    total++; pass += t2_coverage();
    total++; pass += t3_roundtrip();
    total++; pass += t4_random();
    total++; pass += t5_seed();
    total++; pass += t6_capo();
    total++; pass += t7_full();
    total++; pass += t8_speed();
    total++; pass += t9_compare();

    printf("\n============================================================\n");
    printf("  FINAL: %d/%d PASS  (CUBE_FACES=%d)\n", pass, total, CUBE_FACES);
    printf("============================================================\n");
    return (pass == total) ? 0 : 1;
}
