/* test_real_gguf.c — Test FGLS pipeline with real GGUF tensor data
 * Build: gcc -O2 -std=c11 -I.. -I../../ext -I../../collection -I../../collection/src \
 *        -I../../collection/core/pogls_engine/twin_core -I../../collection/core/pogls_engine \
 *        -I../../collection/core/pogls_engine/core -I../../collection/core/core \
 *        -I../../collection/rdh -I../../beam_addressing \
 *        test_real_gguf.c fgls_pipeline_cli.c -lm -o test_real_gguf.exe
 */

#include "fgls_pipeline.h"
#include "gguf_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Convert GGUF tensor raw bytes to fgls_cell array */
static int tensor_to_cells(const uint8_t *data, size_t n_bytes, fgls_cell *cells, int max_cells) {
    int n = (n_bytes < (size_t)max_cells) ? (int)n_bytes : max_cells;
    for (int i = 0; i < n; i++) {
        cells[i].face = (i / 1000) % 6;
        cells[i].z = (i / 100) % 10;
        cells[i].y = (i / 10) % 10;
        cells[i].x = i % 10;
        cells[i].value = (int8_t)data[i];
        cells[i].global_idx = i;
    }
    return n;
}

/* Convert fgls_cell array back to bytes */
static void cells_to_tensor(const fgls_cell *cells, int n, uint8_t *out) {
    for (int i = 0; i < n; i++) {
        if (cells[i].global_idx >= 0 && cells[i].global_idx < 6000) {
            out[cells[i].global_idx] = (uint8_t)cells[i].value;
        }
    }
}

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
    
    /* Use first 6000 bytes for contour cube */
    int use_bytes = read_bytes < 6000 ? (int)read_bytes : 6000;
    printf("Using first %d bytes (padded to 6000 cells)\n", use_bytes);
    
    /* Convert to cells */
    fgls_cell cells[6000] = {0};
    fgls_cell decoded[6000] = {0};
    int n_cells = tensor_to_cells(tensor_data, use_bytes, cells, 6000);
    printf("Created %d cells\n", n_cells);
    
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
    
    /* Encode */
    printf("\n--- Encoding ---\n");
    int enc = fgls_encode(ctx, cells, n_cells);
    if (enc != 0) {
        fprintf(stderr, "Encode failed\n");
        fgls_free(ctx);
        free(tensor_data);
        return 1;
    }
    printf("Encoded %d cells\n", n_cells);
    
    /* Decode */
    printf("\n--- Decoding ---\n");
    int dec = fgls_decode(ctx, decoded, 6000);
    if (dec < 0) {
        fprintf(stderr, "Decode failed\n");
        fgls_free(ctx);
        free(tensor_data);
        return 1;
    }
    printf("Decoded %d cells\n", dec);
    
    /* Verify roundtrip */
    uint8_t *reconstructed = (uint8_t*)calloc(6000, 1);
    cells_to_tensor(decoded, dec, reconstructed);
    
    int mismatches = 0;
    for (int i = 0; i < use_bytes; i++) {
        if (tensor_data[i] != reconstructed[i]) {
            mismatches++;
            if (mismatches <= 10) {
                printf("  Mismatch at %d: orig=%d recon=%d\n", i, tensor_data[i], reconstructed[i]);
            }
        }
    }
    
    printf("\n=== Roundtrip Verification ===\n");
    printf("  Total bytes: %d\n", use_bytes);
    printf("  Mismatches: %d\n", mismatches);
    printf("  Match rate: %.4f%%\n", 100.0 * (use_bytes - mismatches) / use_bytes);
    
    if (mismatches == 0) {
        printf("  ✓ PERFECT ROUNDTRIP\n");
    }
    
    /* Benchmark */
    printf("\n--- Benchmark ---\n");
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