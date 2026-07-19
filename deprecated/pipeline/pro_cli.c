/*
 * pro_cli.c — Pro Tier CLI: SID Capture + .twidx
 *
 * Usage:
 *   pro_cli.exe capture <input.bin> [output.twidx]
 *   pro_cli.exe summon <input.twidx> <name>
 *   pro_cli.exe info <input.bin>
 *   pro_cli.exe bench <input.bin>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sid.h"

static int cmd_capture(const char *in_path, const char *out_path) {
    FILE *f = fopen(in_path, "rb");
    if (!f) { fprintf(stderr, "Error: cannot open %s\n", in_path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *raw = (uint8_t *)malloc(sz);
    if (!raw) { fclose(f); return 1; }
    fread(raw, 1, sz, f);
    fclose(f);
    fprintf(stderr, "Input: %s (%ld bytes)\n", in_path, sz);

    SIDStore *s = (SIDStore *)calloc(1, sizeof(SIDStore));
    uint32_t n_chunks = (uint32_t)(sz / 64);
    fprintf(stderr, "Chunks: %u\n", n_chunks);

    clock_t t0 = clock();
    uint32_t n_captured = 0, n_drain = 0;
    for (uint32_t i = 0; i < n_chunks && n_captured < SID_MAX_ENTRIES; i++) {
        SIDCoord coord;
        int rc = sid_capture(raw + i * 64, 64, 1, &coord);
        if (rc == 0) {
            SIDEntry *e = &s->entries[n_captured];
            snprintf(e->name, SID_NAME_MAX, "chunk_%u", i);
            e->coord = coord;
            n_captured++;
            if (coord.drain) n_drain++;
        }
    }
    s->n_entries = n_captured;
    clock_t t1 = clock();
    double elapsed = (double)(t1 - t0) / CLOCKS_PER_SEC;

    fprintf(stderr, "Captured: %u chunks, %u drain (%.1f%%)\n",
            n_captured, n_drain, n_captured ? 100.0 * n_drain / n_captured : 0);
    if (elapsed > 0)
        fprintf(stderr, "Speed: %.0f chunks/sec\n", n_captured / elapsed);

    /* Node distribution */
    uint32_t *nc = (uint32_t *)calloc(20736, sizeof(uint32_t));
    for (uint32_t i = 0; i < n_captured; i++)
        if (s->entries[i].coord.node_id < 20736) nc[s->entries[i].coord.node_id]++;
    uint32_t uniq = 0, maxc = 0;
    for (uint32_t i = 0; i < 20736; i++) {
        if (nc[i] > 0) uniq++;
        if (nc[i] > maxc) maxc = nc[i];
    }
    fprintf(stderr, "Unique nodes: %u/20736 (%.1f%%), max collisions: %u\n",
            uniq, 100.0 * uniq / 20736, maxc);
    free(nc);

    if (out_path) {
        int written = sid_write(out_path, s);
        fprintf(stderr, "Output: %s (%d entries)\n", out_path, written);
    }

    fprintf(stderr, "\nSample entries:\n");
    for (uint32_t i = 0; i < 5 && i < n_captured; i++) {
        SIDEntry *e = &s->entries[i];
        fprintf(stderr, "  [%u] %s → node=%u resid=(%lld,%lld) drain=%d\n",
                i, e->name, e->coord.node_id,
                e->coord.resid_x, e->coord.resid_y, e->coord.drain);
    }

    free(raw);
    free(s);
    return 0;
}

