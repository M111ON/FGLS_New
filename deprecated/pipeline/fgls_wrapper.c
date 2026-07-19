/*
 * fgls_wrapper.c — Unified User-Friendly CLI
 *
 * Simple interface for non-technical users:
 *   fgls compress <file>           — auto-compress with best route
 *   fgls decompress <file.fgls>    — auto-decompress
 *   fgls info <file>               — show file info
 *   fgls benchmark <file>          — benchmark all routes
 *   fgls version                   — show version
 *
 * Auto-detects file type, chooses best encoding, handles everything.
 *
 * Compile (WSL):
 *   gcc -O2 -std=c11 -Icollection -Icore -Icore/pogls_engine \
 *       -Icollection/src -Irunner -Icollection/dgls/geo/include \
 *       fgls_wrapper.c collection/dgls/geo/src/geo_jump.c -lm -lzstd -o fgls
 *
 * Compile (Windows/MSYS2):
 *   gcc -O2 -std=c11 -Icollection -Icore -Icore/pogls_engine \
 *       -Icollection/src -Irunner -Icollection/dgls/geo/include \
 *       -DWIN32 -D_WIN32 -include windows.h \
 *       fgls_wrapper.c collection/dgls/geo/src/geo_jump.c -lm -lzstd -o fgls.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sid.h"
#include "fgls_profile.h"

/* ═══════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════ */

#define FGLS_VERSION "2.0.0"
#define FGLS_MAGIC   0x46474C53u  /* "FGLS" */
#define CHUNK_SZ     64

/* ═══════════════════════════════════════════════════════════════
   HELP
   ═══════════════════════════════════════════════════════════════ */

static void show_help(void) {
    printf("FGLS Universal Codec v%s\n\n", FGLS_VERSION);
    printf("Usage:\n");
    printf("  fgls compress <input> [output.fgls]   Compress a file\n");
    printf("  fgls decompress <input.fgls> [output]  Decompress a file\n");
    printf("  fgls info <file>                       Show file details\n");
    printf("  fgls benchmark <file>                  Benchmark all routes\n");
    printf("  fgls version                           Show version\n");
    printf("  fgls help                              Show this help\n");
    printf("\nExamples:\n");
    printf("  fgls compress model.bin                → model.bin.fgls\n");
    printf("  fgls decompress model.bin.fgls         → model.bin\n");
    printf("  fgls info model.bin                    → file details\n");
    printf("  fgls benchmark model.bin               → speed comparison\n");
    printf("\nThe encoder automatically selects the best compression route\n");
    printf("for your data. No configuration needed.\n");
}

/* ═══════════════════════════════════════════════════════════════
   FILE I/O HELPERS
   ═══════════════════════════════════════════════════════════════ */

static uint8_t *read_file(const char *path, long *sz) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Error: cannot open '%s'\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    *sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(*sz);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, *sz, f);
    fclose(f);
    return buf;
}

static int write_file(const char *path, const uint8_t *data, long sz) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Error: cannot write '%s'\n", path); return -1; }
    fwrite(data, 1, sz, f);
    fclose(f);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   AUTO-DETECT FILE TYPE
   ═══════════════════════════════════════════════════════════════ */

typedef enum {
    FILE_UNKNOWN,
    FILE_RAW,
    FILE_FGLS_COMPRESSED,
    FILE_TWIDX
} FileType;

static FileType detect_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return FILE_UNKNOWN;

    uint8_t header[16];
    size_t n = fread(header, 1, 16, f);
    fclose(f);

    if (n >= 4) {
        uint32_t magic = *(uint32_t *)header;
        if (magic == FGLS_MAGIC) return FILE_FGLS_COMPRESSED;
        if (magic == SID_TWIDX_MAGIC) return FILE_TWIDX;
    }

    /* Check extension */
    size_t len = strlen(path);
    if (len > 5 && strcmp(path + len - 5, ".fgls") == 0) return FILE_FGLS_COMPRESSED;
    if (len > 6 && strcmp(path + len - 6, ".twidx") == 0) return FILE_TWIDX;

    return FILE_RAW;
}

/* ═══════════════════════════════════════════════════════════════
   COMPRESS — Auto-select best route
   ═══════════════════════════════════════════════════════════════ */

