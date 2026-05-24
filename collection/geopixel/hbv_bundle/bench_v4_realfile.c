/*
 * bench_v4_realfiles.c — v4 pipeline on real project files
 * Compares: v3 batch (best_rot=0, no seed) vs v4 (rotation scan + hilbert seed)
 *
 * Build:
 *   gcc -O2 -I/path/to/new_diamond_tring -I/path/to/core \
 *       bench_v4_realfiles.c -lm -o bench_v4_realfiles
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "pogls_fold.h"
#include "/tmp/geo_diamond_field_v4.h"

/* ── file loader ─────────────────────────────────────────── */
static uint8_t *load_file(const char *path, uint32_t *len_out) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); return NULL; }
    fclose(f); *len_out = (uint32_t)sz; return buf;
}

/* ── metrics ─────────────────────────────────────────────── */
typedef struct {
    uint64_t n_chunks;
    uint64_t n_single;      /* encoded as single chunks */
    uint64_t n_batched;     /* encoded as part of a batch */
    uint64_t n_batch_groups;
    uint64_t raw_bytes;
    uint64_t enc_bytes_single;
    uint64_t enc_bytes_batch;
    uint64_t roundtrip_ok;
    uint64_t roundtrip_fail;
    double   enc_ms;
    double   dec_ms;
    /* v4 specific */
    uint64_t rot_wins[6];
    uint64_t seed_valid_batches;
    int      seed_pc_sum;
    int      seed_pc_count;
} BenchMetrics;

/* ── single-chunk encode (with rotation, from bench_v3_integrated) ── */
static uint32_t encode_single_rotated(DiamondField *df,
                                       uint8_t rot_table[], /* tick→rot */
                                       uint32_t rot_cap,
                                       const uint8_t chunk[64],
                                       BenchMetrics *m)
{
    uint8_t rotbuf[64], best_buf[64];
    uint8_t best_rot = 0; int best_pc = -1;

    for (uint8_t rot = 0; rot < 6; rot++) {
        /* inline _shell_rotate64: 4×4×4 re-index */
        for (uint8_t z=0;z<4;z++) for (uint8_t y=0;y<4;y++) for (uint8_t x=0;x<4;x++) {
            uint8_t sx,sy,sz2;
            switch(rot){
                case 0:sx=x;sy=y;sz2=z;break; case 1:sx=y;sy=z;sz2=x;break;
                case 2:sx=z;sy=x;sz2=y;break; case 3:sx=x;sy=z;sz2=3-y;break;
                case 4:sx=z;sy=y;sz2=3-x;break; default:sx=3-y;sy=x;sz2=z;break;
            }
            rotbuf[z*16+y*4+x]=chunk[sz2*16+sy*4+sx];
        }
        DiamondBlock db; memset(&db,0,sizeof(db));
        memcpy(&db.core.raw, rotbuf, 8); db.invert=~db.core.raw;
        fold_build_quad_mirror(&db);
        if(!fold_xor_audit(&db)){db.invert=~db.core.raw;fold_build_quad_mirror(&db);}
        int pc = __builtin_popcountll(fold_fibo_intersect(&db));
        if (pc > best_pc) { best_pc=pc; best_rot=rot; memcpy(best_buf,rotbuf,64); }
    }

    uint32_t gidx = dfield_encode(df, best_buf, NULL);
    if (gidx == SLOT_NULL) return SLOT_NULL;

    uint32_t tick = df->sidx.tick[gidx];
    if (tick < rot_cap) rot_table[tick] = best_rot;
    m->rot_wins[best_rot]++;
    return gidx;
}

/* ── inverse rotate ─────────────────────────────────────── */
static void inverse_rotate64(uint8_t out[64], const uint8_t in[64], uint8_t rot) {
    /* inverse of 4×4×4 rotation */
    uint8_t tmp[64];
    for (uint8_t z=0;z<4;z++) for (uint8_t y=0;y<4;y++) for (uint8_t x=0;x<4;x++) {
        uint8_t sx,sy,sz2;
        switch(rot%6){
            case 0:sx=x;sy=y;sz2=z;break;    /* identity = own inverse */
            case 1:sx=z;sy=x;sz2=y;break;    /* inv of (y,z,x) = (z,x,y) */
            case 2:sx=y;sy=z;sz2=x;break;    /* inv of (z,x,y) = (y,z,x) */
            case 3:sx=x;sy=3-z;sz2=y;break;  /* inv of (x,z,3-y) */
            case 4:sx=3-z;sy=y;sz2=x;break;  /* inv of (z,y,3-x) */
            default:sx=y;sy=3-x;sz2=z;break; /* inv of (3-y,x,z) */
        }
        tmp[sz2*16+sy*4+sx]=in[z*16+y*4+x];
    }
    memcpy(out,tmp,64);
}

