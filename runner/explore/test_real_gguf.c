/* test_real_gguf.c — Test FGLS pipeline with real GGUF tensor data
 * Build: gcc -O2 -std=c11 -DFGLS_PIPELINE_IMPLEMENTATION
 *        -I. -I../../ext -I../../collection -I../../collection/src
 *        -I../../collection/core/pogls_engine/twin_core
 *        -I../../collection/core/pogls_engine
 *        -I../../collection/core/pogls_engine/core -I../../collection/core/core
 *        -I../../collection/rdh -I../../beam_addressing
 *        test_real_gguf.c -lm -o test_real_gguf.exe
 * Usage: test_real_gguf.exe [model.gguf] [tensor_name] [strategy]
 * strategy: stride37, sequential, face_region, grid
 */

#define FGLS_PIPELINE_IMPLEMENTATION
#include "fgls_pipeline.h"
#include "gguf_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1] : "I:/model/Qwen3-0.6B-Q4_0.gguf";
    const char *tensor_name = argc > 2 ? argv[2] : "token_embd";
    const char *strategy_str = argc > 3 ? argv[3] : "stride37";
    
    fgls_strategy strategy = FGLS_STRIDE37;
    if (strcmp(strategy_str, "sequential") == 0) strategy = FGLS_SEQUENTIAL;
    else if (strcmp(strategy_str, "face_region") == 0) strategy = FGLS_FACE_REGION;
    else if (strcmp(strategy_str, "grid") == 0) strategy = FGLS_GRID;
    
    printf("=== FGLS Pipeline + Real GGUF Test ===\n");
    printf("Model: %s\n", model_path);
    printf("Tensor: %s\n", tensor_name);
    printf("Strategy: %s\n", strategy_str);
    
    /* Open GGUF */
    GGUF_File *gf = gguf_open(model_path);
    if (!gf) {
        fprintf(stderr, "Failed to open GGUF file\n");
        return 1;
    }
    
    int tidx = gguf_find_tensor(gf, tensor_name);
    if (tidx < 0) {
        fprintf(stderr, "Tensor '%s' not found\n", tensor_name);
        gguf_close(gf);
        return 1;
    }
    
    GGUF_Tensor *t = &gf->tensors[tidx];
    printf("Tensor: %s\n", t->name);
    printf("  Type: %u, Dims: ", t->type);
    for (uint32_t d = 0; d < t->n_dims; d++) printf("%llu ", (unsigned long long)t->dims[d]);
    printf("\n  n_weights: %llu, size_bytes: %llu, offset: %llu\n",
           (unsigned long long)t->n_weights, (unsigned long long)t->size_bytes, (unsigned long long)t->offset);
    
    /* Read tensor data */
    uint8_t *tensor_data = (uint8_t*)malloc((size_t)t->size_bytes);
    if (!tensor_data) {
        fprintf(stderr, "OOM\n");
        gguf_close(gf);
        return 1;
    }
    
    fseek(gf->fp, (long)t->offset, SEEK_SET);
    size_t read_bytes = fread(tensor_data, 1, (size_t)t->size_bytes, gf->fp);
    printf("Read %zu bytes\n", read_bytes);
    gguf_close(gf);
    
    /* Initialize pipeline */
    fgls_config cfg = fgls_default_config();
    cfg.strategy = strategy;
    cfg.use_geojump = 1;

    fgls_ctx *ctx = fgls_init(&cfg);
    if (!ctx) {
        fprintf(stderr, "Pipeline init failed\n");
        free(tensor_data);
        return 1;
    }

    /* Streaming roundtrip: encode + decode per chunk, supports any size */
    printf("\n--- Streaming Roundtrip (%zu bytes) ---\n", read_bytes);
    uint8_t *reconstructed = (uint8_t*)malloc(read_bytes);
    if (!reconstructed) { fgls_free(ctx); free(tensor_data); return 1; }
    int errors = fgls_stream_roundtrip(ctx, tensor_data, reconstructed, read_bytes);
    if (errors < 0) {
        fprintf(stderr, "Streaming roundtrip failed (%d)\n", errors);
        fgls_free(ctx);
        free(tensor_data);
        free(reconstructed);
        return 1;
    }
    printf("Streaming roundtrip done: %d block-level errors\n", errors);

    /* Verify full match */
    printf("\n--- Verifying roundtrip ---\n");
    int mismatches = 0;
    for (size_t i = 0; i < read_bytes; i++) {
        if (tensor_data[i] != reconstructed[i]) {
            if (mismatches < 10)
                printf("  Mismatch at %zu: orig=%d recon=%d\n", i, tensor_data[i], reconstructed[i]);
            mismatches++;
        }
    }

    printf("\n=== Roundtrip Verification ===\n");
    printf("  Total bytes: %zu\n", read_bytes);
    printf("  Mismatches: %d\n", mismatches);
    printf("  Match rate: %.4f%%\n", 100.0 * (read_bytes - mismatches) / read_bytes);

    if (mismatches == 0) {
        printf("  ✓ PERFECT ROUNDTRIP — full tensor verified\n");
    }
    
    /* Benchmark */
    printf("\n--- Benchmark (100 iterations) ---\n");
    double ns_op = fgls_benchmark(ctx, 100);
    if (ns_op > 0) {
        printf("  %.2f ns/op (%.2f M cells/s)\n", ns_op, 1e9 / ns_op / 1e6);
    }
    
    /* Stats */
    fgls_stats(ctx, stdout);
    
    fgls_free(ctx);
    free(tensor_data);
    free(reconstructed);
    
    return mismatches == 0 ? 0 : 1;
}