/*
 * zstd_ratio.c — measure zstd compression ratio of a file WITHOUT writing output
 *
 * Streams input through zstd in chunks (ZSTD_compressStream), counts bytes.
 *
 * Compile:
 *   gcc -Wall -Werror -Wno-error=unused-function -O2 -std=c11
 *     -Irunner/explore/zstd_inc runner/explore/zstd_ratio.c
 *     C:/mingw64/lib/libzstd.a -lssp -o runner/explore/zstd_ratio.exe
 * Run:
 *   runner/explore/zstd_ratio.exe file.gguf [level]
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <zstd.h>

#define CHUNK 1u << 20

int main(int argc, char **argv) {
    const char *fin = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";
    int lvl = (argc > 2) ? atoi(argv[2]) : 3;

    FILE *fp = fopen(fin, "rb");
    if (!fp) { printf("[FAIL] open\n"); return 1; }
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    (void)fsz;

    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, lvl);

    uint8_t *in = (uint8_t*)malloc(CHUNK);
    uint8_t *out = (uint8_t*)malloc(ZSTD_compressBound(CHUNK));

    uint64_t total_in = 0, total_out = 0;
    ZSTD_inBuffer ib = { in, 0, 0 };
    ZSTD_outBuffer ob = { out, ZSTD_compressBound(CHUNK), 0 };

    size_t r;
    while ((ib.size = fread(in, 1, CHUNK, fp)) > 0) {
        ib.pos = 0;
        ob.pos = 0;
        while (ib.pos < ib.size) {
            ob.size = ZSTD_compressBound(CHUNK);
            ob.pos = 0;
            r = ZSTD_compressStream2(cctx, &ob, &ib, ZSTD_e_continue);
            if (ZSTD_isError(r)) { printf("[FAIL] %s\n", ZSTD_getErrorName(r)); return 1; }
            total_out += ob.pos;
        }
        total_in += ib.size;
    }
    /* flush */
    ZSTD_inBuffer empty = { NULL, 0, 0 };
    do {
        ob.size = ZSTD_compressBound(CHUNK);
        ob.pos = 0;
        r = ZSTD_compressStream2(cctx, &ob, &empty, ZSTD_e_end);
        if (ZSTD_isError(r)) { printf("[FAIL] %s\n", ZSTD_getErrorName(r)); return 1; }
        total_out += ob.pos;
    } while (r != 0);

    printf("%s | level %d: %" PRIu64 " -> %" PRIu64 " bytes (%.4fx, %.1f%% saved)\n",
           fin, lvl, total_in, total_out,
           (double)total_out / total_in, 100.0 * (1.0 - (double)total_out / total_in));

    fclose(fp);
    free(in); free(out);
    ZSTD_freeCCtx(cctx);
    return 0;
}