static int do_compress(const char *in_path, const char *out_path) {
    long sz;
    uint8_t *raw = read_file(in_path, &sz);
    if (!raw) return 1;

    /* Auto-generate output path if not provided */
    char default_out[1024];
    if (!out_path) {
        snprintf(default_out, sizeof(default_out), "%s.fgls", in_path);
        out_path = default_out;
    }

    printf("Compressing: %s (%ld bytes)\n", in_path, sz);

    /* Profile the data */
    FglsProfile prof;
    fgls_profile(raw, sz, &prof);

    printf("  Data type: %s\n", fgls_route_name(prof.route));
    printf("  Entropy:   %.2f bits/byte\n", prof.entropy);

    /* Choose best route based on profile */
    FglsRoute route = prof.route;
    if (route == FGLS_ROUTE_RAW) {
        /* For raw data, try ZSTD if available, otherwise keep raw */
        #ifdef FGLS_USE_ZSTD
        route = FGLS_ROUTE_ZSTD;
        #endif
    }

    printf("  Route:     %s\n", fgls_route_name(route));

    /* Encode */
    clock_t t0 = clock();
    long out_sz = 0;
    uint8_t *encoded = fgls_encode(raw, sz, route, &out_sz);
    clock_t t1 = clock();

    if (!encoded) {
        fprintf(stderr, "Error: encoding failed\n");
        free(raw);
        return 1;
    }

    double ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;
    double ratio = (double)out_sz / sz;

    printf("  Output:    %s (%ld bytes, %.2fx)\n", out_path, out_sz, ratio);
    printf("  Speed:     %.1f ms\n", ms);

    if (ratio >= 1.0) {
        printf("  Note:      Data is incompressible (ratio >= 1.0x)\n");
        printf("             Original file kept as-is.\n");
    } else {
        write_file(out_path, encoded, out_sz);
        printf("  Saved:     %.1f%% space reduction\n", (1.0 - ratio) * 100.0);
    }

    free(encoded);
    free(raw);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   DECOMPRESS
   ═══════════════════════════════════════════════════════════════ */

static int do_decompress(const char *in_path, const char *out_path) {
    long sz;
    uint8_t *encoded = read_file(in_path, &sz);
    if (!encoded) return 1;

    /* Auto-generate output path if not provided */
    char default_out[1024];
    if (!out_path) {
        /* Strip .fgls extension */
        size_t len = strlen(in_path);
        if (len > 5 && strcmp(in_path + len - 5, ".fgls") == 0) {
            snprintf(default_out, sizeof(default_out), "%.*s", (int)(len - 5), in_path);
        } else {
            snprintf(default_out, sizeof(default_out), "%s.dec", in_path);
        }
        out_path = default_out;
    }

    printf("Decompressing: %s (%ld bytes)\n", in_path, sz);

    /* Decode */
    clock_t t0 = clock();
    long dec_sz = 0;
    uint8_t *decoded = fgls_decode(encoded, sz, &dec_sz);
    clock_t t1 = clock();

    if (!decoded) {
        fprintf(stderr, "Error: decoding failed (invalid or corrupted file)\n");
        free(encoded);
        return 1;
    }

    double ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;

    printf("  Output:    %s (%ld bytes)\n", out_path, dec_sz);
    printf("  Speed:     %.1f ms\n", ms);

    write_file(out_path, decoded, dec_sz);
    printf("  Done!\n");

    free(decoded);
    free(encoded);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   INFO
   ═══════════════════════════════════════════════════════════════ */

static int do_info(const char *path) {
    long sz;
    uint8_t *data = read_file(path, &sz);
    if (!data) return 1;

    FileType ft = detect_file(path);
    printf("File: %s\n", path);
    printf("Size: %ld bytes\n", sz);

    if (ft == FILE_FGLS_COMPRESSED) {
        printf("Type: FGLS compressed\n");
        /* Parse header */
        if (sz >= 20) {
            uint32_t magic = *(uint32_t *)data;
            uint32_t version = *(uint32_t *)(data + 4);
            uint32_t n_chunks = *(uint32_t *)(data + 8);
            uint32_t orig_sz = *(uint32_t *)(data + 12);
            printf("  Version:   %u\n", version);
            printf("  Chunks:    %u\n", n_chunks);
            printf("  Original:  %u bytes\n", orig_sz);
            printf("  Ratio:     %.2fx\n", (double)sz / orig_sz);
        }
    } else if (ft == FILE_TWIDX) {
        printf("Type: SID coordinates (.twidx)\n");
        SIDStore *s = (SIDStore *)calloc(1, sizeof(SIDStore));
        if (sid_read(path, s) >= 0) {
            printf("  Entries:   %u\n", s->n_entries);
            /* Compute stats */
            uint32_t n_drain = 0;
            for (uint32_t i = 0; i < s->n_entries; i++)
                if (s->entries[i].coord.drain) n_drain++;
            printf("  Drain:     %u (%.1f%%)\n", n_drain,
                   100.0 * n_drain / s->n_entries);
        }
        free(s);
    } else {
        printf("Type: Raw data\n");
        /* Profile it */
        FglsProfile prof;
        fgls_profile(data, sz, &prof);
        printf("  Best route: %s\n", fgls_route_name(prof.route));
        printf("  Entropy:    %.2f bits/byte\n", prof.entropy);
        printf("  Chunks:     %u\n", (uint32_t)(sz / CHUNK_SZ));
    }

    free(data);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   BENCHMARK
   ═══════════════════════════════════════════════════════════════ */

static int do_benchmark(const char *path) {
    long sz;
    uint8_t *raw = read_file(path, &sz);
    if (!raw) return 1;

    printf("Benchmarking: %s (%ld bytes)\n\n", path, sz);

    /* Profile */
    FglsProfile prof;
    fgls_profile(raw, sz, &prof);
    printf("Profile: %s (%.2f bits/byte entropy)\n\n",
           fgls_route_name(prof.route), prof.entropy);

    /* Test each route */
    const char *routes[] = {
        "FLAT", "SPARSE", "HEX", "DELTA", "GRADIENT", "HILBERT", "ZSTD", "RAW"
    };
    int n_routes = 8;

    printf("%-12s %10s %8s %8s\n", "Route", "Output", "Ratio", "Time");
    printf("%-12s %10s %8s %8s\n", "─────────", "──────────", "────────", "────────");

    for (int r = 0; r < n_routes; r++) {
        FglsRoute route = (FglsRoute)r;

        clock_t t0 = clock();
        long out_sz = 0;
        uint8_t *encoded = fgls_encode(raw, sz, route, &out_sz);
        clock_t t1 = clock();

        if (encoded) {
            double ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;
            double ratio = (double)out_sz / sz;
            printf("%-12s %10ld %7.2fx %7.1fms\n",
                   routes[r], out_sz, ratio, ms);
            free(encoded);
        } else {
            printf("%-12s %10s %8s %8s\n", routes[r], "N/A", "N/A", "N/A");
        }
    }

    /* Auto-route */
    clock_t t0 = clock();
    long out_sz = 0;
    uint8_t *encoded = fgls_encode(raw, sz, prof.route, &out_sz);
    clock_t t1 = clock();
    if (encoded) {
        double ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;
        double ratio = (double)out_sz / sz;
        printf("%-12s %10ld %7.2fx %7.1fms  ← AUTO\n",
               "AUTO", out_sz, ratio, ms);
        free(encoded);
    }

    printf("\nRecommendation: Use 'fgls compress %s'\n", path);

    free(raw);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    if (argc < 2) {
        show_help();
        return 0;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        show_help();
        return 0;
    }

    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
        printf("FGLS Universal Codec v%s\n", FGLS_VERSION);
        #ifdef FGLS_USE_ZSTD
        printf("ZSTD support: yes\n");
        #else
        printf("ZSTD support: no\n");
        #endif
        return 0;
    }

    if (strcmp(cmd, "compress") == 0 || strcmp(cmd, "c") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: fgls compress <input> [output.fgls]\n");
            return 1;
        }
        return do_compress(argv[2], argc > 3 ? argv[3] : NULL);
    }

    if (strcmp(cmd, "decompress") == 0 || strcmp(cmd, "d") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: fgls decompress <input.fgls> [output]\n");
            return 1;
        }
        return do_decompress(argv[2], argc > 3 ? argv[3] : NULL);
    }

    if (strcmp(cmd, "info") == 0 || strcmp(cmd, "i") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: fgls info <file>\n");
            return 1;
        }
        return do_info(argv[2]);
    }

    if (strcmp(cmd, "benchmark") == 0 || strcmp(cmd, "bench") == 0 || strcmp(cmd, "b") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: fgls benchmark <file>\n");
            return 1;
        }
        return do_benchmark(argv[2]);
    }

    fprintf(stderr, "Unknown command: %s\n\n", cmd);
    show_help();
    return 1;
}
