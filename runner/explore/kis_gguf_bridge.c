/*
 * kis_gguf_bridge.c — Bridge: GGUF weights → Adaptive Store → Kis Container
 *
 * Pipeline: GGUF tensor → dequant Q8_0 → entropy detect → adaptive write → kis container
 *          → kis deserialize → adaptive read → verify lossless roundtrip
 *
 * Compile:
 *   gcc -O2 -std=c11 -Wall -Wextra -I. -Irunner/explore \
 *       -o runner/explore/kis_gguf_bridge.exe \
 *       runner/explore/kis_gguf_bridge.c -lm
 * Run:
 *   runner/explore/kis_gguf_bridge.exe I:/model/SmolLM2-360M-Instruct.Q8_0.gguf
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "core/geo_adaptive_store.h"
#include "core/geo_kis_container.h"
#include "gguf_reader.h"

static int pass_count = 0, fail_count = 0;
#define T(n,desc,ok) do { \
    if (ok) { pass_count++; printf("  T%d: PASS — %s\n", n, desc); } \
    else    { fail_count++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* Compute entropy score from float block (0..255) */
static uint8_t compute_entropy(const float *w, int n) {
    int distinct = 0;
    uint8_t seen[256];
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < n; i++) {
        uint8_t bucket = (uint8_t)((int)(w[i] * 100) & 0xFF);
        if (!seen[bucket]) { seen[bucket] = 1; distinct++; }
    }
    return (uint8_t)(distinct > 255 ? 255 : distinct);
}

