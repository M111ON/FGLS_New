/*
 * bench_bond_tgw.c — Bond → TGW Full-Stack Benchmark v1.0
 *
 * Stages measured:
 *   S1  seed_from_fp + make_piece → PoglsSlot   (Piece Factory)
 *   S2  bond_verify N pairs                      (Bond Verify)
 *   S3  tgw_dispatch N slots                     (TGW Dispatch)
 *   S4  tgw_get_stats ring scan                  (Ring Analysis)
 *
 * Input: Fibonacci-weighted synthetic data 8K/16K/32K/64K
 *        64 bytes/chunk = 1 piece — mirrors real geo_field chunks
 *
 * Compile (Linux):   gcc -O2 -I. -o bench_bond_tgw bench_bond_tgw.c
 * Compile (Windows): gcc -O2 -I. -D__USE_MINGW_ANSI_STDIO -o bench_bond_tgw bench_bond_tgw.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "pogls_bond.h"
#include "tgw_bond_dispatch.h"

/* ── timing ────────────────────────────────────────────────── */

static double now_sec(void) {
#if defined(_WIN32)
    return (double)clock() / CLOCKS_PER_SEC;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

/* ── synthetic data: Fibonacci-weighted byte pattern ─────── */
/*    i*37 + (i>>3)*13 + fib[i%12]  — same formula as poc files */

static void gen_data(uint8_t *buf, size_t n) {
    static const uint8_t fib12[12] = {1,1,2,3,5,8,13,21,34,55,89,144};
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)((i * 37 + (i >> 3) * 13 + fib12[i % 12]) & 0xFF);
}

/* chunk-local topology_fp: FNV-1a over 16-point strided sample */
static void derive_fp(const uint8_t *chunk, size_t n, char fp_out[17]) {
    uint64_t h = POGLS_FNV_OFFSET;
    size_t stride = (n >= 16) ? n / 16 : 1;
    for (size_t i = 0; i < 16 && i * stride < n; i++) {
        h ^= chunk[i * stride];
        h *= POGLS_FNV_PRIME;
    }
    snprintf(fp_out, 17, "%016llx", (unsigned long long)h);
}

/* ── slot builder ───────────────────────────────────────────── */

static void build_slot(PoglsSlot *out, uint64_t seed, uint8_t axis,
                       uint32_t agent_id, uint32_t token_cap) {
    memset(out, 0, sizeof(PoglsSlot));
    out->piece     = pogls_make_piece(seed, axis);
    out->agent_id  = agent_id;
    out->token_cap = token_cap;
}

/* ── result struct ──────────────────────────────────────────── */

typedef struct {
    size_t   data_bytes;
    uint32_t n_pieces;
    double   t_s1_ms;
    double   t_s2_ms;
    double   t_s3_ms;
    double   t_s4_ms;
    double   t_total_ms;
    double   tp_e2e;       /* pieces/s end-to-end */
    double   tp_verify;    /* verify/s            */
    double   tp_dispatch;  /* dispatch/s          */
    uint32_t valid_bonds;
    TgwStats stats;
} BenchResult;

/* shape schedule: 7-cycle covers all shapes evenly */
static const uint8_t  AXIS7[7]    = {1, 2, 3, 4, 5, 6, 7};
static const uint32_t FANOUT3[3]  = {10, 11, 12};

/* ── single benchmark run ───────────────────────────────────── */

