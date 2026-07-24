/*
 * analyze_weights.c — Analyze GGUF Q8_0 weight distribution
 * 
 * Reads Q8_0 tensors and checks:
 * - Value distribution (histogram)
 * - Clustering (are values concentrated?)
 * - Delta encoding potential
 * - Run-length encoding potential
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* GGUF magic */
#define GGUF_MAGIC 0x46554747  /* "GGUF" */

/* GGUF types */
#define GGUF_TYPE_UINT8    0
#define GGUF_TYPE_INT8     1
#define GGUF_TYPE_UINT16   2
#define GGUF_TYPE_INT16    3
#define GGUF_TYPE_UINT32   4
#define GGUF_TYPE_INT32    5
#define GGUF_TYPE_FLOAT32  6
#define GGUF_TYPE_BOOL     7
#define GGUF_TYPE_STRING   8
#define GGUF_TYPE_ARRAY    9
#define GGUF_TYPE_UINT64   10
#define GGUF_TYPE_INT64    11
#define GGUF_TYPE_FLOAT64  12

/* Tensor types */
#define GGML_TYPE_Q8_0     7

/* Stats */
typedef struct {
    uint64_t total_values;
    uint64_t histogram[256];  /* Q8_0: -128..127 mapped to 0..255 */
    double min_val;
    double max_val;
    double mean;
    double variance;
    uint64_t distinct_values;
    uint64_t zero_count;
    
    /* Delta stats */
    double avg_abs_delta;
    double max_abs_delta;
    uint64_t small_deltas;  /* delta < 2 */
    uint64_t zero_deltas;   /* delta == 0 */
    
    /* Run-length stats */
    uint64_t max_run;
    uint64_t total_runs;
    double avg_run_length;
} WeightStats;

static void stats_init(WeightStats *s) {
    memset(s, 0, sizeof(WeightStats));
    s->min_val = 1e9;
    s->max_val = -1e9;
}

static void stats_add_value(WeightStats *s, int8_t val) {
    uint8_t idx = (uint8_t)val;
    s->histogram[idx]++;
    s->total_values++;
    
    double dv = (double)val;
    if (dv < s->min_val) s->min_val = dv;
    if (dv > s->max_val) s->max_val = dv;
    s->mean += dv;
    s->variance += dv * dv;
    
    if (val == 0) s->zero_count++;
}

static void stats_finalize(WeightStats *s) {
    if (s->total_values > 0) {
        s->mean /= (double)s->total_values;
        s->variance = s->variance / (double)s->total_values - s->mean * s->mean;
    }
    
    /* Count distinct values */
    for (int i = 0; i < 256; i++) {
        if (s->histogram[i] > 0) s->distinct_values++;
    }
}

static void stats_analyze_deltas(WeightStats *s, int8_t *values, uint64_t count) {
    if (count < 2) return;
    
    uint64_t total_abs_delta = 0;
    uint64_t max_abs = 0;
    uint64_t small = 0;
    uint64_t zero = 0;
    
    for (uint64_t i = 1; i < count; i++) {
        int delta = abs((int)values[i] - (int)values[i-1]);
        total_abs_delta += delta;
        if (delta > max_abs) max_abs = delta;
        if (delta < 2) small++;
        if (delta == 0) zero++;
    }
    
    s->avg_abs_delta = (double)total_abs_delta / (double)(count - 1);
    s->max_abs_delta = (double)max_abs;
    s->small_deltas = small;
    s->zero_deltas = zero;
}

static void stats_analyze_runs(WeightStats *s, int8_t *values, uint64_t count) {
    if (count == 0) return;
    
    uint64_t current_run = 1;
    uint64_t max_run = 1;
    uint64_t total_runs = 1;
    
    for (uint64_t i = 1; i < count; i++) {
        if (values[i] == values[i-1]) {
            current_run++;
        } else {
            if (current_run > max_run) max_run = current_run;
            total_runs++;
            current_run = 1;
        }
    }
    if (current_run > max_run) max_run = current_run;
    
    s->max_run = max_run;
    s->total_runs = total_runs;
    s->avg_run_length = (double)count / (double)total_runs;
}

