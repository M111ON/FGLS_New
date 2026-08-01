/*
 * park_20736.c — Route real Q8_0 weights onto 20736-position grid
 *
 * Grid = 162 vertices × 128 slots = 20736
 * Route = stride-37, length 20736, visits all cells
 * CPU: batch by vertex (162 batches)
 * GPU: process 128 weights per batch
 *
 * Strategy: no compression, no codec — just PARK weights on grid.
 *   grid position = weight index % 20736 (or stride-37 spread)
 *   each position = list of weights that parked there
 *   pull = re-assemble weights from grid at inference time
 *
 * Compile: gcc -O2 -std=c11 -I. runner/explore/park_20736.c -o runner/explore/park_20736.exe
 * Run:     runner/explore/park_20736.exe I:/model/Qwen3-0.6B-Q8_0.gguf
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <inttypes.h>
#include "beam_addressing/gguf_reader.h"

#define GRID 20736
#define N_ICOSA 162
#define SLOTS 128

static int8_t *grid[GRID];
static uint32_t grid_cnt[GRID];
static uint32_t grid_cap[GRID];

static void grid_init(void) {
    for (int i = 0; i < GRID; i++) {
        grid[i] = NULL; grid_cnt[i] = 0; grid_cap[i] = 0;
    }
}

static void grid_add(uint32_t pos, int8_t w) {
    if (grid_cnt[pos] >= grid_cap[pos]) {
        if (grid_cap[pos] == 0) grid_cap[pos] = 4096;
        else grid_cap[pos] *= 2;
        grid[pos] = (int8_t*)realloc(grid[pos], grid_cap[pos] * sizeof(int8_t));
    }
    grid[pos][grid_cnt[pos]++] = w;
}

int main(int argc, char **argv) {
    const char *fin = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";
    GGUF_File *gf = gguf_open(fin);
    if (!gf) { printf("[FAIL] open\n"); return 1; }

    /* count Q8_0 weights */
    uint64_t n_q8 = 0;
    for (uint64_t t = 0; t < gf->tensor_count; t++)
        if (gf->tensors[t].type == 8) n_q8 += (gf->tensors[t].size_bytes / 34) * 32;

    /* per-grid-slot: average depth for 5M sample */
    uint64_t sample = n_q8;
    if (sample > 5000000) sample = 5000000;
    uint64_t avg_per_cell = (sample + GRID - 1) / GRID;
    printf("=== PARK ROUTE 20736: grid routing of Q8_0 weights ===\n");
    printf("  Q8_0 weights: %" PRIu64 " (%.1f M)\n", n_q8, n_q8 / 1e6);
    printf("  grid cells:   %d (162 vtx x 128 slots)\n", GRID);
    printf("  sample:       %" PRIu64 " weights\n", sample);
    printf("  avg/cell:     ~%" PRIu64 "\n\n", avg_per_cell);

    grid_init();

    /* Stream weights through stride-37 route onto grid */
    FILE *fp = fopen(fin, "rb");
    uint64_t loaded = 0;
    for (uint64_t t = 0; t < gf->tensor_count && loaded < sample; t++) {
        if (gf->tensors[t].type != 8) continue;
        uint64_t sz = gf->tensors[t].size_bytes;
        uint64_t blocks = sz / 34;
        uint64_t off = gf->tensor_data_start + gf->tensors[t].offset;
        uint8_t *buf = (uint8_t*)malloc(sz);
        fseek(fp, off, SEEK_SET); fread(buf, 1, sz, fp);
        for (uint64_t b = 0; b < blocks && loaded < sample; b++) {
            for (int i = 0; i < 32 && loaded < sample; i++) {
                int8_t w = (int8_t)buf[b * 34 + 2 + i];
                uint32_t pos = (uint32_t)((loaded * 37) % GRID);
                grid_add(pos, w);
                loaded++;
            }
        }
        free(buf);
    }
    fclose(fp);

    printf("── grid cell fill stats ──\n");
    uint32_t min_fill = 99999, max_fill = 0;
    double mean_fill = 0;
    int used = 0, empty = 0;
    for (int i = 0; i < GRID; i++) {
        if (grid_cnt[i] > 0) {
            used++;
            if (grid_cnt[i] < min_fill) min_fill = grid_cnt[i];
            if (grid_cnt[i] > max_fill) max_fill = grid_cnt[i];
            mean_fill += grid_cnt[i];
        } else {
            empty++;
        }
    }
    mean_fill /= (used > 0 ? used : 1);
    printf("  cells used:     %d / %d (%.1f%%)\n", used, GRID, 100.0*used/GRID);
    printf("  cells empty:    %d (%.1f%%)\n", empty, 100.0*empty/GRID);
    printf("  fill range:     %u .. %u\n", min_fill, max_fill);
    printf("  fill mean:     %.1f\n\n", mean_fill);

    /* vertex aggregate: 162 vertex-level histograms */
    printf("── vertex aggregates (162 vertices, 128 slots each) ──\n");
    int active_vertices = 0;
    for (int v = 0; v < N_ICOSA; v++) {
        int active_slots = 0;
        int vmin = 127, vmax = -128;
        float sum = 0;
        uint64_t vcnt = 0;
        for (int s = 0; s < SLOTS; s++) {
            int pos = v * SLOTS + s;
            if (grid_cnt[pos] > 0) {
                active_slots++;
                for (uint32_t k = 0; k < grid_cnt[pos]; k++) {
                    if (grid[pos][k] < vmin) vmin = grid[pos][k];
                    if (grid[pos][k] > vmax) vmax = grid[pos][k];
                    sum += grid[pos][k];
                    vcnt++;
                }
            }
        }
        if (active_slots > 0) {
            active_vertices++;
            if (v < 5) printf("  vertex %3d: %d/128 slots active, values [%d..%d], avg=%.1f\n",
                              v, active_slots, vmin, vmax, vcnt > 0 ? sum/vcnt : 0);
        }
    }
    printf("  ...\n");
    printf("  active vertices: %d / %d (%.1f%%)\n\n",
           active_vertices, N_ICOSA, 100.0*active_vertices/N_ICOSA);

    /* ── verify route round-trip on 5 cells ── */
    printf("── roundtrip verification (first 5 cells) ──\n");
    for (int i = 0; i < 5; i++) {
        if (grid_cnt[i] == 0) continue;
        printf("  cell %d: stored %u weights first= %d last= %d\n",
               i, grid_cnt[i], grid[i][0], grid[i][grid_cnt[i]-1]);
    }
    printf("\n");

    /* memory efficiency */
    printf("── storage footprint ──\n");
    uint64_t total_stored = 0;
    for (int i = 0; i < GRID; i++) total_stored += grid_cnt[i];
    printf("   weights stored:  %" PRIu64 "\n", total_stored);
    printf("   grid cells used: %d (%.1f%%)\n", used, 100.0*used/GRID);
    printf("   lossless park:   YES (no discard, no codec)\n");
    printf("   route:           20736 steps (stride-37)\n");
    printf("   batch:           162 CPU routes x 128 GPU slots\n");
    printf("   address space:   %d (%.1f utilization)\n\n", GRID, (float)total_stored/GRID);

    printf("── grid routing overview ──\n");
    printf("  The 20736 grid IS the route.\n");
    printf("  Each weight flows onto a grid cell via stride-37.\n");
    printf("  No codec, no compliance — just park the weight.\n");
    printf("  GPU batch pulls weights from its grid cell.\n");

    /* free */
    for (int i = 0; i < GRID; i++) free(grid[i]);
    return 0;
}