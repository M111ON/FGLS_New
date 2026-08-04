/* kis_bond_codec_test.c — Full pipeline: codec + bond_key position roundtrip
 *
 * Pipeline:
 *   1. Read Q8_0 weights from GGUF
 *   2. For each weight at position i:
 *      - bond_key = pogls_bond_key(origin_seed = i)
 *      - pair = {Q8_value, bond_key}
 *   3. Sort pairs by Q8_value
 *   4. Encode:
 *      - RLE counts per value (from kis_codec.h)
 *      - Sorted bond_keys (for position reconstruction)
 *   5. Decode:
 *      - RLE → sorted values
 *      - Read sorted bond_keys
 *      - Pair (value, bond_key) → sort by bond_key → original order
 *   6. Verify: decoded == original (byte-level)
 *
 * This tests the FULL lossless pipeline including position.
 * The permutation cost is the key measurement — can we beat raw?
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "gguf_reader.h"
#include "core/kis_codec.h"
#include "collection/pogls_bond.h"

/* ── Weight + bond pair ─────────────────────────────── */
typedef struct {
    int8_t  value;      /* Q8 raw value */
    uint64_t bond_key;  /* unique position identifier */
} WeightBond;

/* Sort by value (primary), bond_key (secondary for stability) */
static int cmp_by_value(const void *a, const void *b) {
    const WeightBond *wa = (const WeightBond*)a;
    const WeightBond *wb = (const WeightBond*)b;
    if (wa->value < wb->value) return -1;
    if (wa->value > wb->value) return 1;
    if (wa->bond_key < wb->bond_key) return -1;
    if (wa->bond_key > wb->bond_key) return 1;
    return 0;
}

/* Sort by bond_key (for position reconstruction) */
static int cmp_by_bond(const void *a, const void *b) {
    const WeightBond *wa = (const WeightBond*)a;
    const WeightBond *wb = (const WeightBond*)b;
    if (wa->bond_key < wb->bond_key) return -1;
    if (wa->bond_key > wb->bond_key) return 1;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   ENCODE: full pipeline
   Returns compressed buffer, sets out_size
   ═══════════════════════════════════════════════════════════════ */
static uint8_t *encode_full(const int8_t *weights, uint64_t n,
                             uint64_t *out_size) {
    /* Step 1: Create pairs with bond_key */
    WeightBond *pairs = (WeightBond*)malloc(n * sizeof(WeightBond));
    if (!pairs) return NULL;

    for (uint64_t i = 0; i < n; i++) {
        pairs[i].value = weights[i];
        PoglsPiece piece = pogls_make_piece((uint64_t)i, 1);
        pairs[i].bond_key = pogls_bond_key(&piece);
    }

    /* Step 2: Sort by value */
    qsort(pairs, n, sizeof(WeightBond), cmp_by_value);

    /* Step 3: RLE on sorted values */
    KisCodec codec;
    kis_codec_build(&codec, weights, n);

    uint8_t rle[2048];
    if (kis_rle_encode(&codec, rle, 2048) != 0) {
        free(pairs);
        return NULL;
    }

    /* Step 4: Delta-varint encode sorted bond_keys */
    uint8_t *bond_buf = (uint8_t*)malloc(n * 9); /* max varint per key */
    if (!bond_buf) { free(pairs); return NULL; }

    uint32_t bond_pos = 0;
    uint64_t prev = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t cur = pairs[i].bond_key;
        uint64_t delta = cur - prev;
        /* varint encode delta */
        while (delta >= 0x80) {
            bond_buf[bond_pos++] = (uint8_t)(delta & 0x7F) | 0x80;
            delta >>= 7;
        }
        bond_buf[bond_pos++] = (uint8_t)delta;
        prev = cur;
    }

    /* Step 5: Pack output: [codec_header][rle][bond_data] */
    /* codec_header: 32B bitmap + 8B n_weights + 4B rle_len + 4B bond_len */
    uint32_t header_sz = 32 + 8 + 4 + 4;
    uint32_t total = header_sz + codec.rle_bytes + bond_pos;
    uint8_t *out = (uint8_t*)malloc(total);
    if (!out) { free(pairs); free(bond_buf); return NULL; }

    uint32_t off = 0;
    memcpy(out + off, codec.active, 32); off += 32;
    memcpy(out + off, &n, 8); off += 8;
    uint32_t rl = codec.rle_bytes;
    memcpy(out + off, &rl, 4); off += 4;
    memcpy(out + off, &bond_pos, 4); off += 4;
    memcpy(out + off, rle, rl); off += rl;
    memcpy(out + off, bond_buf, bond_pos); off += bond_pos;

    *out_size = off;

    free(pairs);
    free(bond_buf);
    return out;
}

/* ═══════════════════════════════════════════════════════════════
   DECODE: full pipeline
   Returns decoded weights (original order), 0 on success
   ═══════════════════════════════════════════════════════════════ */