static BenchResult run_bench(size_t data_bytes) {
    BenchResult r;
    memset(&r, 0, sizeof(r));
    r.data_bytes = data_bytes;

    uint8_t *data = (uint8_t *)malloc(data_bytes);
    gen_data(data, data_bytes);

    uint32_t N = (uint32_t)(data_bytes / 64);
    r.n_pieces  = N;

    PoglsSlot *sa = (PoglsSlot *)calloc(N, sizeof(PoglsSlot));
    PoglsSlot *sb = (PoglsSlot *)calloc(N, sizeof(PoglsSlot));

    TgwDispatchCtx ctx;
    tgw_dispatch_init(&ctx, 0xBEEF000000000001ULL);

    /* ── S1: Piece Factory ─────────────────────────────── */
    double t0 = now_sec();
    for (uint32_t i = 0; i < N; i++) {
        char fp[17];
        derive_fp(data + (size_t)i * 64, 64, fp);
        uint64_t seed_a = pogls_seed_from_fp(fp);
        uint64_t seed_b = seed_a ^ ((uint64_t)i + 1); /* cross-seed pair */
        uint8_t  axis   = AXIS7[i % 7];
        build_slot(&sa[i], seed_a, axis, i,     4096);
        build_slot(&sb[i], seed_b, axis, i + N, 4096);
    }
    r.t_s1_ms = (now_sec() - t0) * 1000.0;

    /* ── S2: Bond Verify ───────────────────────────────── */
    t0 = now_sec();
    for (uint32_t i = 0; i < N; i++) {
        PoglsBond b = pogls_bond_verify(&sa[i].piece, &sb[i].piece);
        r.valid_bonds += b.valid;
    }
    r.t_s2_ms = (now_sec() - t0) * 1000.0;

    /* ── S3: TGW Dispatch ──────────────────────────────── */
    t0 = now_sec();
    for (uint32_t i = 0; i < N; i++) {
        uint8_t axis = AXIS7[i % 7];
        uint8_t fn   = (axis == 3) ? 3 : 0;  /* axis=3 → T-shape fanout */
        tgw_dispatch(&ctx, &sa[i], &sb[i], fn ? FANOUT3 : NULL, fn);
    }
    r.t_s3_ms = (now_sec() - t0) * 1000.0;

    /* ── S4: Ring Analysis ─────────────────────────────── */
    t0 = now_sec();
    r.stats = tgw_get_stats(&ctx);
    r.t_s4_ms = (now_sec() - t0) * 1000.0;

    r.t_total_ms  = r.t_s1_ms + r.t_s2_ms + r.t_s3_ms + r.t_s4_ms;
    double t_s    = r.t_total_ms / 1000.0;
    r.tp_e2e      = (t_s        > 0) ? N / t_s                      : 0;
    r.tp_verify   = (r.t_s2_ms > 0) ? N / (r.t_s2_ms  / 1000.0)   : 0;
    r.tp_dispatch = (r.t_s3_ms > 0) ? N / (r.t_s3_ms  / 1000.0)   : 0;

    free(data); free(sa); free(sb);
    return r;
}

/* ── output ─────────────────────────────────────────────────── */

static void print_result(const BenchResult *r) {
    double pct_v = r->n_pieces > 0 ? 100.0 * r->valid_bonds / r->n_pieces : 0;
    printf("┌─ %4zuK bytes  N=%-4u pieces\n",
           r->data_bytes / 1024, r->n_pieces);
    printf("│  S1  Piece Factory  %8.3f ms  %9.0f pieces/s\n",
           r->t_s1_ms,  r->n_pieces / (r->t_s1_ms  / 1000.0));
    printf("│  S2  Bond Verify    %8.3f ms  %9.0f verify/s   valid=%u (%.0f%%)\n",
           r->t_s2_ms,  r->tp_verify, r->valid_bonds, pct_v);
    printf("│  S3  TGW Dispatch   %8.3f ms  %9.0f dispatch/s\n",
           r->t_s3_ms,  r->tp_dispatch);
    printf("│  S4  Ring Scan      %8.3f ms\n", r->t_s4_ms);
    printf("│  ──────────────────────────────────────────────────\n");
    printf("│  Total              %8.3f ms  %9.0f pieces/s (e2e)\n",
           r->t_total_ms, r->tp_e2e);
    printf("│\n");
    printf("│  TGW  dispatched=%-5llu  grounded=%-5llu  quarantined=%-5llu\n",
           (unsigned long long)r->stats.total_dispatched,
           (unsigned long long)r->stats.total_grounded,
           (unsigned long long)r->stats.total_quarantined);
    printf("│       route_slots=%-4u  ground_slots=%-4u  held=%-4u  fanout=%-4u\n",
           r->stats.route_slots, r->stats.ground_slots,
           r->stats.held_slots,  r->stats.fanout_slots);
    printf("└───────────────────────────────────────────────────────────\n\n");
}

int main(void) {
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║  BOND → TGW FULL-STACK BENCHMARK  v1.0                   ║\n");
    printf("║  Piece Factory → Bond Verify → TGW Dispatch → Ring Scan  ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");
    printf("Shape schedule: I O T S Z L J  (7-cycle)\n");
    printf("  ROUTE  (I,O,T,J) 4/7=57%%    GROUND (S,Z,L) 3/7=43%%\n");
    printf("  seed_b = seed_a ^ i  →  cross-seed pairs, bond_valid~0%%\n");
    printf("  dispatch runs on piece.shape regardless (non-gated by bond)\n\n");

    static const size_t SIZES[4] = {8192, 65536, 524288, 2097152};
    for (int i = 0; i < 4; i++) {
        BenchResult r = run_bench(SIZES[i]);
        print_result(&r);
    }
    return 0;
}
