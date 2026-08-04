/* probe_kis_ratio.c — ground-truth check: does KIS v4 actually compress real Q8_0?
 * Encodes the 3 largest Q8_0 tensors from a real model, prints enc/raw ratio
 * and delta-statistics of the permutation layer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "beam_addressing/gguf_reader.h"
#include "core/kis_codec_v4.h"

int main(int argc, char **argv) {
    const char *model = (argc > 1) ? argv[1] : "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf";
    GGUF_File *gf = gguf_open(model);
    if (!gf) { printf("cannot open\n"); return 1; }

    /* find 3 largest Q8_0 tensors */
    uint64_t n = gf->tensor_count;
    uint64_t best[3] = {0,0,0}; uint64_t best_sz[3] = {0,0,0};
    for (uint64_t i = 0; i < n; i++) {
        if (gf->tensors[i].type != 8) continue;
        uint64_t sz = gf->tensors[i].size_bytes;
        for (int k = 0; k < 3; k++) {
            if (sz > best_sz[k]) {
                for (int j = 2; j > k; j--) { best[j] = best[j-1]; best_sz[j] = best_sz[j-1]; }
                best[k] = i; best_sz[k] = sz; break;
            }
        }
    }

    FILE *fp = fopen(model, "rb");
    uint8_t *buf = (uint8_t*)malloc(128*1024*1024);
    for (int k = 0; k < 3 && best_sz[k]; k++) {
        uint64_t i = best[k];
        GGUF_Tensor *t = &gf->tensors[i];
        fseek(fp, (long)(gf->tensor_data_start + t->offset), SEEK_SET);
        fread(buf, 1, (size_t)t->size_bytes, fp);
        uint64_t nblocks = t->size_bytes / 34;
        /* extract weights */
        uint8_t *w = (uint8_t*)malloc(nblocks*32);
        for (uint64_t b = 0; b < nblocks; b++) memcpy(w + b*32, buf + b*34 + 2, 32);
        uint64_t w_raw = nblocks*32;
        uint8_t *enc = (uint8_t*)malloc(w_raw*2 + 4096);
        uint32_t enc_sz = kis_v4_encode((int8_t*)w, (uint32_t)w_raw, enc, (uint32_t)(w_raw*2+4096));
        printf("tensor %-60s type=%d raw=%lluMB enc=%uKB ratio=%s\n",
               t->name, t->type,
               (unsigned long long)(t->size_bytes/1048576),
               enc_sz/1024,
               (enc_sz < w_raw) ? "COMPRESSED" : "EXPANDED");
        printf("    weights-only raw=%lluMB enc=%lluKB  (enc/raw=%.2fx)\n",
               (unsigned long long)(w_raw/1048576), (unsigned long long)(enc_sz/1024),
               (double)enc_sz/w_raw);
        free(w); free(enc);
    }
    free(buf); fclose(fp);
    gguf_close(gf);
    return 0;
}