static int cmd_summon(const char *twidx_path, const char *name) {
    SIDStore *s = (SIDStore *)calloc(1, sizeof(SIDStore));
    if (sid_read(twidx_path, s) < 0) {
        fprintf(stderr, "Error: cannot read %s\n", twidx_path);
        free(s);
        return 1;
    }
    fprintf(stderr, "Loaded: %u entries\n", s->n_entries);

    SIDEntry *entry = sid_lookup(s, name);
    if (!entry) {
        fprintf(stderr, "Error: '%s' not found\n", name);
        for (uint32_t i = 0; i < s->n_entries && i < 10; i++)
            fprintf(stderr, "  %s\n", s->entries[i].name);
        free(s);
        return 1;
    }

    int64_t vx, vy;
    sid_summon(&entry->coord, &vx, &vy);
    printf("Tensor: %s\n", entry->name);
    printf("  node_id: %u\n", entry->coord.node_id);
    printf("  resid: (%lld, %lld)\n", entry->coord.resid_x, entry->coord.resid_y);
    printf("  drain: %d\n", entry->coord.drain);
    printf("  summoned: (vx=%lld, vy=%lld)\n", vx, vy);
    free(s);
    return 0;
}

static int cmd_info(const char *in_path) {
    FILE *f = fopen(in_path, "rb");
    if (!f) { fprintf(stderr, "Error: cannot open %s\n", in_path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *raw = (uint8_t *)malloc(sz);
    fread(raw, 1, sz, f);
    fclose(f);

    printf("File: %s (%ld bytes)\n", in_path, sz);
    printf("Chunks: %u (64B each)\n", (uint32_t)(sz / 64));
    if (sz >= 64) {
        SIDCoord coord;
        sid_capture(raw, 64, 1, &coord);
        printf("First chunk SID:\n");
        printf("  node_id: %u\n", coord.node_id);
        printf("  resid: (%lld, %lld)\n", coord.resid_x, coord.resid_y);
        printf("  drain: %d\n", coord.drain);
        printf("  pentagon: %u\n", geo_pentagon_id(coord.node_id));
        printf("  shell_level: %u\n", geo_shell_level(coord.node_id));
        printf("  clock_tick: %u\n", geo_clock_tick(coord.node_id));
    }
    free(raw);
    return 0;
}

static int cmd_bench(const char *in_path) {
    FILE *f = fopen(in_path, "rb");
    if (!f) { fprintf(stderr, "Error: cannot open %s\n", in_path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *raw = (uint8_t *)malloc(sz);
    fread(raw, 1, sz, f);
    fclose(f);

    uint32_t n = (uint32_t)(sz / 64);
    SIDCoord coord;
    for (int i = 0; i < 100; i++)
        sid_capture(raw + (i % n) * 64, 64, 1, &coord);

    clock_t t0 = clock();
    uint32_t iters = 10;
    for (uint32_t it = 0; it < iters; it++)
        for (uint32_t i = 0; i < n; i++)
            sid_capture(raw + i * 64, 64, 1, &coord);
    clock_t t1 = clock();

    double sec = (double)(t1 - t0) / CLOCKS_PER_SEC;
    uint64_t total = (uint64_t)iters * n;
    fprintf(stderr, "Benchmark: %llu captures in %.3f sec\n", total, sec);
    if (sec > 0)
        fprintf(stderr, "  Rate: %.0f captures/sec (%.1f ns/capture)\n",
                total / sec, sec / total * 1e9);

    free(raw);
    return 0;
}

static void usage(const char *p) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s capture <input.bin> [output.twidx]\n", p);
    fprintf(stderr, "  %s summon <input.twidx> <name>\n", p);
    fprintf(stderr, "  %s info <input.bin>\n", p);
    fprintf(stderr, "  %s bench <input.bin>\n", p);
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 1; }
    if (strcmp(argv[1], "capture") == 0)
        return cmd_capture(argv[2], argc > 3 ? argv[3] : NULL);
    if (strcmp(argv[1], "summon") == 0)
        return argc > 3 ? cmd_summon(argv[2], argv[3]) : (usage(argv[0]), 1);
    if (strcmp(argv[1], "info") == 0)
        return cmd_info(argv[2]);
    if (strcmp(argv[1], "bench") == 0)
        return cmd_bench(argv[2]);
    fprintf(stderr, "Unknown: %s\n", argv[1]);
    usage(argv[0]);
    return 1;
}
