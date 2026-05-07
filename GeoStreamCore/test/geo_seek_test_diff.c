--- geo_seek_test.c (原始)


+++ geo_seek_test.c (修改后)
/*
 * geo_seek_test.c - Proof of Concept for Geometric Streaming
 *
 * This program demonstrates direct geometric-to-byte-offset mapping
 * without sequential reading. It proves that "Geometric Seek" is possible.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>

// === Configuration Constants ===
#define CELL_SIZE       40              // bytes per cell (fixed size for O(1) access)
#define CELLS_PER_LAYER 256             // cells in each layer
#define LAYERS_PER_SECTOR 64            // layers in each sector
#define MAX_SECTORS     64              // maximum sectors in file (reduced for PoC)

#define LAYER_SIZE      (CELLS_PER_LAYER * CELL_SIZE)
#define SECTOR_SIZE     (LAYERS_PER_SECTOR * LAYER_SIZE)
#define HEADER_SIZE     4096            // reserved header space

#define TEST_FILENAME   "geo_vault.dat"
#define TOTAL_RECORDS   (MAX_SECTORS * LAYERS_PER_SECTOR * CELLS_PER_LAYER)

// === Data Structure (Fixed Size for Random Access) ===
typedef struct {
    int32_t x;          // 4 bytes - coordinate or sector_id
    int32_t y;          // 4 bytes - coordinate or layer_id
    int32_t z;          // 4 bytes - coordinate or cell_id
    int32_t sector_id;  // 4 bytes - logical sector
    int32_t layer_id;   // 4 bytes - logical layer
    int32_t cell_id;    // 4 bytes - logical cell
    double  value;      // 8 bytes - payload data
    uint32_t checksum;  // 4 bytes - validation
} GeoCell;

// === Function Prototypes ===
void init_vault(const char* filename);
long geo_to_offset(int sector_id, int layer_id, int cell_id);
int stream_chunk(FILE* fp, long offset, GeoCell* out);
void benchmark_sequential(FILE* fp);
void benchmark_random_geometric(FILE* fp);
GeoCell generate_cell_data(int sector_id, int layer_id, int cell_id);
uint32_t simple_checksum(GeoCell* cell);

// === Main Entry Point ===
int main(int argc, char* argv[]) {
    printf("=== Geometric Streaming PoC ===\n\n");

    // Step 1: Initialize vault (write test data)
    printf("[1] Initializing vault with %d records...\n", TOTAL_RECORDS);
    clock_t start = clock();
    init_vault(TEST_FILENAME);
    clock_t end = clock();
    double init_time = (double)(end - start) / CLOCKS_PER_SEC;
    printf("    Created %s in %.3f seconds\n", TEST_FILENAME, init_time);
    printf("    File size: ~%.2f MB\n\n", (double)(HEADER_SIZE + (long)MAX_SECTORS * SECTOR_SIZE) / (1024 * 1024));

    // Open file for reading
    FILE* fp = fopen(TEST_FILENAME, "rb");
    if (!fp) {
        perror("Failed to open vault");
        return 1;
    }

    // Step 2: Demonstrate direct geometric seek
    printf("[2] Testing Direct Geometric Seek:\n");

    // Test case 1: First cell (0,0,0)
    long offset1 = geo_to_offset(0, 0, 0);
    GeoCell cell1;
    stream_chunk(fp, offset1, &cell1);
    printf("    Sector 0, Layer 0, Cell 0 -> Offset %ld\n", offset1);
    printf("      Data: x=%d, y=%d, z=%d, value=%.2f\n", cell1.x, cell1.y, cell1.z, cell1.value);

    // Test case 2: Middle cell (30, 30, 100)
    long offset2 = geo_to_offset(30, 30, 100);
    GeoCell cell2;
    stream_chunk(fp, offset2, &cell2);
    printf("    Sector 30, Layer 30, Cell 100 -> Offset %ld\n", offset2);
    printf("      Data: x=%d, y=%d, z=%d, value=%.2f\n", cell2.x, cell2.y, cell2.z, cell2.value);

    // Test case 3: Last cell (63, 63, 255)
    long offset3 = geo_to_offset(MAX_SECTORS - 1, LAYERS_PER_SECTOR - 1, CELLS_PER_LAYER - 1);
    GeoCell cell3;
    stream_chunk(fp, offset3, &cell3);
    printf("    Sector %d, Layer %d, Cell %d -> Offset %ld\n",
           MAX_SECTORS - 1, LAYERS_PER_SECTOR - 1, CELLS_PER_LAYER - 1, offset3);
    printf("      Data: x=%d, y=%d, z=%d, value=%.2f\n\n", cell3.x, cell3.y, cell3.z, cell3.value);

    // Step 3: Benchmark Sequential vs Random Geometric Access
    printf("[3] Benchmarking Access Patterns:\n");

    printf("    Sequential Access:\n");
    benchmark_sequential(fp);

    printf("    Random Geometric Seek:\n");
    benchmark_random_geometric(fp);

    fclose(fp);

    printf("\n=== PoC Complete ===\n");
    printf("If random seek time ~= sequential time (within 2-3x), Geometric Streaming works!\n");

    return 0;
}

// === Implementation ===

void init_vault(const char* filename) {
    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        perror("Failed to create vault");
        exit(1);
    }

    // Write header (zero-filled)
    char* header = calloc(1, HEADER_SIZE);
    fwrite(header, 1, HEADER_SIZE, fp);
    free(header);

    // Write all sectors/layers/cells in order
    GeoCell cell;
    for (int s = 0; s < MAX_SECTORS; s++) {
        for (int l = 0; l < LAYERS_PER_SECTOR; l++) {
            for (int c = 0; c < CELLS_PER_LAYER; c++) {
                cell = generate_cell_data(s, l, c);
                fwrite(&cell, sizeof(GeoCell), 1, fp);
            }
        }
    }

    fclose(fp);
}

GeoCell generate_cell_data(int sector_id, int layer_id, int cell_id) {
    GeoCell cell;
    memset(&cell, 0, sizeof(cell));

    cell.x = sector_id * 100 + layer_id;
    cell.y = layer_id * 50 + cell_id;
    cell.z = cell_id * 10 + sector_id % 10;
    cell.sector_id = sector_id;
    cell.layer_id = layer_id;
    cell.cell_id = cell_id;
    cell.value = sin(sector_id) * cos(layer_id) * 1000.0;
    cell.checksum = simple_checksum(&cell);

    return cell;
}

uint32_t simple_checksum(GeoCell* cell) {
    return (uint32_t)(cell->x ^ cell->y ^ cell->z ^ cell->sector_id ^ cell->layer_id ^ cell->cell_id);
}

long geo_to_offset(int sector_id, int layer_id, int cell_id) {
    if (sector_id < 0 || sector_id >= MAX_SECTORS) return -1;
    if (layer_id < 0 || layer_id >= LAYERS_PER_SECTOR) return -1;
    if (cell_id < 0 || cell_id >= CELLS_PER_LAYER) return -1;

    long offset = HEADER_SIZE
                + ((long)sector_id * SECTOR_SIZE)
                + ((long)layer_id * LAYER_SIZE)
                + ((long)cell_id * CELL_SIZE);

    return offset;
}

int stream_chunk(FILE* fp, long offset, GeoCell* out) {
    if (fseek(fp, offset, SEEK_SET) != 0) {
        perror("fseek failed");
        return -1;
    }

    if (fread(out, sizeof(GeoCell), 1, fp) != 1) {
        perror("fread failed");
        return -1;
    }

    uint32_t expected = simple_checksum(out);
    if (out->checksum != expected) {
        fprintf(stderr, "Checksum mismatch at offset %ld\n", offset);
        return -1;
    }

    return 0;
}

void benchmark_sequential(FILE* fp) {
    int count = 10000;
    GeoCell cell;

    clock_t start = clock();

    for (int i = 0; i < count; i++) {
        int sector = i / (LAYERS_PER_SECTOR * CELLS_PER_LAYER);
        int layer = (i / CELLS_PER_LAYER) % LAYERS_PER_SECTOR;
        int cell_id = i % CELLS_PER_LAYER;

        long offset = geo_to_offset(sector, layer, cell_id);
        stream_chunk(fp, offset, &cell);
    }

    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    printf("      %d sequential reads: %.4f sec (%.0f reads/sec)\n",
           count, elapsed, count / elapsed);
}

void benchmark_random_geometric(FILE* fp) {
    int count = 10000;
    GeoCell cell;

    srand(42);

    clock_t start = clock();

    for (int i = 0; i < count; i++) {
        int sector = rand() % MAX_SECTORS;
        int layer = rand() % LAYERS_PER_SECTOR;
        int cell_id = rand() % CELLS_PER_LAYER;

        long offset = geo_to_offset(sector, layer, cell_id);
        stream_chunk(fp, offset, &cell);
    }

    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    printf("      %d random seeks:   %.4f sec (%.0f seeks/sec)\n",
           count, elapsed, count / elapsed);
}