static void stats_print(WeightStats *s) {
    printf("=== Weight Distribution Analysis ===\n");
    printf("Total values:    %lu\n", s->total_values);
    printf("Distinct values: %lu / 256\n", s->distinct_values);
    printf("Range:           %.0f to %.0f\n", s->min_val, s->max_val);
    printf("Mean:            %.2f\n", s->mean);
    printf("StdDev:          %.2f\n", sqrt(s->variance));
    printf("Zero count:      %lu (%.1f%%)\n", s->zero_count, 
           100.0 * s->zero_count / s->total_values);
    printf("\n");
    
    printf("=== Histogram (top 20) ===\n");
    /* Find top 20 most common values */
    typedef struct { int val; uint64_t count; } ValCount;
    ValCount top[256];
    for (int i = 0; i < 256; i++) {
        top[i].val = (int)(int8_t)(uint8_t)i;
        top[i].count = s->histogram[i];
    }
    /* Simple selection sort for top 20 */
    for (int i = 0; i < 20; i++) {
        int max_idx = i;
        for (int j = i + 1; j < 256; j++) {
            if (top[j].count > top[max_idx].count) max_idx = j;
        }
        ValCount tmp = top[i]; top[i] = top[max_idx]; top[max_idx] = tmp;
    }
    for (int i = 0; i < 20 && top[i].count > 0; i++) {
        printf("  %4d: %10lu (%.2f%%)\n", top[i].val, top[i].count,
               100.0 * top[i].count / s->total_values);
    }
    printf("\n");
    
    printf("=== Delta Encoding Potential ===\n");
    printf("Avg abs delta:   %.2f\n", s->avg_abs_delta);
    printf("Max abs delta:   %.0f\n", s->max_abs_delta);
    printf("Small deltas (<2): %lu (%.1f%%)\n", s->small_deltas,
           100.0 * s->small_deltas / (s->total_values > 1 ? s->total_values - 1 : 1));
    printf("Zero deltas:     %lu (%.1f%%)\n", s->zero_deltas,
           100.0 * s->zero_deltas / (s->total_values > 1 ? s->total_values - 1 : 1));
    printf("\n");
    
    printf("=== Run-Length Potential ===\n");
    printf("Total runs:      %lu\n", s->total_runs);
    printf("Avg run length:  %.2f\n", s->avg_run_length);
    printf("Max run length:  %lu\n", s->max_run);
    printf("\n");
    
    /* Compression estimate */
    double entropy = 0;
    for (int i = 0; i < 256; i++) {
        if (s->histogram[i] > 0) {
            double p = (double)s->histogram[i] / (double)s->total_values;
            entropy -= p * log2(p);
        }
    }
    printf("=== Compression Potential ===\n");
    printf("Shannon entropy: %.2f bits/value\n", entropy);
    printf("Raw Q8_0:        8.00 bits/value\n");
    printf("Theoretical min: %.2f bits/value\n", entropy);
    printf("Savings:         %.1f%%\n", 100.0 * (1.0 - entropy / 8.0));
}

/* Simple GGUF reader - reads weight tensors only */
static int analyze_gguf(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open: %s\n", filename);
        return 1;
    }
    
    /* Read header */
    uint32_t magic;
    fread(&magic, 4, 1, f);
    if (magic != GGUF_MAGIC) {
        fprintf(stderr, "Not a GGUF file (magic: 0x%08X)\n", magic);
        fclose(f);
        return 1;
    }
    
    uint32_t version;
    fread(&version, 4, 1, f);
    printf("GGUF version: %u\n", version);
    
    uint64_t n_tensors;
    fread(&n_tensors, 8, 1, f);
    printf("Tensors: %lu\n", n_tensors);
    
    uint64_t n_kv;
    fread(&n_kv, 8, 1, f);
    printf("Metadata KV pairs: %lu\n\n", n_kv);
    
    /* Skip metadata (simplified - just skip forward) */
    /* In reality we'd parse KV pairs, but for analysis we just need tensor data */
    
    /* For now, let's just read the first tensor's raw data */
    /* This is a simplified approach - real GGUF parsing is more complex */
    
    /* Seek to end of header area (approximate) */
    /* Actually, let's just read raw bytes and look for Q8_0 patterns */
    
    /* Read a chunk of the file and analyze */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    printf("File size: %.1f MB\n\n", file_size / (1024.0 * 1024.0));
    
    /* Read tensor data (skip header, read weight data) */
    /* Header is roughly: magic(4) + version(4) + n_tensors(8) + n_kv(8) + metadata + tensor_info */
    /* For simplicity, skip first 1MB and analyze the rest */
    
    long header_skip = 1024 * 1024;  /* Skip 1MB header */
    if (header_skip >= file_size) {
        fprintf(stderr, "File too small\n");
        fclose(f);
        return 1;
    }
    
    fseek(f, header_skip, SEEK_SET);
    
    long data_size = file_size - header_skip;
    int8_t *buffer = (int8_t *)malloc(data_size);
    if (!buffer) {
        fprintf(stderr, "Cannot allocate %ld bytes\n", data_size);
        fclose(f);
        return 1;
    }
    
    size_t read = fread(buffer, 1, data_size, f);
    fclose(f);
    
    printf("Analyzing %zu bytes of weight data...\n\n", read);
    
    /* Analyze */
    WeightStats stats;
    stats_init(&stats);
    
    /* Analyze in chunks to get delta stats */
    uint64_t chunk_size = 1000000;  /* 1M values per chunk */
    int8_t *chunk = (int8_t *)malloc(chunk_size);
    
    uint64_t total_analyzed = 0;
    for (uint64_t offset = 0; offset < read; offset += chunk_size) {
        uint64_t this_chunk = chunk_size;
        if (offset + this_chunk > read) this_chunk = read - offset;
        
        /* Add values to stats */
        for (uint64_t i = 0; i < this_chunk; i++) {
            stats_add_value(&stats, buffer[offset + i]);
        }
        
        /* Analyze deltas for this chunk */
        stats_analyze_deltas(&stats, buffer + offset, this_chunk);
        stats_analyze_runs(&stats, buffer + offset, this_chunk);
        
        total_analyzed += this_chunk;
        
        if (total_analyzed % 10000000 == 0) {
            printf("  Analyzed %lu / %zu values...\n", total_analyzed, read);
        }
    }
    
    stats_finalize(&stats);
    stats_print(&stats);
    
    free(buffer);
    free(chunk);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <gguf_file>\n", argv[0]);
        printf("\nExample:\n");
        printf("  %s I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf\n", argv[0]);
        return 1;
    }
    
    return analyze_gguf(argv[1]);
}
