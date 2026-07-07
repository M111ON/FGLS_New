/*
 * demo_radial_capture.c — Demo radial capture with real GGUF tensor names
 *
 * Reads tensor names from a .gguf model file via the ggml/gguf API.
 * 
 * Build:
 *   gcc -O2 -std=c11 -I. -I../collection -II:/llama.cpp/ggml/include \
 *       -o demo_radial_capture.exe demo_radial_capture.c \
 *       -L. -lggml -lggml-base -lggml-cpu -lstdc++ -lm
 *
 * Run:
 *   ./demo_radial_capture.exe I:/model/LFM2.5-8B-A1B-Q4_K_M.gguf
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "capture_radial.h"
#include "ggml.h"
#include "gguf.h"

static void print_distribution(const CradResult *cr) {
    int vert_counts[24] = {0};
    int node_counts[7] = {0};
    int a_count = 0, b_count = 0;
    uint64_t hash_counts[168] = {0};

    for (uint32_t i = 0; i < cr->n_captured; i++) {
        uint64_t base = cr->entries[i].addr & ~RC_TWIN_BIT;
        int vi = (int)(base / RC_N_NODES);
        int ni = (int)(base % RC_N_NODES);
        if (vi >= 0 && vi < 24) vert_counts[vi]++;
        if (ni >= 0 && ni < 7) node_counts[ni]++;
        if (cr->entries[i].addr & RC_TWIN_BIT) b_count++;
        else a_count++;
        hash_counts[base]++;
    }

    printf("Twin bit: A=%d, B=%d\n", a_count, b_count);

    printf("\nVertex distribution (24 faces):\n");
    for (int i = 0; i < 24; i++)
        if (vert_counts[i] > 0)
            printf("  v%02d (%s): %d\n", i, i < 12 ? "A" : "B", vert_counts[i]);

    printf("\nNode distribution (7 radial lines):\n");
    for (int i = 0; i < 7; i++)
        if (node_counts[i] > 0)
            printf("  n%d (%3.0f°): %d\n", i, RC_ANGLES_DEG[i], node_counts[i]);

    int collisions = 0, max_col = 0, used = 0;
    for (int i = 0; i < 168; i++) {
        if (hash_counts[i] > 1) {
            collisions += (int)(hash_counts[i] - 1);
            if (hash_counts[i] > (uint64_t)max_col)
                max_col = (int)hash_counts[i];
        }
        if (hash_counts[i] > 0) used++;
    }
    printf("\nUnique slots: %d/168, collisions: %d (max %d-way)\n",
           used, collisions, max_col);
}

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1] :
        "I:/model/LFM2.5-8B-A1B-Q4_K_M.gguf";

    printf("=== Radial Capture with GGUF ===\n");
    printf("Model: %s\n", model_path);

    /* Open GGUF */
    struct gguf_init_params params = { .no_alloc = 1 };
    struct gguf_context *ctx = gguf_init_from_file(model_path, params);
    if (!ctx) {
        fprintf(stderr, "ERROR: cannot open %s\n", model_path);
        return 1;
    }
    int n = gguf_get_n_tensors(ctx);
    printf("Tensors: %d\n\n", n);

    /* Capture all tensor names */
    CradResult cr;
    crad_init(&cr);
    for (int i = 0; i < n; i++) {
        const char *name = gguf_get_tensor_name(ctx, i);
        if (name) crad_capture(&cr, name);
    }
    printf("[crad] %u tensors captured\n", cr.n_captured);

    /* Verify determinism */
    crad_verify(&cr, NULL, 0);
    printf("[crad] verification: %s (%u tensors)\n",
           cr.lossless_ok ? "OK" : "FAIL", cr.n_verified);

    /* Distribution */
    print_distribution(&cr);

    /* Write store */
    crad_write_store(&cr, ".");

    gguf_free(ctx);
    return 0;
}
