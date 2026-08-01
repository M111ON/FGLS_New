/* q4_hist.c — nibble distribution histogram of Q4_0 tensors in a GGUF */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include "beam_addressing/gguf_reader.h"

int main(int argc, char **argv) {
    const char *fin = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q4_0.gguf";
    GGUF_File *gf = gguf_open(fin);
    if (!gf) { printf("[FAIL] open\n"); return 1; }

    FILE *fp = fopen(fin, "rb");
    uint64_t hist[16];
    memset(hist, 0, sizeof(hist));
    uint64_t n_vals = 0;
    int n_q4 = 0;

    for (uint64_t t = 0; t < gf->tensor_count; t++) {
        if (gf->tensors[t].type != 2) continue;  /* 2 = Q4_0 */
        n_q4++;
        uint64_t sz = gf->tensors[t].size_bytes;
        uint64_t blocks = sz / 34;
        uint64_t off = gf->tensor_data_start + gf->tensors[t].offset;
        uint8_t *buf = (uint8_t*)malloc(sz);
        fseek(fp, off, SEEK_SET);
        fread(buf, 1, sz, fp);
        for (uint64_t b = 0; b < blocks; b++) {
            const uint8_t *q = buf + b * 34 + 2;  /* skip 2B scale */
            for (int i = 0; i < 32; i++) {
                hist[q[i] & 0x0F]++;
                hist[q[i] >> 4]++;
                n_vals += 2;
            }
        }
        free(buf);
    }
    fclose(fp);

    printf("Q4_0 tensors: %d, nibbles: %" PRIu64 "\n", n_q4, n_vals);
    printf("  nibble  count        pct     cum%%\n");
    uint64_t cum = 0;
    double entropy = 0;
    for (int a = 0; a < 16; a++) {
        cum += hist[a];
        double p = (double)hist[a] / n_vals;
        if (p > 0) entropy += -p * log2(p);
        printf("  %5d %12" PRIu64 "  %6.2f%%  %6.2f%%\n", a, hist[a], 100.0*p, 100.0*cum/n_vals);
    }
    printf("  entropy: %.4f bits/nibble\n", entropy);
    printf("  theoretical packed size: %.1f MB (vs %.1f MB raw nibbles)\n",
           n_vals * entropy / 8 / 1048576.0, n_vals * 4 / 8 / 1048576.0);
    gguf_close(gf);
    return 0;
}
