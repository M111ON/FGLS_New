/*
 * geo_vault_bench.c — GeoVault Performance Benchmark
 * ════════════════════════════════════════════════════
 * Measures and compares:
 *   1. Pack throughput     (files → .geovault)
 *   2. Sequential read     (cat all files via gv_extract)
 *   3. Random seek read    (O(1) chunk seek by slot)
 *   4. Hash lookup         (gv_find × N queries)
 *   5. Baseline ext4       (fopen/fread same files)
 *
 * compile:
 *   gcc -O2 -o geo_vault_bench geo_vault_bench.c -lzstd
 *
 * usage:
 *   ./geo_vault_bench [--files N] [--size KB] [--iters N]
 * ════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "geo_vault.h"
#include "geo_vault_io.h"

/* ── config defaults ──────────────────────────────────────────────── */
#define BENCH_FILES_DEFAULT   64
#define BENCH_SIZE_KB_DEFAULT  16    /* per file */
#define BENCH_ITERS_DEFAULT  1000    /* random seek iters */
#define BENCH_TMPDIR          "/tmp/gvbench"
#define BENCH_VAULT           "/tmp/gvbench.geovault"

/* ── timer ────────────────────────────────────────────────────────── */
static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ── helpers ──────────────────────────────────────────────────────── */
static void mkdirp(const char *path) {
    char tmp[512]; snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

static void rm_rf(const char *path) {
    char cmd[512]; snprintf(cmd, sizeof(cmd), "rm -rf %s", path);
    int _sr = system(cmd); (void)_sr;
}

/* xorshift RNG — no rand() dependency */
static uint32_t xr_state = 0xdeadbeef;
static uint32_t xrand(void) {
    xr_state ^= xr_state << 13;
    xr_state ^= xr_state >> 17;
    xr_state ^= xr_state << 5;
    return xr_state;
}

/* ── result printer ───────────────────────────────────────────────── */
typedef struct {
    const char *label;
    double      elapsed_sec;
    uint64_t    bytes;
    uint64_t    ops;
} BenchResult;

static void print_result(BenchResult r) {
    double mbps = (r.bytes > 0 && r.elapsed_sec > 0)
                ? (double)r.bytes / r.elapsed_sec / (1024.0*1024.0) : 0.0;
    double ops  = (r.ops > 0 && r.elapsed_sec > 0)
                ? (double)r.ops / r.elapsed_sec : 0.0;
    printf("  %-30s  %7.3f s", r.label, r.elapsed_sec);
    if (mbps > 0) printf("  %8.2f MB/s", mbps);
    if (ops  > 0) printf("  %10.0f ops/s", ops);
    printf("\n");
}

/* separator */
static void sep(void) { printf("  %s\n", "──────────────────────────────────────────────────────"); }

/* ══════════════════════════════════════════════════════════════════
 * PHASE 1 — generate test files on ext4/tmpfs
 * ══════════════════════════════════════════════════════════════════ */
static char **gen_files(int n_files, int size_kb, uint64_t *total_bytes_out) {
    mkdirp(BENCH_TMPDIR);
    char **paths = malloc((size_t)n_files * sizeof(char*));
    uint64_t total = 0;

    for (int i = 0; i < n_files; i++) {
        paths[i] = malloc(256);
        int depth = i % 4;
        if (depth == 0)
            snprintf(paths[i], 256, "%s/file_%04d.bin",   BENCH_TMPDIR, i);
        else if (depth == 1)
            snprintf(paths[i], 256, "%s/sub/file_%04d.bin",     BENCH_TMPDIR, i);
        else if (depth == 2)
            snprintf(paths[i], 256, "%s/sub/deep/file_%04d.bin", BENCH_TMPDIR, i);
        else
            snprintf(paths[i], 256, "%s/alt/file_%04d.bin", BENCH_TMPDIR, i);

        /* ensure parent dir */
        char tmp[256]; strncpy(tmp, paths[i], 255); tmp[255]=0;
        char *sl = strrchr(tmp, '/'); if (sl) { *sl = 0; mkdirp(tmp); }

        size_t sz = (size_t)size_kb * 1024;
        FILE *f = fopen(paths[i], "wb");
        if (!f) { perror(paths[i]); continue; }
        uint8_t *buf = malloc(sz);
        /* mixed: 70% text-like (ASCII 32-126), 30% structured binary */
        for (size_t j = 0; j < sz; j++) {
            if (j % 10 < 7) buf[j] = (uint8_t)(32 + (xrand() % 95));  /* printable */
            else             buf[j] = (uint8_t)(xrand() % 16);          /* low-byte  */
        }
        fwrite(buf, 1, sz, f);
        free(buf); fclose(f);
        total += sz;
    }
    *total_bytes_out = total;
    return paths;
}

/* ══════════════════════════════════════════════════════════════════
 * PHASE 2 — ext4 baseline: sequential read all files
 * ══════════════════════════════════════════════════════════════════ */
static BenchResult bench_ext4_seq(char **paths, int n_files, int size_kb) {
    uint64_t total = 0;
    size_t   sz    = (size_t)size_kb * 1024;
    uint8_t *buf   = malloc(sz);
    /* warm cache */
    for (int i = 0; i < n_files && i < 4; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (f) { size_t r = fread(buf, 1, sz, f); (void)r; fclose(f); }
    }
    double t0 = now_sec();
    for (int i = 0; i < n_files; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) continue;
        size_t r = fread(buf, 1, sz, f); (void)r;
        fclose(f);
        total += sz;
    }
    double elapsed = now_sec() - t0;
    free(buf);
    return (BenchResult){ "ext4 seq-read (fopen+fread)", elapsed, total, (uint64_t)n_files };
}

