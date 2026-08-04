/* kis_chunk_codec_test.c — Full roundtrip: per-chunk RLE + delta permutation */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "gguf_reader.h"
#include "core/kis_chunk_codec.h"

static int test_full(const char *path, uint64_t max_weights) {
    GGUF_File *gf = gguf_open(path);
    if (!gf) { printf("  SKIP: %s\n", path); return 0; }
    int tidx = -1;
    for (uint64_t i = 0; i < gf->tensor_count; i++)
        if (gf->tensors[i].type == GGML_TYPE_Q8_0) { tidx = (int)i; break; }
    if (tidx < 0) { printf("  SKIP: no Q8_0\n"); gguf_close(gf); return 0; }
    GGUF_Tensor *t = &gf->tensors[tidx];
    uint64_t n = t->n_weights;
    if (max_weights > 0 && n > max_weights) n = max_weights;
    printf("  Tensor: %s  (%lu weights)\n", t->name, (unsigned long)n);

    int8_t *raw = (int8_t*)malloc((size_t)n);
    uint64_t foff = gf->tensor_data_start + t->offset;
    foff = (foff + 31) & ~(uint64_t)31;
    fseek(gf->fp, (long)foff, SEEK_SET);
    uint64_t rd = 0;
    uint64_t nblk = (t->n_weights + 31) / 32;
    for (uint64_t b = 0; b < nblk && rd < n; b++) {
        uint16_t scale; int8_t w[32];
        if (fread(&scale,2,1,gf->fp) != 1) break;
        if (fread(w,1,32,gf->fp) != 32) break;
        for (int i = 0; i < 32 && rd < n; i++) raw[rd++] = w[i];
    }
    gguf_close(gf);

    /* === ENCODE === */
    clock_t t0 = clock();
    uint64_t max_c = (rd + 31) / 32;
    KCC_Chunk *chunks = (KCC_Chunk*)malloc(max_c * sizeof(KCC_Chunk));
    uint64_t nc = kcc_group_chunks(raw, rd, chunks, max_c);
    kcc_sort_chunks(chunks, nc);
    KCC_Stats stats;
    uint64_t enc_sz = 0;
    uint8_t *enc = kcc_encode(chunks, nc, &stats, &enc_sz);
    clock_t t1 = clock();

    /* === DECODE === */
    uint32_t off = 56;
    uint32_t species_sz = (uint32_t)(nc * 2);
    uint32_t bond_sz = (uint32_t)(nc * 8);
    uint32_t offsets_sz = (uint32_t)(nc * 4);
    const uint32_t *offsets = (const uint32_t*)(enc + off + species_sz + bond_sz);
    const uint8_t *chunk_data = enc + off + species_sz + bond_sz + offsets_sz;

    /* Decode delta permutation */
    uint32_t delta_off = off + species_sz + bond_sz + offsets_sz + stats.chunk_data_bytes;
    uint32_t delta_len = (uint32_t)(enc_sz - delta_off);
    uint32_t *perm = (uint32_t*)malloc(nc * sizeof(uint32_t));
    kcc_decode_delta_perm(enc + delta_off, delta_len, perm, nc);

    /* Build inverse permutation */
    uint32_t *inv_perm = (uint32_t*)malloc(nc * sizeof(uint32_t));
    for (uint64_t i = 0; i < nc; i++) inv_perm[perm[i]] = (uint32_t)i;

    /* Decode each chunk's raw data */
    uint8_t *sorted_chunk_weights = (uint8_t*)malloc(nc * KCC_CHUNK_WORDS);
    for (uint64_t i = 0; i < nc; i++) {
        uint32_t c_off = offsets[i];
        kcc_decode_chunk_raw(chunk_data + c_off, sorted_chunk_weights + i * KCC_CHUNK_WORDS);
    }

    /* Reconstruct: for each original position j, get sorted chunk inv_perm[j] */
    clock_t t2 = clock();
    int8_t *reconstructed = (int8_t*)malloc(rd);
    for (uint64_t j = 0; j < nc; j++) {
        uint32_t sp = inv_perm[j];
        uint64_t src = sp * KCC_CHUNK_WORDS;
        uint64_t dst = j * KCC_CHUNK_WORDS;
        for (int w = 0; w < KCC_CHUNK_WORDS && dst + w < (uint64_t)rd; w++)
            reconstructed[dst + w] = (int8_t)sorted_chunk_weights[src + w];
    }
    clock_t t3 = clock();

    /* === XOR === */
    uint64_t diff = 0;
    for (uint64_t i = 0; i < rd; i++)
        if (reconstructed[i] != raw[i]) diff++;

    /* === RESULTS === */
    kcc_print(&stats);
    printf("  Encode: %.4fs  Decode: %.4fs\n",
           (double)(t1-t0)/CLOCKS_PER_SEC, (double)(t3-t2)/CLOCKS_PER_SEC);

    if (diff == 0) {
        printf("  ✅ FULL ROUNDTRIP: LOSSLESS (%lu weights, 0 differ)\n",
               (unsigned long)rd);
    } else {
        printf("  ❌ MISMATCH: %lu / %lu differ\n",
               (unsigned long)diff, (unsigned long)rd);
    }

    free(inv_perm); free(perm); free(reconstructed);
    free(sorted_chunk_weights); free(enc); free(chunks); free(raw);
    return (diff == 0) ? 0 : 1;
}

int main(int argc, char **argv) {
    printf("═══ KIS CHUNK CODEC v2 — FULL ROUNDTRIP ═══\n");
    const char *mp = argc > 1 ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    uint64_t mw = argc > 2 ? (uint64_t)atoll(argv[2]) : 500000;
    int fail = test_full(mp, mw);
    printf("\n══════════════════════\n  RESULT: %s\n══════════════════════\n",
           fail ? "FAIL" : "PASS");
    return fail;
}