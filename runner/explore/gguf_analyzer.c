/**
 * gguf_analyzer.c — Simple GGUF analyzer
 * Usage: gguf_analyzer.exe <model.gguf>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define MAX_TENSORS 200
#define BLOCK_SIZE 64
#define GRID_DIM 144
#define TOTAL_CELLS (GRID_DIM * GRID_DIM)

typedef struct {
    char name[128];
    uint32_t n_dims;
    uint32_t dims[4];
    uint32_t type;
    uint64_t size;
    float entropy;
    int tier;
} TensorInfo;

static float calc_entropy(const uint8_t *data, size_t len) {
    if (len == 0) return 0.0f;
    uint32_t hist[256] = {0};
    for (size_t i = 0; i < len; i++) hist[data[i]]++;
    float e = 0.0f;
    for (int i = 0; i < 256; i++) {
        if (hist[i] > 0) {
            float p = (float)hist[i] / len;
            e -= p * log2f(p);
        }
    }
    return e * (255.0f / 8.0f);
}

static int tier_from_score(int s) {
    if (s < 64) return 0;
    if (s < 128) return 1;
    if (s < 192) return 2;
    return 3;
}

/* Skip a GGUF value */
static void skip_value(FILE *f, uint32_t type) {
    switch (type) {
        case 0: case 1: case 7: fseek(f, 1, SEEK_CUR); break;
        case 2: case 3: case 10: fseek(f, 2, SEEK_CUR); break;
        case 4: case 5: case 6: fseek(f, 4, SEEK_CUR); break;
        case 8: { uint64_t len; fread(&len, 8, 1, f); fseek(f, len, SEEK_CUR); break; }
        case 9: {
            uint32_t et; uint64_t n;
            fread(&et, 4, 1, f); fread(&n, 8, 1, f);
            for (uint64_t i = 0; i < n; i++) skip_value(f, et);
            break;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <model.gguf>\n", argv[0]); return 1; }
    
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", argv[1]); return 1; }
    
    /* Read GGUF header */
    uint32_t magic, version, n_tensors_raw, n_kv;
    fread(&magic, 4, 1, f);
    fread(&version, 4, 1, f);
    fread(&n_tensors_raw, 4, 1, f);
    fread(&n_kv, 4, 1, f);
    
    if (magic != 0x46554747) { fprintf(stderr, "Not GGUF\n"); fclose(f); return 1; }
    
    fprintf(stderr, "GGUF v%u, %u tensors, %u kv pairs\n", version, n_tensors_raw, n_kv);
    
    /* Skip KV pairs */
    for (uint32_t i = 0; i < n_kv; i++) {
        uint64_t key_len; fread(&key_len, 8, 1, f);
        fseek(f, key_len, SEEK_CUR);
        uint32_t val_type; fread(&val_type, 4, 1, f);
        skip_value(f, val_type);
    }
    
    /* Read tensors */
    int n_tensors = (n_tensors_raw < MAX_TENSORS) ? n_tensors_raw : MAX_TENSORS;
    TensorInfo *tensors = calloc(n_tensors, sizeof(TensorInfo));
    
    for (int i = 0; i < n_tensors; i++) {
        uint32_t name_len; fread(&name_len, 4, 1, f);
        if (name_len > 127) name_len = 127;
        fread(tensors[i].name, 1, name_len, f);
        tensors[i].name[name_len] = 0;
        fread(&tensors[i].n_dims, 4, 1, f);
        for (uint32_t d = 0; d < tensors[i].n_dims && d < 4; d++)
            fread(&tensors[i].dims[d], 4, 1, f);
        fread(&tensors[i].type, 4, 1, f);
    }
    
    fprintf(stderr, "Read %d tensors\n", n_tensors);
    
    /* Get file size */
    fseek(f, 0, SEEK_END);
    uint64_t file_size = ftell(f);
    rewind(f);
    
    /* Process tensors */
    int tier_counts[4] = {0};
    int entropy_hist[256] = {0};
    
    for (int i = 0; i < n_tensors; i++) {
        uint64_t tsz = 1;
        for (uint32_t d = 0; d < tensors[i].n_dims; d++) tsz *= tensors[i].dims[d];
        
        uint32_t type_sz = 4;
        switch (tensors[i].type) {
            case 6: type_sz = 4; break;
            case 10: type_sz = 2; break;
            case 8: case 9: type_sz = 1; break;
        }
        tensors[i].size = tsz * type_sz;
        
        /* Sample from file */
        uint8_t sample[8192];
        size_t sample_sz = 8192;
        uint64_t read_pos = 256 + (i * 2048);
        if (read_pos + sample_sz > file_size) read_pos = 256;
        
        fseek(f, read_pos, SEEK_SET);
        size_t nread = fread(sample, 1, sample_sz, f);
        if (nread > 0) {
            tensors[i].entropy = calc_entropy(sample, nread);
            tensors[i].tier = tier_from_score((int)tensors[i].entropy);
        } else {
            tensors[i].entropy = 0;
            tensors[i].tier = 0;
        }
        
        tier_counts[tensors[i].tier]++;
        int hi = (int)tensors[i].entropy;
        if (hi >= 0 && hi < 256) entropy_hist[hi]++;
    }
    
    fclose(f);
    
    fprintf(stderr, "Tier counts: T0=%d T1=%d T2=%d T3=%d\n",
            tier_counts[0], tier_counts[1], tier_counts[2], tier_counts[3]);
    
    /* Block allocation */
    int block_alloc[TOTAL_CELLS];
    memset(block_alloc, 0, sizeof(block_alloc));
    for (int i = 0; i < n_tensors; i++) {
        int nb = (tensors[i].size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        if (nb > 500) nb = 500;
        for (int b = 0; b < nb; b++) {
            int idx = (i * 37 + b * 13) % TOTAL_CELLS;
            block_alloc[idx] = tensors[i].tier;
        }
    }
    
    /* Output JSON to stdout */
    printf("{\n");
    printf("  \"tier_distribution\": {\"tier0\":%d,\"tier1\":%d,\"tier2\":%d,\"tier3\":%d},\n",
           tier_counts[0], tier_counts[1], tier_counts[2], tier_counts[3]);
    
    printf("  \"entropy_histogram\": [");
    for (int i = 0; i < 256; i++) printf("%d%s", entropy_hist[i], i < 255 ? "," : "");
    printf("],\n");
    
    printf("  \"tensors\": [\n");
    for (int i = 0; i < n_tensors; i++) {
        printf("    {\"name\":\"%s\",\"tier\":%d,\"entropy\":%.0f,\"size\":%I64u}",
               tensors[i].name, tensors[i].tier, tensors[i].entropy,
               (unsigned long long)tensors[i].size);
        if (i < n_tensors - 1) printf(",");
        printf("\n");
    }
    printf("  ],\n");
    
    printf("  \"block_allocation\": [");
    for (int i = 0; i < TOTAL_CELLS; i++) printf("%d%s", block_alloc[i], i < TOTAL_CELLS - 1 ? "," : "");
    printf("],\n");
    
    printf("  \"container_stats\": {\n");
    printf("    \"file\":\"%s\",\"file_size\":%I64u,\"n_tensors\":%d,\"block_size\":%d,\"total_cells\":%d,\"grid_dim\":%d\n",
           argv[1], (unsigned long long)file_size, n_tensors, BLOCK_SIZE, TOTAL_CELLS, GRID_DIM);
    printf("  }\n}\n");
    
    free(tensors);
    return 0;
}
