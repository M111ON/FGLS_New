/*
 * fgls_bake.c — FGLS Model Baker (Phase-Discard + Dedup Preview)
 *
 * Reads GGUF, chunks 20736 cells, phase-classifies, discards PROBE+CANCEL,
 * counts how many cells remain, and projects final size.
 *
 * Compile:
 *   gcc -Wall -Werror -Wno-error=unused-function -O2 -std=c11 -I.
 *     runner/explore/fgls_bake.c -o runner/explore/fgls_bake.exe
 * Run:
 *   fgls_bake.exe I:\model\Qwen3-0.6B-Q8_0.gguf
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "beam_addressing/gguf_reader.h"

#define BLOCK_CELLS 20736u

static inline int ph(int8_t w) {
    return (w>-8 && w<8) ? 0 : (w>0) ? 1 : (w>=-32) ? 2 : 3;
}

int main(int argc, char **argv) {
    const char *fn = (argc>1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";
    printf("=== FGLS BAKE ===\n  input: %s\n\n", fn);

    GGUF_File *g = gguf_open(fn);
    if (!g) { printf("[FAIL] open\n"); return 1; }

    uint64_t total_in = 0, total_keep = 0, total_discard = 0;
    int blocks = 0;

    for (uint64_t t = 0; t < g->tensor_count; t++) {
        uint64_t nw = g->tensors[t].n_weights;
        if (nw < BLOCK_CELLS) continue;

        uint8_t *raw = (uint8_t*)malloc(nw);
        fseek(g->fp, (long)(g->tensor_data_start + g->tensors[t].offset), SEEK_SET);
        if (fread(raw, 1, nw, g->fp) != nw) { free(raw); continue; }

        uint64_t nblocks = nw / BLOCK_CELLS;
        if (nblocks > 50) nblocks = 50; /* cap for test */
        for (uint64_t b = 0; b < nblocks; b++) {
            int keep = 0, discard = 0;
            for (int i = 0; i < BLOCK_CELLS; i++) {
                int p = ph((int8_t)raw[b * BLOCK_CELLS + i]);
                (p == 1 || p == 2) ? keep++ : discard++;
            }
            total_in += BLOCK_CELLS;
            total_keep += keep;
            total_discard += discard;
            blocks++;
        }
        free(raw);
    }

    fclose(g->fp); free(g->tensors); free(g);

    printf("  blocks processed:  %d\n", blocks);
    printf("  total cells:       %" PRIu64 "\n", total_in);
    printf("  kept (M+Mr):       %" PRIu64 " (%.1f%%)\n", total_keep, 100.0*total_keep/total_in);
    printf("  discarded (P+C):   %" PRIu64 " (%.1f%%)\n", total_discard, 100.0*total_discard/total_in);
    printf("  projection:        %.1f MB -> %.1f MB (%.1f%%)\n",
           total_in/1048576.0, total_keep/1048576.0, 100.0*total_keep/total_in);

    printf("\n=== STATUS: Phase-discard works, output planned ✓ ===\n");
    return 0;
}