/* ══════════════════════════════════════════════════════════════════
 * PHASE 3 — ext4 baseline: random file open
 * ══════════════════════════════════════════════════════════════════ */
static BenchResult bench_ext4_rand(char **paths, int n_files, int iters) {
    uint8_t buf[3];
    double t0 = now_sec();
    for (int i = 0; i < iters; i++) {
        int idx = (int)(xrand() % (uint32_t)n_files);
        int fd  = open(paths[idx], O_RDONLY);
        if (fd < 0) continue;
        int off = (int)(xrand() % 1024) * 3;
        lseek(fd, off, SEEK_SET);
        ssize_t r = read(fd, buf, 3); (void)r;
        close(fd);
    }
    return (BenchResult){ "ext4 rand-seek (open+lseek+read)", now_sec()-t0, (uint64_t)iters*3, (uint64_t)iters };
}

/* ══════════════════════════════════════════════════════════════════
 * PHASE 4 — pack files → .geovault
 * ══════════════════════════════════════════════════════════════════ */
static BenchResult bench_pack(uint64_t total_bytes, int n_files) {
    double t0 = now_sec();
    /* use CLI pack logic via gv_write directly */
    char **rel_paths = malloc((size_t)n_files * sizeof(char*));
    uint8_t **datas  = malloc((size_t)n_files * sizeof(uint8_t*));
    uint32_t *sizes  = malloc((size_t)n_files * sizeof(uint32_t));
    uint32_t n = 0;

    /* walk BENCH_TMPDIR and collect */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "find %s -type f | sort", BENCH_TMPDIR);
    FILE *fp = popen(cmd, "r");
    char line[512];
    while (fgets(line, sizeof(line), fp) && n < (uint32_t)n_files) {
        line[strcspn(line, "\n")] = 0;
        FILE *f = fopen(line, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); uint32_t sz = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET);
        datas[n]  = malloc(sz);
        size_t r  = fread(datas[n], 1, sz, f); (void)r; fclose(f);
        sizes[n]  = sz;
        /* rel path: strip BENCH_TMPDIR + '/' */
        rel_paths[n] = strdup(line + strlen(BENCH_TMPDIR) + 1);
        n++;
    }
    pclose(fp);

    /* build abs paths array (gv_write reads files itself) */
    const char **abs_paths = malloc(n * sizeof(char*));
    for (uint32_t i = 0; i < n; i++) {
        char *p = malloc(512);
        snprintf(p, 512, "%s/%s", BENCH_TMPDIR, rel_paths[i]);
        abs_paths[i] = p;
    }

    double t1 = now_sec();
    gv_write(abs_paths, (const char**)rel_paths, n, BENCH_VAULT);
    double elapsed = now_sec() - t1;

    for (uint32_t i = 0; i < n; i++) free((char*)abs_paths[i]);
    free(abs_paths);

    for (uint32_t i = 0; i < n; i++) { free(datas[i]); free(rel_paths[i]); }
    free(rel_paths); free(datas); free(sizes);
    (void)t0;
    return (BenchResult){ "geovault pack (gv_write)", elapsed, total_bytes, n };
}

/* ══════════════════════════════════════════════════════════════════
 * PHASE 5 — GeoVault sequential read (gv_extract all)
 * ══════════════════════════════════════════════════════════════════ */
static BenchResult bench_gv_seq(void) {
    GeoVaultCtx *ctx = gv_open(BENCH_VAULT);
    if (!ctx) return (BenchResult){ "geovault seq-read", 0,0,0 };

    uint64_t total = 0;
    double t0 = now_sec();
    gv_load_frames(ctx);
    for (uint32_t i = 0; i < ctx->hdr.n_files; i++) {
        uint8_t *data = gv_extract(ctx, i);
        total += ctx->entries[i].raw_size;
        free(data);
    }
    double elapsed = now_sec() - t0;
    uint64_t n_files_saved = ctx->hdr.n_files;
    gv_close(ctx);
    return (BenchResult){ "geovault seq-read (gv_extract)", elapsed, total, n_files_saved };
}

/* ══════════════════════════════════════════════════════════════════
 * PHASE 6 — GeoVault random slot seek (O(1) geometric seek)
 * ══════════════════════════════════════════════════════════════════ */
