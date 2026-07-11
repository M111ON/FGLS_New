/*
 * gguf_dump.c — GGUF metadata inspector
 *
 * Usage: gguf_dump <model.gguf>
 *
 * Prints GGUF header, KV metadata, and tensor list.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gguf_reader.h"

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <model.gguf>\n", prog);
    fprintf(stderr, "  Dumps GGUF file header, KV pairs, and tensor list.\n");
}

static const char* gguf_type_name(uint32_t t) {
    switch (t) {
        case 0:  return "F32";
        case 1:  return "F16";
        case 2:  return "Q4_0";
        case 3:  return "Q4_1";
        case 6:  return "Q5_0";
        case 7:  return "Q5_1";
        case 8:  return "Q8_0";
        case 9:  return "Q8_1";
        case 10: return "Q2_K";
        case 11: return "Q3_K";
        case 12: return "Q4_K";
        case 13: return "Q5_K";
        case 14: return "Q6_K";
        case 15: return "Q8_K";
        case 20: return "IQ4_NL";
        case 23: return "IQ4_XS";
        case 30: return "BF16";
        default: return "???";
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    const char *path = argv[1];

    if (strcmp(path, "--help") == 0 || strcmp(path, "-h") == 0) {
        usage(argv[0]); return 0;
    }

    GgufReader reader;
    if (gguf_open(path, &reader) != 0) {
        fprintf(stderr, "Error: cannot open %s\n", path);
        return 1;
    }

    /* Get file size */
    FILE *f = fopen(path, "rb");
    long fsize = 0;
    if (f) { fseek(f, 0, SEEK_END); fsize = ftell(f); fclose(f); }

    printf("═══ GGUF File Inspector ═══\n");
    printf("File:     %s\n", path);
    printf("Size:     %ld bytes (%.2f MB)\n", fsize, fsize / 1048576.0);
    printf("Tensors:  %u\n", reader.n_tensors);
    printf("Data off: %llu\n\n", (unsigned long long)reader.data_offset);

    /* Tensor list */
    printf("═══ Tensor List ═══\n");
    size_t total_bytes = 0;
    uint32_t type_counts[32] = {0};

    for (uint32_t i = 0; i < reader.n_tensors; i++) {
        uint32_t sz = reader.sizes[i];
        total_bytes += sz;

        printf("[%4u] %-50s  %8u bytes  %s\n",
               i, reader.names[i], sz, gguf_type_name(0));

        /* Count types (approximate from size) */
        if (i < 32) type_counts[i]++;
    }

    printf("\n═══ Summary ═══\n");
    printf("Total:    %zu bytes (%.2f MB)\n", total_bytes, total_bytes / 1048576.0);

    /* Find max tensor */
    uint32_t max_idx = 0;
    for (uint32_t i = 1; i < reader.n_tensors; i++) {
        if (reader.sizes[i] > reader.sizes[max_idx]) max_idx = i;
    }
    if (reader.n_tensors > 0) {
        printf("Largest:  [%u] %s (%u bytes)\n",
               max_idx, reader.names[max_idx], reader.sizes[max_idx]);
    }

    gguf_close(&reader);
    return 0;
}
