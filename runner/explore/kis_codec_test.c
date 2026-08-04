/* kis_codec_test.c — Verify kis_codec.h on synthetic and real GGUF Q8_0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gguf_reader.h"
#include "core/kis_codec.h"

/* ── Test 1: Codebook on 256 distinct values ────────── */
static int test_synthetic(void)
{
    int8_t data[256];
    for (int i = 0; i < 256; i++) data[i] = (int8_t)(i - 128);

    KisCodec c;
    if (kis_codec_build(&c, data, 256) != 0) {
        printf("  FAIL: codec build\n"); return 1;
    }
    if (c.n_active != 256) {
        printf("  FAIL: n_active=%u != 256\n", c.n_active); return 1;
    }
    for (int i = 0; i < 256; i++) {
        if (c.histo[i] != 1) {
            printf("  FAIL: histo[%d]=%u\n", i, c.histo[i]); return 1;
        }
    }
    printf("  PASS: codebook 256x1\n");
    return 0;
}

/* ── Test 2: RLE roundtrip (3×256) ──────────────────── */

static int test_rle_roundtrip(void)
{
    int8_t data[768];
    for (int v = 0; v < 256; v++)
        for (int r = 0; r < 3; r++)
            data[r*256 + v] = (int8_t)(v - 128);

    KisCodec codec;
    kis_codec_build(&codec, data, 768);

    uint8_t rle[2048];
    if (kis_rle_encode(&codec, rle, 2048) != 0) {
        printf("  FAIL: rle encode error\n"); return 1;
    }

    int8_t *dec = (int8_t*)malloc(768);
    if (!dec) return 1;

    if (kis_rle_decode(codec.active, rle, codec.rle_bytes, dec, 768) != 0) {
        printf("  FAIL: rle decode error\n"); free(dec); return 1;
    }

    if (kis_codec_verify(&codec, data, dec) != 0) {
        printf("  FAIL: histogram mismatch\n"); free(dec); return 1;
    }

    /* Check sorted order: decoded =  val×cnt sequence */
    uint64_t pos = 0;
    for (int v = 0; v < 256; v++) {
        if (codec.histo[v] != 3) {
            printf("  FAIL: expected 3 count for val %d\n", v);
            free(dec); return 1;
        }
        for (uint32_t i = 0; i < (uint32_t)codec.histo[v]; i++) {
            if (pos >= 768 || dec[pos] != (int8_t)(v - 128)) {
                printf("  FAIL: decoded[%lu] != val%d\n",
                       (unsigned long)pos, v);
                free(dec); return 1;
            }
            pos++;
        }
    }

    free(dec);
    printf("  PASS: RLE roundtrip 256×3\n");
    return 0;
}

/* ── Test 3: Real GGUF ──────────────────────────────── */

static int test_gguf(const char *path, uint64_t max_weights)
{
    GGUF_File *gf = gguf_open(path);
    if (!gf) {
        printf("  SKIP: cannot open %s\n", path);
        return 0;
    }

    int tidx = -1;
    for (uint64_t i = 0; i < gf->tensor_count; i++) {
        if (gf->tensors[i].type == GGML_TYPE_Q8_0) {
            tidx = (int)i; break;
        }
    }
    if (tidx < 0) {
        printf("  SKIP: no Q8_0 tensor in model\n");
        gguf_close(gf); return 0;
    }

    GGUF_Tensor *t = &gf->tensors[tidx];
    printf("  Tensor: %s  (%lu weights)\n", t->name,
           (unsigned long)t->n_weights);

    uint64_t n = t->n_weights;
    if (max_weights > 0 && n > max_weights) n = max_weights;

    int8_t *raw = (int8_t*)malloc((size_t)n);
    if (!raw) { gguf_close(gf); return 1; }

    /* Read Q8_0 blocks */
    uint64_t data_off = gf->tensor_data_start + t->offset;
    data_off = (data_off + 31) & ~(uint64_t)31;
    fseek(gf->fp, (long)data_off, SEEK_SET);

    uint64_t rd = 0;
    uint64_t nblk = (t->n_weights + 31) / 32;
    for (uint64_t b = 0; b < nblk && rd < n; b++) {
        uint16_t scale; int8_t w[32];
        if (fread(&scale,2,1,gf->fp) != 1) break;
        if (fread(w,1,32,gf->fp) != 32) break;
        for (int i = 0; i < 32 && rd < n; i++)
            raw[rd++] = w[i];
    }
    gguf_close(gf);
    printf("  Read %lu weights\n", (unsigned long)rd);

    /* Encode */
    KisCodec codec;
    kis_codec_build(&codec, raw, rd);

    uint8_t rle[2048];
    if (kis_rle_encode(&codec, rle, 2048) != 0) {
        printf("  FAIL: rle encode\n"); free(raw); return 1;
    }

    /* Decode */
    int8_t *dec = (int8_t*)malloc((size_t)rd);
    if (!dec) { free(raw); return 1; }
    if (kis_rle_decode(codec.active, rle, codec.rle_bytes, dec, rd) != 0) {
        printf("  FAIL: rle decode\n"); free(dec); free(raw); return 1;
    }

    if (kis_codec_verify(&codec, raw, dec) != 0) {
        printf("  FAIL: histogram mismatch\n");
        free(dec); free(raw); return 1;
    }

    kis_codec_print(&codec);

    /* Quick ratio check: codec weight bytes vs raw bytes */
    printf("  PASS: GGUF roundtrip (histogram verified)\n");

    free(dec); free(raw);
    return 0;
}

int main(int argc, char **argv)
{
    printf("═══ KIS CODEC TEST ═══\n");
    int fail = 0;

    fail += test_synthetic();
    fail += test_rle_roundtrip();

    const char *mpath = argc > 1 ? argv[1]
        : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    uint64_t maxw = argc > 2 ? (uint64_t)atoll(argv[2]) : 1000000;
    fail += test_gguf(mpath, maxw);

    printf("\n══════════════════════\n");
    printf("  RESULTS: %d FAIL\n", fail);
    printf("══════════════════════\n");
    return fail;
}