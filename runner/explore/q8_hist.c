/* q8_hist.c — int8 weight distribution histogram of a GGUF (Q8_0 blocks) */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "beam_addressing/gguf_reader.h"

int main(int argc, char **argv) {
    const char *fin = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";
    GGUF_File *gf = gguf_open(fin);
    if (!gf) { printf("[FAIL] open\n"); return 1; }

    FILE *fp = fopen(fin, "rb");
    uint64_t hist[128];  /* bins: |w| 0..126, 127+ */
    memset(hist, 0, sizeof(hist));
    uint64_t n_blocks = 0, n_vals = 0;
    int n_q8 = 0;

    for (uint64_t t = 0; t < gf->tensor_count; t++) {
        if (gf->tensors[t].type != 8) continue;
        n_q8++;
        uint64_t sz = gf->tensors[t].size_bytes;
        uint64_t blocks = sz / 34;
        uint64_t off = gf->tensor_data_start + gf->tensors[t].offset;
        uint8_t *buf = (uint8_t*)malloc(sz);
        fseek(fp, off, SEEK_SET);
        fread(buf, 1, sz, fp);
        n_blocks += blocks;
        for (uint64_t b = 0; b < blocks; b++) {
            const int8_t *w = (const int8_t*)(buf + b * 34 + 2);
            for (int i = 0; i < 32; i++) {
                int v = w[i];
                int a = (v < 0) ? -v : v;
                if (a > 127) a = 127;
                hist[a]++;
                n_vals++;
            }
        }
        free(buf);
    }
    fclose(fp);

    printf("Q8_0 tensors: %d, blocks: %" PRIu64 ", values: %" PRIu64 "\n", n_q8, n_blocks, n_vals);
    printf("  |w|     count        pct     cum%%\n");
    uint64_t cum = 0;
    for (int a = 0; a <= 40; a++) {
        cum += hist[a];
        printf("  %4d %12" PRIu64 "  %6.2f%%  %6.2f%%\n", a, hist[a],
               100.0*hist[a]/n_vals, 100.0*cum/n_vals);
    }
    uint64_t rest = 0;
    for (int a = 41; a < 128; a++) rest += hist[a];
    printf("  >40  %12" PRIu64 "  %6.2f%%\n", rest, 100.0*rest/n_vals);
    gguf_close(gf);
    return 0;
}