static BenchResult bench_gv_rand(int iters) {
    GeoVaultCtx *ctx = gv_open(BENCH_VAULT);
    if (!ctx) return (BenchResult){ "geovault rand-seek", 0,0,0 };
    gv_load_frames(ctx);

    uint32_t n_files = ctx->hdr.n_files;
    uint8_t chunk[GV_CHUNK_BYTES];
    uint64_t total = 0;

    double t0 = now_sec();
    for (int i = 0; i < iters; i++) {
        uint32_t fidx      = xrand() % n_files;
        GeoVaultFileEntry *e = &ctx->entries[fidx];
        if (e->slot_count == 0) continue;
        uint32_t chunk_idx = xrand() % e->slot_count;
        uint32_t global    = e->frame_start * GV_RESIDUAL + e->slot_start + chunk_idx;
        uint32_t frame     = global / GV_RESIDUAL;
        uint32_t slot_inf  = global % GV_RESIDUAL;
        uint8_t *fbuf = gv_get_frame(ctx, frame);
        if (!fbuf) continue;
        uint32_t rem = e->raw_size - chunk_idx * GV_CHUNK_BYTES;
        if (rem > GV_CHUNK_BYTES) rem = GV_CHUNK_BYTES;
        gv_unpack_chunk(fbuf, slot_inf, chunk, rem);
        total += rem;
    }
    double elapsed = now_sec() - t0;
    gv_close(ctx);
    return (BenchResult){ "geovault rand-seek (slot lookup)", elapsed, total, (uint64_t)iters };
}

/* ══════════════════════════════════════════════════════════════════
 * PHASE 7 — Hash lookup benchmark (gv_find × iters)
 * ══════════════════════════════════════════════════════════════════ */
static BenchResult bench_gv_hash(int iters) {
    GeoVaultCtx *ctx = gv_open(BENCH_VAULT);
    if (!ctx) return (BenchResult){ "geovault hash lookup", 0,0,0 };

    uint32_t n = ctx->hdr.n_files;
    /* preload all paths for random query */
    char **qpaths = malloc(n * sizeof(char*));
    for (uint32_t i = 0; i < n; i++) qpaths[i] = ctx->paths[i];

    double t0 = now_sec();
    int hits = 0;
    for (int i = 0; i < iters; i++) {
        uint32_t qi = xrand() % n;
        int r = gv_find(ctx, qpaths[qi]);
        if (r >= 0) hits++;
    }
    double elapsed = now_sec() - t0;
    (void)hits;
    free(qpaths);
    gv_close(ctx);
    return (BenchResult){ "geovault hash lookup (gv_find)", elapsed, 0, (uint64_t)iters };
}

/* ══════════════════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    int n_files  = BENCH_FILES_DEFAULT;
    int size_kb  = BENCH_SIZE_KB_DEFAULT;
    int iters    = BENCH_ITERS_DEFAULT;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i],"--files") && i+1<argc) n_files = atoi(argv[++i]);
        if (!strcmp(argv[i],"--size")  && i+1<argc) size_kb = atoi(argv[++i]);
        if (!strcmp(argv[i],"--iters") && i+1<argc) iters   = atoi(argv[++i]);
    }

    printf("\n");
    printf("  GeoVault Benchmark\n");
    printf("  files=%d  size_per_file=%dKB  rand_iters=%d\n\n",
           n_files, size_kb, iters);

    /* --- setup --- */
    rm_rf(BENCH_TMPDIR);
    rm_rf(BENCH_VAULT);
    uint64_t total_bytes = 0;
    char **paths = gen_files(n_files, size_kb, &total_bytes);
    printf("  Setup: %d files  total=%.2f MB\n\n",
           n_files, (double)total_bytes / (1024.0*1024.0));

    /* --- ext4 baseline --- */
    printf("  [Baseline: ext4 / native filesystem]\n");
    sep();
    print_result(bench_ext4_seq(paths, n_files, size_kb));
    print_result(bench_ext4_rand(paths, n_files, iters));
    printf("\n");

    /* --- geovault --- */
    printf("  [GeoVault]\n");
    sep();
    print_result(bench_pack(total_bytes, n_files));
    print_result(bench_gv_seq());
    print_result(bench_gv_rand(iters));
    print_result(bench_gv_hash(iters * 10));
    printf("\n");

    /* --- vault stats --- */
    GeoVaultCtx *ctx = gv_open(BENCH_VAULT);
    if (ctx) {
        struct stat st; stat(BENCH_VAULT, &st);
        double ratio = (double)ctx->hdr.total_raw / (double)ctx->hdr.compressed_sz;
        printf("  [Vault Stats]\n");
        sep();
        printf("  %-30s  %llu B  →  %lld B  (%.2fx)\n",
               "compression",
               (unsigned long long)ctx->hdr.total_raw,
               (long long)st.st_size, ratio);
        printf("  %-30s  %u\n", "frames", ctx->hdr.n_frames);
        printf("  %-30s  %u\n", "files",  ctx->hdr.n_files);
        gv_close(ctx);
    }
    printf("\n");

    /* cleanup */
    for (int i = 0; i < n_files; i++) free(paths[i]);
    free(paths);
    rm_rf(BENCH_TMPDIR);
    rm_rf(BENCH_VAULT);
    return 0;
}
