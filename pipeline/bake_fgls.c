/*
 * bake_fgls.c — Standalone GGUF → .fgls bake tool
 *
 * Uses fgls_tensor_archive.h (zstd level 9) for compression.
 * Roundtrip: bake → extract → compare bytes.
 *
 * Build:
 *   gcc -O2 -std=c11 -fno-strict-aliasing -DFGLS_USE_ZSTD \
 *       -Icore -Irunner -IC:/msys64/mingw64/include \
 *       -LC:/msys64/mingw64/lib -lzstd -lm \
 *       -o bake_fgls.exe pipeline/bake_fgls.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "gguf_reader.h"
#include "fgls_tensor_archive.h"

/* Read callback: read raw bytes from original GGUF at virtual offset */
typedef struct {
    const char *path;
    FILE *f;
} SrcIO;

static int src_read(void *ud, uint64_t v_offset, uint8_t *out, uint64_t size) {
    SrcIO *s = (SrcIO *)ud;
    if (!s->f) {
        s->f = fopen(s->path, "rb");
        if (!s->f) return -1;
    }
    if (_fseeki64(s->f, (long long)v_offset, SEEK_SET) != 0) return -2;
    if (fread(out, 1, (size_t)size, s->f) != (size_t)size) return -3;
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <input.gguf> <output.fgls>\n", prog);
    fprintf(stderr, "  Bake a GGUF model into .fgls archive (zstd level 9).\n");
    fprintf(stderr, "  Lossless roundtrip verified automatically.\n");
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 1; }

    const char *in_path = argv[1];
    const char *out_path = argv[2];

    printf("FGLS Bake: %s → %s\n", in_path, out_path);

    /* Step 1: Parse GGUF */
    GgufReader gf;
    if (gguf_open(in_path, &gf) != 0) {
        fprintf(stderr, "Error: failed to parse GGUF %s\n", in_path);
        return 1;
    }
    printf("Parsed GGUF: %u tensors\n", gf.n_tensors);
    printf("Data section offset: %llu\n", (unsigned long long)gf.data_offset);

    /* fglt_bake wants ABSOLUTE virtual offsets. gguf_reader gives offsets
     * relative to the data section, so add gf.data_offset. */
    uint64_t data_offset = gf.data_offset;
    for (uint32_t i = 0; i < gf.n_tensors; i++) {
        gf.offsets[i] += data_offset;
    }

    /* End of last tensor, aligned */
    for (uint32_t i = 0; i < gf.n_tensors; i++) {
        uint64_t end = gf.offsets[i] + (uint64_t)gf.sizes[i];
        if (end > data_offset) data_offset = end;
    }

    printf("Data offset: %llu\n", (unsigned long long)data_offset);

    /* Convert uint32_t sizes → uint64_t for fglt_bake */
    uint64_t *raw_sizes = (uint64_t *)calloc(gf.n_tensors, sizeof(uint64_t));
    uint8_t  *types     = (uint8_t  *)calloc(gf.n_tensors, 1);
    for (uint32_t i = 0; i < gf.n_tensors; i++) {
        raw_sizes[i] = (uint64_t)gf.sizes[i];
    }

    /* Step 2: Bake */
    SrcIO src = { in_path, NULL };
    FGLT_BakeIO bake_io = { 0 };
    bake_io.read_tensor_src = src_read;
    bake_io.ud = &src;

    clock_t t0 = clock();

    int rc = fglt_bake(out_path,
                       (const char *const *)gf.names,
                       types,
                       gf.offsets,
                       raw_sizes,
                       gf.n_tensors,
                       data_offset,
                       &bake_io);

    if (src.f) fclose(src.f);
    free(raw_sizes);
    free(types);

    if (rc != 0) {
        fprintf(stderr, "Error: fglt_bake returned %d\n", rc);
        return 1;
    }

    clock_t t1 = clock();
    double bake_sec = (double)(t1 - t0) / CLOCKS_PER_SEC;

    /* Get file sizes */
    FILE *fin = fopen(in_path, "rb");
    fseek(fin, 0, SEEK_END);
    long in_size = ftell(fin);
    fclose(fin);

    FILE *fout = fopen(out_path, "rb");
    fseek(fout, 0, SEEK_END);
    long out_size = ftell(fout);
    fclose(fout);

    printf("\n=== BAKE COMPLETE ===\n");
    printf("Input:  %s (%.2f MB)\n", in_path, in_size / 1048576.0);
    printf("Output: %s (%.2f MB)\n", out_path, out_size / 1048576.0);
    printf("Ratio:  %.2fx (%.1f%%)\n",
           (double)in_size / (double)out_size,
           100.0 * (double)out_size / (double)in_size);
    printf("Time:   %.2f seconds\n", bake_sec);

    /* Step 3: Verify roundtrip — open archive, decode each tensor, compare */
    printf("\n=== VERIFY ROUNDTRIP ===\n");
    FGLT_Archive arch;
    if (fglt_open(out_path, &arch) != 0) {
        fprintf(stderr, "Error: fglt_open failed\n");
        return 1;
    }

    int pass = 0, fail = 0;
    uint64_t total_bytes = 0;

    FILE *fsrc = fopen(in_path, "rb");

    for (uint64_t i = 0; i < arch.n; i++) {
        const FGLT_IndexEntry *e = &arch.idx[i];
        if (e->raw_size == 0) continue;

        uint8_t *decoded = (uint8_t *)malloc((size_t)e->raw_size);
        if (!decoded) { fail++; continue; }

        size_t got = fglt_callback(&arch, decoded, e->v_offset, (size_t)e->raw_size);
        if (got != (size_t)e->raw_size) {
            free(decoded); fail++;
            continue;
        }

        uint8_t *original = (uint8_t *)malloc((size_t)e->raw_size);
        if (!original) { free(decoded); fail++; continue; }

        if (_fseeki64(fsrc, (long long)e->v_offset, SEEK_SET) != 0 ||
            fread(original, 1, (size_t)e->raw_size, fsrc) != (size_t)e->raw_size) {
            free(decoded); free(original); fail++;
            continue;
        }

        if (memcmp(decoded, original, (size_t)e->raw_size) == 0) {
            pass++;
        } else {
            fail++;
            fprintf(stderr, "  MISMATCH: tensor %llu '%s'\n", (unsigned long long)i, e->name);
        }

        total_bytes += e->raw_size;
        free(decoded);
        free(original);
    }

    fclose(fsrc);
    fglt_close(&arch);

    printf("Tensors: %d PASS / %d FAIL\n", pass, fail);
    printf("Total data verified: %.2f MB\n", total_bytes / 1048576.0);
    printf("RESULT: %s\n", fail == 0 ? "PASS ✓" : "FAIL ✗");

    return fail == 0 ? 0 : 1;
}