/* ── run bench on one file ──────────────────────────────── */
static BenchMetrics bench_file(const char *path,
                                uint8_t batch_layer, /* 0=L1/8, 1=L2/64 */
                                int do_batch)
{
    BenchMetrics m; memset(&m,0,sizeof(m));

    uint32_t file_len;
    uint8_t *buf = load_file(path, &file_len);
    if (!buf) return m;

    uint64_t N = file_len / 64;
    if (N == 0) { free(buf); return m; }
    m.n_chunks  = N;
    m.raw_bytes = N * 64;

    uint32_t tring_cap = (uint32_t)(N * 3 + 256);
    DiamondField df; dfield_init(&df, tring_cap);
    uint8_t *rot_table = calloc(tring_cap, 1);
    uint32_t *gidxs = malloc(N * sizeof(uint32_t));
    uint32_t *batch_gidxs = malloc(512 * sizeof(uint32_t));

    uint32_t batch_sz = do_batch
        ? (batch_layer == 0 ? BATCH_L1 : BATCH_L2)
        : 0;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    uint64_t i = 0;
    while (i < N) {
        if (do_batch && (N - i) >= batch_sz) {
            /* try batch */
            uint32_t enc = dfield_encode_batch(&df,
                buf + i*64, batch_sz, batch_layer, batch_gidxs);
            if (enc == batch_sz) {
                /* check hilbert seed validity */
                uint32_t sv = df.sidx.tick[batch_gidxs[0]];
                uint32_t tick = sidx_is_batch(sv) ? sidx_batch_tick(sv) : sv;
                uint32_t sz2; const uint8_t *wire = tring_read(&df.tring,tick,&sz2);
                if (wire && sz2 >= 12) {
                    uint8_t flags = wire[3];
                    if (flags & 1) {
                        m.seed_valid_batches++;
                        uint64_t seed; memcpy(&seed, wire+4, 8);
                        m.seed_pc_sum += __builtin_popcountll(seed);
                        m.seed_pc_count++;
                        m.rot_wins[wire[1]]++;
                    }
                }
                for (uint32_t j=0;j<enc;j++) gidxs[i+j]=batch_gidxs[j];
                m.n_batched  += enc;
                m.n_batch_groups++;
                /* encode cost: 4+8+64 header + enc×avg_diff */
                m.enc_bytes_batch += 4 + 8 + 64 + enc * (8 + 4); /* est avg 4 diffs */
                i += enc;
                continue;
            }
        }
        /* single */
        gidxs[i] = encode_single_rotated(&df, rot_table, tring_cap,
                                          buf + i*64, &m);
        m.n_single++;
        /* encode cost: 64B raw (worst case) or sparse */
        m.enc_bytes_single += 64;
        i++;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    m.enc_ms = (t1.tv_sec-t0.tv_sec)*1e3 + (t1.tv_nsec-t0.tv_nsec)*1e-6;

    /* decode + verify */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint64_t ci = 0; ci < N; ci++) {
        if (gidxs[ci] == SLOT_NULL) continue;
        uint8_t out[64]; memset(out,0,64);
        int r = dfield_decode(&df, gidxs[ci], out);
        if (r != 0) { m.roundtrip_fail++; continue; }
        /* inverse rotate if single */
        uint32_t sv = df.sidx.tick[gidxs[ci]];
        if (!sidx_is_batch(sv)) {
            uint32_t tick = sv;
            uint8_t rot = (tick < tring_cap) ? rot_table[tick] : 0;
            if (rot > 0) {
                uint8_t tmp[64]; memcpy(tmp,out,64);
                inverse_rotate64(out,tmp,rot);
            }
        }
        if (memcmp(out, buf + ci*64, 64) == 0) m.roundtrip_ok++;
        else m.roundtrip_fail++;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    m.dec_ms = (t1.tv_sec-t0.tv_sec)*1e3 + (t1.tv_nsec-t0.tv_nsec)*1e-6;

    dfield_free(&df);
    free(rot_table); free(gidxs); free(batch_gidxs); free(buf);
    return m;
}

static void print_metrics(const char *label, const char *mode,
                           const BenchMetrics *m)
{
    uint64_t total_enc = m->enc_bytes_single + m->enc_bytes_batch;
    double ratio = total_enc > 0 ? (double)m->raw_bytes / total_enc : 0;
    double exact  = m->n_chunks > 0
        ? (double)m->roundtrip_ok * 100.0 / m->n_chunks : 0;

    printf("  %-10s  N=%4llu  ratio=%5.2fx  exact=%5.1f%%  "
           "batch=%llu/%llu  seed_valid=%llu",
           mode,
           (unsigned long long)m->n_chunks, ratio, exact,
           (unsigned long long)m->n_batched,
           (unsigned long long)m->n_chunks,
           (unsigned long long)m->seed_valid_batches);

    if (m->seed_pc_count > 0)
        printf("  seed_pc_avg=%.1f", (double)m->seed_pc_sum/m->seed_pc_count);

    /* dominant rotation */
    uint8_t dom = 0;
    for (uint8_t r=1;r<6;r++) if(m->rot_wins[r]>m->rot_wins[dom]) dom=r;
    if (m->rot_wins[dom] > 0) printf("  dom_rot=%d", dom);

    printf("  enc=%.1fms dec=%.1fms\n", m->enc_ms, m->dec_ms);
}

int main(void) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  v4 Real File Bench — Hilbert XOR-fold + Rotation Scan  ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    struct { const char *label, *path; } files[] = {
        /* project headers (largest first) */
        {"geo_diamond_field.h",  "/tmp/core/geo_diamond_field.h"},
        {"geo_temporal_lut.h",   "/tmp/core/geo_temporal_lut.h"},
        {"geo_whe.h",            "/tmp/core/geo_whe.h"},
        {"pogls_fold.h",         "/tmp/core/pogls_fold.h"},
        {"pogls_v3.h",           "/tmp/core/pogls_v3.h"},
        {"pogls_compress.h",     "/tmp/core/pogls_compress.h"},
        {"angular_mapper.c",     "/tmp/core/angular_mapper_v36.c"},
        {"geo_fibo_clock.h",     "/tmp/core/geo_fibo_clock.h"},
        {"tring.h",              "/tmp/new_diamond_tring/tring.h"},
        {"test_diamond_v3.c",    "/tmp/new_diamond_tring/test_diamond_field_v3.c"},
        {NULL, NULL}
    };

    for (int fi = 0; files[fi].label; fi++) {
        uint32_t fl = 0;
        uint8_t *tb = load_file(files[fi].path, &fl);
        if (!tb) { printf("  %-28s  [cannot open]\n", files[fi].label); continue; }
        uint64_t N = fl / 64;
        if (N < 8) { printf("  %-28s  [too small, %llu chunks]\n",
                            files[fi].label, (unsigned long long)N); free(tb); continue; }
        free(tb);

        printf("┌─ %-28s  %u B / %llu chunks\n",
               files[fi].label, fl, (unsigned long long)N);

        /* no batch (single only) */
        BenchMetrics m0 = bench_file(files[fi].path, 0, 0);
        print_metrics(files[fi].label, "single", &m0);

        /* L1 batch (8 chunks) */
        if (N >= 8) {
            BenchMetrics m1 = bench_file(files[fi].path, 0, 1);
            print_metrics(files[fi].label, "L1(8)", &m1);
        }

        /* L2 batch (64 chunks) */
        if (N >= 64) {
            BenchMetrics m2 = bench_file(files[fi].path, 1, 1);
            print_metrics(files[fi].label, "L2(64)", &m2);
        }
        printf("└─\n");
    }

    /* rotation distribution summary */
    printf("\nRotation semantics:\n");
    const char *rn[] = {
        "0 identity","1 +X","2 +Y","3 -Y","4 -X","5 -Z"
    };
    for (int r=0;r<6;r++) printf("  rot=%s\n",rn[r]);

    return 0;
}
