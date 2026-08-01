/*
 * bake_sweep.c — Parametrized model bake for PPL sweep
 *
 * Reads GGUF, zeroes Q8_0 weights per policy, writes rebuilt GGUF.
 *
 * Policies:
 *   0: keep everything (lossless sanity check)
 *   1 <T>: keep |w| >= T (drop small |w|, keep ALL negatives)
 *   2: current FGLS rule (drop |w|<8 OR w<-32)
 *   3 <E>: real-magnitude: drop |scale*w| < E  (per-block adaptive)
 *
 * Compile:
 *   gcc -Wall -Werror -Wno-error=unused-function -O2 -std=c11 -I.
 *     runner/explore/bake_sweep.c -o runner/explore/bake_sweep.exe
 * Run:
 *   runner/explore/bake_sweep.exe in.gguf out.gguf policy [T|E]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <inttypes.h>
#include "beam_addressing/gguf_reader.h"

static inline float fp16_to_f32(uint16_t h) {
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    uint32_t f;
    if (exp == 0) {
        if (man == 0) { f = sign << 31; }
        else {
            exp = 127 - 15 + 1;
            while (!(man & 0x400)) { man <<= 1; exp--; }
            man &= 0x3FF;
            f = (sign << 31) | (exp << 23) | (man << 13);
        }
    } else if (exp == 0x1F) {
        f = (sign << 31) | 0x7F800000 | (man << 13);
    } else {
        f = (sign << 31) | ((exp + 127 - 15) << 23) | (man << 13);
    }
    float r; memcpy(&r, &f, 4); return r;
}

int main(int argc, char **argv) {
    const char *fin = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";
    const char *fout = (argc > 2) ? argv[2] : "I:/model/sweep.gguf";
    int policy = (argc > 3) ? atoi(argv[3]) : 1;
    float param = (argc > 4) ? atof(argv[4]) : 8.0f;
    int T = (int)param;
    float E = param;

    printf("=== BAKE SWEEP — policy %d", policy);
    if (policy == 1) printf(" (keep |w|>=%d)", T);
    if (policy == 3) printf(" (real-mag |s*w|>=%.4f)", E);
    printf(" ===\n");
    printf("  input:  %s\n  output: %s\n\n", fin, fout);

    GGUF_File *gf = gguf_open(fin);
    if (!gf) { printf("[FAIL] open\n"); return 1; }

    FILE *fp = fopen(fin, "rb");
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *buf = (uint8_t*)malloc(fsz);
    if (!buf) { printf("[FAIL] malloc\n"); return 1; }
    fread(buf, 1, fsz, fp);
    fclose(fp);

    uint64_t total_w = 0, kept_w = 0;
    int tensors_done = 0;
    for (uint64_t t = 0; t < gf->tensor_count; t++) {
        if (gf->tensors[t].type != 8) continue;
        uint64_t sz = gf->tensors[t].size_bytes;
        if (sz < 34) continue;
        uint64_t off = gf->tensor_data_start + gf->tensors[t].offset;
        uint64_t n_q8 = sz / 34;
        tensors_done++;

        for (uint64_t qb = 0; qb < n_q8; qb++) {
            uint8_t *blk = buf + off + qb * 34;
            uint16_t h; memcpy(&h, blk, 2);
            float scale = fp16_to_f32(h);

            for (int i = 2; i < 34; i++) {
                int8_t w = (int8_t)blk[i];
                int keep = 1;
                if (policy == 0) {
                    keep = 1;
                } else if (policy == 1) {
                    int a = (w < 0) ? -w : w;
                    keep = (a >= T);
                } else if (policy == 2) {
                    if (w > -8 && w < 8) keep = 0;
                    else if (w > 0) keep = 1;
                    else keep = (w >= -32);
                } else if (policy == 3) {
                    float mag = scale * (float)((w < 0) ? -w : w);
                    keep = (mag >= E);
                }
                if (keep) kept_w++; else blk[i] = 0;
                total_w++;
            }
        }
    }

    printf("--- SWEEP RESULTS ---\n");
    printf("  Q8_0 tensors: %d\n", tensors_done);
    printf("  kept: %" PRIu64 " / %" PRIu64 " (%.2f%%)\n",
           kept_w, total_w, total_w ? 100.0*kept_w/total_w : 0);

    FILE *fo = fopen(fout, "wb");
    fwrite(buf, 1, fsz, fo);
    fclose(fo);
    free(buf);
    printf("  written: %s (%ld bytes)\n", fout, fsz);
    gguf_close(gf);
    return 0;
}