static int decode_full(const uint8_t *data, uint64_t data_size,
                        int8_t *output, uint64_t output_n) {
    /* Parse header */
    const uint8_t *p = data;
    uint32_t off = 0;

    uint8_t active[32];
    memcpy(active, p + off, 32); off += 32;

    uint64_t n;
    memcpy(&n, p + off, 8); off += 8;
    if (n != output_n) return -1;

    uint32_t rle_len;
    memcpy(&rle_len, p + off, 4); off += 4;
    uint32_t bond_len;
    memcpy(&bond_len, p + off, 4); off += 4;

    const uint8_t *rle = p + off; off += rle_len;
    const uint8_t *bond_data = p + off; off += bond_len;

    if (off > data_size) return -2;

    /* Decode RLE → sorted values */
    int8_t *sorted_vals = (int8_t*)malloc(n);
    if (!sorted_vals) return -3;
    if (kis_rle_decode(active, rle, rle_len, sorted_vals, n) != 0) {
        free(sorted_vals);
        return -4;
    }

    /* Decode delta-varint → sorted bond_keys */
    WeightBond *pairs = (WeightBond*)malloc(n * sizeof(WeightBond));
    if (!pairs) { free(sorted_vals); return -5; }

    uint32_t br = 0;
    uint64_t prev = 0;
    for (uint64_t i = 0; i < n && br < bond_len; i++) {
        uint64_t delta = 0;
        uint32_t shift = 0;
        for (; br < bond_len; ) {
            uint8_t byte = bond_data[br++];
            delta |= (uint64_t)(byte & 0x7F) << shift;
            shift += 7;
            if (!(byte & 0x80)) break;
        }
        prev += delta;
        pairs[i].value = sorted_vals[i];
        pairs[i].bond_key = prev;
    }

    free(sorted_vals);

    /* Sort by bond_key → original order */
    qsort(pairs, n, sizeof(WeightBond), cmp_by_bond);

    /* Extract values in original order */
    for (uint64_t i = 0; i < n; i++) {
        output[i] = pairs[i].value;
    }

    free(pairs);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   TEST
   ═══════════════════════════════════════════════════════════════ */
static int test_gguf(const char *path, uint64_t max_weights) {
    GGUF_File *gf = gguf_open(path);
    if (!gf) {
        printf("  SKIP: cannot open %s\n", path);
        return 0;
    }

    /* Find Q8_0 tensor */
    int tidx = -1;
    for (uint64_t i = 0; i < gf->tensor_count; i++) {
        if (gf->tensors[i].type == GGML_TYPE_Q8_0) {
            tidx = (int)i; break;
        }
    }
    if (tidx < 0) {
        printf("  SKIP: no Q8_0\n");
        gguf_close(gf); return 0;
    }

    GGUF_Tensor *t = &gf->tensors[tidx];
    uint64_t n = t->n_weights;
    if (max_weights > 0 && n > max_weights) n = max_weights;

    printf("  Tensor: %s  (%lu weights)\n", t->name, (unsigned long)n);

    /* Read Q8_0 */
    int8_t *raw = (int8_t*)malloc((size_t)n);
    if (!raw) { gguf_close(gf); return 1; }

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

    /* Encode */
    clock_t t0 = clock();
    uint64_t enc_size = 0;
    uint8_t *encoded = encode_full(raw, rd, &enc_size);
    clock_t t1 = clock();
    if (!encoded) {
        printf("  FAIL: encode error\n");
        free(raw); return 1;
    }

    /* Decode */
    int8_t *decoded = (int8_t*)malloc((size_t)rd);
    if (!decoded) { free(encoded); free(raw); return 1; }

    clock_t t2 = clock();
    if (decode_full(encoded, enc_size, decoded, rd) != 0) {
        printf("  FAIL: decode error\n");
        free(decoded); free(encoded); free(raw); return 1;
    }
    clock_t t3 = clock();

    /* Verify byte-level exact match */
    int mismatch = 0;
    for (uint64_t i = 0; i < rd; i++) {
        if (raw[i] != decoded[i]) {
            if (mismatch < 5) {
                printf("  MISMATCH at [%lu]: orig=%d dec=%d\n",
                       (unsigned long)i, raw[i], decoded[i]);
            }
            mismatch++;
        }
    }

    /* Stats */
    double raw_mb = rd * 1.0 / 1048576.0;
    double enc_mb = enc_size / 1048576.0;
    double enc_time = (double)(t1 - t0) / CLOCKS_PER_SEC;
    double dec_time = (double)(t3 - t2) / CLOCKS_PER_SEC;

    printf("  Raw:       %.2f MB\n", raw_mb);
    printf("  Encoded:   %.2f MB\n", enc_mb);
    printf("  Ratio:     %.4fx\n", raw_mb / enc_mb);
    printf("  Encode:    %.3f sec (%.1f MB/s)\n", enc_time,
           enc_time > 0 ? raw_mb / enc_time : 0);
    printf("  Decode:    %.3f sec (%.1f MB/s)\n", dec_time,
           dec_time > 0 ? raw_mb / dec_time : 0);

    if (mismatch) {
        printf("  FAIL: %d mismatches / %lu\n", mismatch, (unsigned long)rd);
        free(decoded); free(encoded); free(raw);
        return 1;
    }

    printf("  PASS: byte-exact roundtrip\n");

    free(decoded); free(encoded); free(raw);
    return 0;
}

int main(int argc, char **argv) {
    printf("═══ KIS BOND CODEC TEST ═══\n");
    int fail = 0;

    const char *mpath = argc > 1 ? argv[1]
        : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    uint64_t maxw = argc > 2 ? (uint64_t)atoll(argv[2]) : 2000000;
    fail += test_gguf(mpath, maxw);

    /* Test on second model if available */
    if (argc > 3) {
        fail += test_gguf(argv[3], maxw);
    }

    printf("\n══════════════════════════\n");
    printf("  RESULTS: %d FAIL\n", fail);
    printf("══════════════════════════\n");
    return fail;
}