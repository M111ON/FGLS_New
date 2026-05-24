#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "tring.h"
#include "pogls_fold.h"
#include "geo_diamond_field_v4.h"
#include "geo_smart_encode.h"
#include "geo_onion_shell.h"
#include "geo_transform_seq.h"

int main() {
    FILE *f = fopen("test_onion_copy.bin", "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    uint8_t *data = malloc(sz);
    fread(data, 1, sz, f); fclose(f);

    int n_chunks = (sz + 63) / 64;
    printf("n_chunks=%d orig_sz=%ld\n", n_chunks, sz);

    /* Encode-only: check DIAMOND for chunk 505 */
    DiamondField df; dfield_init(&df, (uint32_t)(n_chunks + 64));
    TsEncCtx ts; ts_enc_init(&ts, &df, (uint32_t)n_chunks);

    for (int seq = 0; seq < n_chunks; seq++) {
        uint8_t chunk[64] = {0};
        size_t src = (size_t)seq * 64;
        size_t cp = (size_t)sz - src; if (cp > 64) cp = 64;
        memcpy(chunk, data + src, cp);

        uint32_t esz;
        /* Simulate what v7 does: update ts_ctx for each chunk */
        if (ts.has_prev && memcmp(chunk, ts.prev_chunk, 64) == 0) {
            // skip - would be identity
        } else {
            const uint8_t *enc = ts_encode(&ts, chunk, &esz, (uint32_t)seq);
            if (seq == 505) {
                printf("seq=505 tag=%u esz=%u\n", enc[0], esz);
                if (enc[0] == 2) { // DIAMOND
                    uint32_t dsz;
                    memcpy(&dsz, enc+1, 4);
                    printf("  dsz=%u data[0..7]=", dsz);
                    for (int i=0;i<8;i++) printf("%02x",enc[5+i]);
                    printf("\n");
                }
            }
        }
        /* Update ts_ctx has_prev regardless (since v7 syncs) */
        memcpy(ts.prev_chunk, chunk, 64);
        ts.has_prev = 1;
    }

    /* Now decode: re-init and simulate decoder */
    DiamondField df2; dfield_init(&df2, (uint32_t)(n_chunks + 64));
    TsEncCtx ts2; ts_enc_init(&ts2, &df2, (uint32_t)n_chunks);

    // Reset to redo
    ts_enc_free(&ts2);
    dfield_free(&df2);
    dfield_init(&df2, (uint32_t)(n_chunks + 64));
    memset(&ts2, 0, sizeof(ts2));
    ts_enc_init(&ts2, &df2, (uint32_t)n_chunks);

    uint8_t *dec = calloc(sz, 1);
    for (int seq = 0; seq < n_chunks; seq++) {
        uint8_t chunk[64] = {0};
        size_t src = (size_t)seq * 64;
        size_t cp = (size_t)sz - src; if (cp > 64) cp = 64;
        memcpy(chunk, data + src, cp);

        uint32_t esz;
        if (ts2.has_prev && memcmp(chunk, ts2.prev_chunk, 64) == 0) {
            // identity
        } else {
            const uint8_t *enc = ts_encode(&ts2, chunk, &esz, (uint32_t)seq);
            /* Simulate decoder: push diamond to tring, then decode */
            if (enc[0] == 2) {
                uint32_t dsz;
                memcpy(&dsz, enc+1, 4);
                uint8_t dbuf[64];
                memcpy(dbuf, enc+5, dsz);
                uint32_t tick = tring_push(&df2.tring, dbuf, dsz);
                uint8_t out[64];
                if (dfield_decode_flat(&df2, tick, out)) {
                    printf("FAIL decode seq=%d\n", seq); return 1;
                }
                memcpy(dec + seq*64, out, cp);
            } else {
                memcpy(dec + src, chunk, cp);
            }
        }
        memcpy(ts2.prev_chunk, chunk, 64);
        ts2.has_prev = 1;
    }

    /* Verify */
    if (memcmp(dec, data, (size_t)sz) == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
        for (int i=0;i<sz;i++) {
            if (dec[i] != data[i]) { printf("diff at byte %d\n", i); break; }
        }
    }

    ts_enc_free(&ts);
    ts_enc_free(&ts2);
    dfield_free(&df);
    dfield_free(&df2);
    free(data); free(dec);
    return 0;
}
