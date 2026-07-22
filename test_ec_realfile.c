/*
 * test_ec_realfile.c — Entropy Container: test with real files
 * ═══════════════════════════════════════════════════════════════
 * Reads a real file, chunks it into 48B blocks, stores via RDH,
 * loads back, verifies roundtrip. Reports coverage & stats.
 *
 * Build:
 *   gcc -O2 -std=c11 -Icore -Icollection/rdh -Icollection/geopixel/geopixel \
 *       -Icollection -Icollection/include -Icollection/dgls/geo/include \
 *       -DP5H_ENABLE test_ec_realfile.c -o test_ec_realfile.exe -lm
 *
 * Run:
 *   ./test_ec_realfile.exe pipeline/fgls_cli.c
 *   ./test_ec_realfile.exe collection/rdh/rdh_capture.h
 *   ./test_ec_realfile.exe (default: self-test)
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "entropy_container.h"
#include "fibo_tick.h"

static inline uint64_t ns_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ── Load entire file into memory ──────────────────────────── */
static uint8_t *load_file(const char *path, size_t *out_sz) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *out_sz = n;
    return buf;
}

int main(int argc, char **argv) {
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  RDH Entropy Container — Real File Test             ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    /* ── Load file ── */
    const char *path = (argc > 1) ? argv[1] : NULL;
    uint8_t *data = NULL;
    size_t file_sz = 0;

    if (path) {
        data = load_file(path, &file_sz);
        if (!data) {
            fprintf(stderr, "ERROR: cannot open %s\n", path);
            return 1;
        }
        printf("File: %s (%zu bytes)\n", path, file_sz);
    } else {
        /* Self-test: generate 10KB of mixed data */
        file_sz = 10240;
        data = (uint8_t *)malloc(file_sz);
        srand(42);
        /* Structured: first 2KB (repeating pattern) */
        for (size_t i = 0; i < 2048; i++)
            data[i] = (uint8_t)((i % 17) * 3 + 7);
        /* Random: next 4KB */
        for (size_t i = 2048; i < 6144; i++)
            data[i] = (uint8_t)(rand() & 0xFF);
        /* Mixed: last 4KB (semi-structured) */
        for (size_t i = 6144; i < file_sz; i++)
            data[i] = (uint8_t)((i * 13 + 97) & 0xFF);
        printf("Self-test: 10KB mixed data (2KB structured + 4KB random + 4KB mixed)\n");
    }

    /* ── Chunk and store ── */
    EntropyContainer ec;
    ec_init(&ec);
    RDHConfig cfg = RDH_CAPTURE_144;

    uint32_t block_sz = EC_BLOCK_SZ;
    uint32_t n_blocks = (uint32_t)((file_sz + block_sz - 1) / block_sz);

    /* Store all blocks, record enc */
    uint16_t *encs = (uint16_t *)malloc(n_blocks * sizeof(uint16_t));
    uint32_t *block_lens = (uint32_t *)malloc(n_blocks * sizeof(uint32_t));

    uint64_t t0 = ns_now();
    for (uint32_t i = 0; i < n_blocks; i++) {
        size_t off = i * block_sz;
        size_t len = (off + block_sz <= file_sz) ? block_sz : (file_sz - off);
        block_lens[i] = (uint32_t)len;

        /* Compute enc before store */
        uint64_t fk = rdh_capture(data + off, len, &cfg);
        encs[i] = (uint16_t)(fk % FRAME_CYCLE);

        ec_store(&ec, data + off, len, &cfg);
    }
    uint64_t t1 = ns_now();
    double store_ns = (double)(t1 - t0);

    printf("\n─── Store ───\n");
    printf("  Blocks: %u (× %u bytes)\n", n_blocks, block_sz);
    printf("  Time: %.1f µs total, %.1f ns/block\n",
           store_ns / 1000.0, store_ns / n_blocks);
    printf("  Throughput: %.1f MB/s\n",
           (double)file_sz / (store_ns / 1e9) / 1e6);

    /* ── Load back and verify ── */
    uint8_t *reconstructed = (uint8_t *)calloc(file_sz, 1);
    uint32_t match_count = 0;
    uint32_t mismatch_count = 0;
    uint32_t not_found = 0;

    t0 = ns_now();
    for (uint32_t i = 0; i < n_blocks; i++) {
        size_t off = i * block_sz;
        size_t len = block_lens[i];

        /* Load back using flat_key (direct O(1)) */
        uint8_t block[EC_BLOCK_SZ];
        uint64_t fk = rdh_capture(data + off, len, &cfg);
        int n = ec_load_by_flat_key(&ec, fk, block, EC_BLOCK_SZ);

        if (n <= 0) {
            not_found++;
            continue;
        }

        if (n >= (int)len && memcmp(block, data + off, len) == 0) {
            match_count++;
            memcpy(reconstructed + off, block, len);
        } else {
            mismatch_count++;
        }
    }
    t1 = ns_now();
    double load_ns = (double)(t1 - t0);

    printf("\n─── Load & Verify ───\n");
    printf("  Blocks loaded: %u\n", n_blocks - not_found);
    printf("  Match: %u / %u (%.1f%%)\n",
           match_count, n_blocks, 100.0 * match_count / n_blocks);
    printf("  Mismatch: %u\n", mismatch_count);
    printf("  Not found: %u\n", not_found);
    printf("  Time: %.1f µs total, %.1f ns/block\n",
           load_ns / 1000.0, load_ns / n_blocks);

    /* ── Verify full file roundtrip ── */
    int full_match = (memcmp(data, reconstructed, file_sz) == 0);
    printf("\n─── Full File Roundtrip ───\n");
    printf("  File size: %zu bytes\n", file_sz);
    printf("  Full match: %s\n", full_match ? "YES ✓" : "NO ✗ (collisions)");

    /* ── Entropy analysis ── */
    uint32_t entropy_counts[4] = {0};
    for (uint32_t i = 0; i < n_blocks; i++) {
        size_t off = i * block_sz;
        size_t len = block_lens[i];
        /* Simple entropy class based on unique bytes */
        uint8_t hist[256] = {0};
        uint32_t uniq = 0;
        for (size_t j = 0; j < len; j++) {
            if (hist[data[off + j]] == 0) uniq++;
            hist[data[off + j]]++;
        }
        uint8_t cls;
        if (uniq <= 4) cls = 0;       /* structured */
        else if (uniq <= 16) cls = 1;  /* moderate */
        else if (uniq <= 40) cls = 2;  /* high */
        else cls = 3;                  /* random */
        entropy_counts[cls]++;
    }

    printf("\n─── Entropy Distribution ───\n");
    printf("  Structured (0): %u (%.1f%%)\n",
           entropy_counts[0], 100.0 * entropy_counts[0] / n_blocks);
    printf("  Moderate   (1): %u (%.1f%%)\n",
           entropy_counts[1], 100.0 * entropy_counts[1] / n_blocks);
    printf("  High       (2): %u (%.1f%%)\n",
           entropy_counts[2], 100.0 * entropy_counts[2] / n_blocks);
    printf("  Random     (3): %u (%.1f%%)\n",
           entropy_counts[3], 100.0 * entropy_counts[3] / n_blocks);

    /* ── Address distribution ── */
    uint32_t enc_used = 0;
    uint8_t enc_seen[FRAME_CYCLE] = {0};
    for (uint32_t i = 0; i < n_blocks; i++) {
        if (!enc_seen[encs[i]]) {
            enc_seen[encs[i]] = 1;
            enc_used++;
        }
    }

    printf("\n─── Address Distribution ───\n");
    printf("  Unique enc used: %u / %u (%.1f%%)\n",
           enc_used, FRAME_CYCLE, 100.0 * enc_used / FRAME_CYCLE);
    printf("  Collision rate: %.1f%%\n",
           100.0 * ec.overwrites / ec.total_stored);

    /* ── Container stats ── */
    uint32_t occ, ovw, st, ld;
    ec_stats(&ec, &occ, &ovw, &st, &ld);
    printf("\n─── Container Stats ───\n");
    printf("  Occupied slots: %u / %u (%.1f%%)\n",
           occ, EC_SLOTS, 100.0 * occ / EC_SLOTS);
    printf("  Overwrites: %u\n", ovw);
    printf("  Total stored: %u\n", st);

    /* ── fibo_tick distribution ── */
    uint32_t action_counts[4] = {0};
    for (uint32_t i = 0; i < n_blocks; i++) {
        uint8_t action = ft_store_action(encs[i]);
        action_counts[action]++;
    }
    printf("\n─── Fibo Tick Distribution ───\n");
    printf("  FREEZE (tick 0): %u (%.1f%%)\n",
           action_counts[0], 100.0 * action_counts[0] / n_blocks);
    printf("  MAIN   (tick 1): %u (%.1f%%)\n",
           action_counts[1], 100.0 * action_counts[1] / n_blocks);
    printf("  PIPE   (tick2-10): %u (%.1f%%)\n",
           action_counts[2], 100.0 * action_counts[2] / n_blocks);
    printf("  BRIDGE (tick 11): %u (%.1f%%)\n",
           action_counts[3], 100.0 * action_counts[3] / n_blocks);

    /* ── Summary ── */
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  SUMMARY\n");
    printf("  File: %zu bytes, %u blocks\n", file_sz, n_blocks);
    printf("  Roundtrip: %u/%u match (%.1f%%)\n",
           match_count, n_blocks, 100.0 * match_count / n_blocks);
    printf("  Store: %.1f MB/s, Load: %.1f MB/s\n",
           (double)file_sz / (store_ns / 1e9) / 1e6,
           (double)file_sz / (load_ns / 1e9) / 1e6);
    printf("  Address space: %u/%u enc used (%.1f%%)\n",
           enc_used, FRAME_CYCLE, 100.0 * enc_used / FRAME_CYCLE);
    printf("═══════════════════════════════════════════════════════\n");

    free(data);
    free(reconstructed);
    free(encs);
    free(block_lens);
    return 0;
}
