/* Minimal C test for geofield_full_decompress */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Copy key structures and function declarations */
#define GFCS_MAX_CODEBOOK 256
#define GFCS_HDR_SZ 34
#define GFCS_IDX_ENTRY_SZ 12
#define DS_SUB_N 8
#define DS_SUB_SZ 8

typedef struct {
    uint8_t rot;
    uint8_t sub_flags;
} GCFSPattern;

typedef struct {
    uint64_t orig_size;
    uint32_t n_segments;
    uint32_t n_patterns;
    uint32_t n_blocks;
    uint64_t comp_size;
    uint64_t total_out;
    uint64_t xxh64;
    uint32_t structure_ms;
    double wall_ms;
    double ratio;
    uint32_t diamond_hits[6];
    uint32_t skel_hits[5];
} GFCSStats;

extern __declspec(dllimport) int geofield_full_compress(
    const uint8_t *data, uint64_t data_size,
    uint32_t min_chunk, uint32_t max_chunk,
    uint8_t *out_buf, uint64_t out_buf_sz,
    GFCSStats *out_stats);

extern __declspec(dllimport) int geofield_full_decompress(
    const uint8_t *in_buf, uint64_t in_sz,
    uint8_t *out_buf, uint64_t out_buf_sz,
    uint64_t *out_xxh64);

int main(void)
{
    uint8_t data[800];
    memset(data, 'x', 800);

    GFCSStats stats;
    memset(&stats, 0, sizeof(stats));

    int rc = geofield_full_compress(data, 800, 32, 4096, NULL, 0, &stats);
    printf("dry run: rc=%d total_out=%llu n_blocks=%d n_patterns=%d\n",
           rc, (unsigned long long)stats.total_out, stats.n_blocks, stats.n_patterns);

    uint8_t *comp = malloc((size_t)stats.total_out);
    rc = geofield_full_compress(data, 800, 32, 4096, comp, stats.total_out, &stats);
    printf("compress: rc=%d total_out=%llu\n", rc, (unsigned long long)stats.total_out);

    uint64_t got_xxh = 0;
    uint8_t *out = malloc(800);
    memset(out, 0, 800);
    printf("Calling geofield_full_decompress...\n"); fflush(stdout);
    rc = geofield_full_decompress(comp, stats.total_out, out, 800, &got_xxh);
    printf("decompress: rc=%d xxh=0x%llx\n", rc, (unsigned long long)got_xxh);

    int match = (memcmp(data, out, 800) == 0);
    printf("match: %d\n", match);
    free(comp);
    free(out);
    printf("DONE\n");
    return (match && rc == 0) ? 0 : 1;
}