/* Dequantize one Q8_0 block: 2B scale + 32B int8 → 32 floats */
static int dequant_q8_block(const uint8_t *raw, float *out) {
    uint16_t scale_u16;
    memcpy(&scale_u16, raw, 2);
    /* Simple fp16 → fp32 */
    float scale = (float)(scale_u16 & 0x7FFF) / 1024.0f;
    if (scale_u16 & 0x8000) scale = -scale;
    for (int i = 0; i < 32; i++) {
        out[i] = (float)((int8_t)raw[2 + i]) * scale;
    }
    return 32; /* weights produced */
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <model.gguf>\n", argv[0]);
        return 1;
    }

    const char *fin = argv[1];
    printf("=== KIS GGUF Bridge — Adaptive Storage Pipeline ===\n");
    printf("File: %s\n\n", fin);

    clock_t t_start = clock();

    /* ── Open GGUF ── */
    GGUF_File gf;
    memset(&gf, 0, sizeof(gf));
    gf.fp = fopen(fin, "rb");
    if (!gf.fp) { printf("Cannot open file\n"); return 1; }

    uint32_t magic;
    fread(&magic, 4, 1, gf.fp);
    T(1, "GGUF magic", magic == GGUF_MAGIC);

    fread(&gf.version, 4, 1, gf.fp);
    fread(&gf.tensor_count, 8, 1, gf.fp);
    fread(&gf.kv_count, 8, 1, gf.fp);
    printf("  version=%u tensors=%llu kv=%llu\n",
           gf.version, (unsigned long long)gf.tensor_count, (unsigned long long)gf.kv_count);
    T(2, "version >= 3", gf.version >= 3);

    /* Skip KV metadata */
    for (uint64_t i = 0; i < gf.kv_count; i++) {
        GGUFFieldStr key;
        read_gguf_str_fp(gf.fp, &key);
        uint32_t vtype; fread(&vtype, 4, 1, gf.fp);
        skip_gguf_value(gf.fp, vtype);
        free(key.data);
    }

    /* Read tensor info */
    gf.tensors = (GGUF_Tensor*)malloc(sizeof(GGUF_Tensor) * gf.tensor_count);
    for (uint64_t i = 0; i < gf.tensor_count; i++) {
        GGUF_Tensor *t = &gf.tensors[i];
        GGUFFieldStr name;
        read_gguf_str_fp(gf.fp, &name);
        strncpy(t->name, name.data, 255);
        free(name.data);
        fread(&t->n_dims, 4, 1, gf.fp);
        for (uint32_t d = 0; d < t->n_dims; d++)
            fread(&t->dims[d], 8, 1, gf.fp);
        fread(&t->type, 4, 1, gf.fp);
        uint64_t offset; fread(&offset, 8, 1, gf.fp);
        t->offset = offset;
        uint64_t block_sz, w_per_block;
        ggml_type_block_size(t->type, &block_sz, &w_per_block);
        uint64_t n_blocks = 1;
        for (uint32_t d = 0; d < t->n_dims; d++) n_blocks *= t->dims[d];
        n_blocks /= w_per_block;
        t->size_bytes = n_blocks * block_sz;
        t->n_weights = n_blocks * w_per_block;
    }

    /* Align to 32 bytes */
    long pos = ftell(gf.fp);
    long aligned = (pos + 31) & ~31L;
    fseek(gf.fp, aligned, SEEK_SET);
    gf.tensor_data_start = aligned;

    /* ── Find first Q8_0 tensor ── */
    int q8_tensor = -1;
    for (uint64_t i = 0; i < gf.tensor_count; i++) {
        if (gf.tensors[i].type == GGML_TYPE_Q8_0 && gf.tensors[i].n_weights >= 256) {
            q8_tensor = (int)i;
            break;
        }
    }
    T(3, "found Q8_0 tensor", q8_tensor >= 0);
    if (q8_tensor < 0) { printf("No Q8_0 tensor found\n"); return 1; }

    GGUF_Tensor *ten = &gf.tensors[q8_tensor];
    printf("  Tensor[%d]: %s type=%u n_weights=%llu\n",
           q8_tensor, ten->name, ten->type, (unsigned long long)ten->n_weights);

    /* ── Read Q8_0 blocks ── */
    fseek(gf.fp, gf.tensor_data_start + ten->offset, SEEK_SET);

    int n_blocks = (int)(ten->n_weights / 32);
    int max_blocks = n_blocks;
    if (max_blocks > 100) max_blocks = 100; /* limit for test */

    uint8_t *raw_blocks = (uint8_t*)malloc(max_blocks * 34); /* 34 bytes per Q8_0 block */
    float *dequant_buf = (float*)malloc(max_blocks * 32 * sizeof(float));
    int total_weights = 0;

    for (int b = 0; b < max_blocks; b++) {
        if (fread(raw_blocks + b * 34, 1, 34, gf.fp) != 34) break;
        dequant_q8_block(raw_blocks + b * 34, dequant_buf + b * 32);
        total_weights += 32;
    }
    printf("  Read %d blocks = %d weights\n", max_blocks, total_weights);
    T(4, "read weights", total_weights > 0);

    /* ── Phase 1: Adaptive Store roundtrip ── */
    printf("\n--- Phase 1: Adaptive Store Roundtrip ---\n");

    /* Process in chunks of 64 weights (1 DiamondBlock) */
    int chunk_size = 64;
    int n_chunks = total_weights / chunk_size;

    /* Each chunk gets its own AdaptiveStore (single-write design) */
    AdaptiveStore *stores = (AdaptiveStore*)malloc(n_chunks * sizeof(AdaptiveStore));
    uint8_t *entropies = (uint8_t*)malloc(n_chunks);
    int write_ok = 1;

    for (int c = 0; c < n_chunks; c++) {
        float *chunk = dequant_buf + c * chunk_size;
        uint8_t entropy = compute_entropy(chunk, chunk_size);
        entropies[c] = entropy;
        adaptive_init(&stores[c]);
        int rc = adaptive_write(&stores[c], c, chunk, chunk_size, entropy);
        if (rc != 0) { write_ok = 0; break; }
    }
    T(5, "write all chunks", write_ok);

    /* Verify each store individually */
    int verify_ok = 1;
    for (int c = 0; c < n_chunks; c++) {
        if (adaptive_verify(&stores[c]) != 0) { verify_ok = 0; break; }
    }
    T(6, "verify all stores", verify_ok);

    /* Read back and compare */
    float *readback = (float*)malloc(total_weights * sizeof(float));
    int read_ok = 1;
    for (int c = 0; c < n_chunks; c++) {
        adaptive_read(&stores[c], c, readback + c * chunk_size, chunk_size);
    }
    for (int i = 0; i < total_weights; i++) {
        if (fabsf(readback[i] - dequant_buf[i]) > 1e-6f) { read_ok = 0; break; }
    }
    T(7, "roundtrip exact", read_ok);

    /* Use last store for container test */
    AdaptiveStore as = stores[n_chunks - 1];

    /* ── Phase 2: Container roundtrip ── */
    printf("\n--- Phase 2: Kis Container Roundtrip ---\n");

    KisHeader hdr;
    kis_container_init(&hdr, &as);
    uint32_t sz = kis_container_size(&hdr);
    printf("  Container size: %u bytes (%.1f KB)\n", sz, sz / 1024.0f);

    uint8_t *buf = (uint8_t*)malloc(sz + 256);
    int wrote = kis_container_serialize(&hdr, as.frames, as.blocks, buf, sz + 256);
    T(8, "serialize", wrote == (int)sz);

    int vrc = kis_container_verify(buf, sz);
    T(9, "CRC verify", vrc == 0);

    /* Deserialize and compare header */
    KisHeader hdr2;
    int drc = kis_container_deserialize(&hdr2, buf, sz);
    T(10, "deserialize", drc == 0);
    T(11, "header match", hdr2.tier == hdr.tier && hdr2.weight_cnt == hdr.weight_cnt);

    /* ── Phase 3: Performance ── */
    printf("\n--- Phase 3: Performance ---\n");

    clock_t t_end = clock();
    double elapsed = (double)(t_end - t_start) / CLOCKS_PER_SEC;
    printf("  Total time: %.3f seconds\n", elapsed);
    printf("  Weights/sec: %.0f\n", total_weights / elapsed);
    printf("  Throughput: %.1f MB/s\n", (total_weights * sizeof(float)) / elapsed / 1024 / 1024);

    /* ── Summary ── */
    printf("\n--- Summary ---\n");
    printf("  Model: %s\n", ten->name);
    printf("  Weights: %d (%d blocks × 32)\n", total_weights, max_blocks);
    printf("  Tier: %d (entropy-based)\n", as.tier);
    printf("  Frames: %d\n", as.frame_count);
    printf("  Container: %u bytes\n", sz);
    printf("  Ratio: %.2f bytes/weight\n", (float)sz / total_weights);

    free(raw_blocks);
    free(dequant_buf);
    free(readback);
    free(buf);
    free(gf.tensors);
    fclose(gf.fp);

    printf("\nFINAL: %d PASS / %d FAIL\n", pass_count, fail_count);
    return fail_count;
}
