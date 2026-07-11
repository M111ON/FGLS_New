/*
 * dramtile_dump.c — DRamTile store stats dumper
 *
 * Usage: dramtile_dump <store_dir> [--twin <file>]
 *
 * Loads a DRamTile store, prints hash stats, capacity, fragmentation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dramtile_store.h"
#include "pogls_core.h"

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [--twin <file>] [--cold <file>]\n", prog);
    fprintf(stderr, "  Loads DRamTile store, prints stats.\n");
    fprintf(stderr, "  --twin    File-backed twin path\n");
    fprintf(stderr, "  --cold    Cold spill twin path\n");
}

int main(int argc, char **argv) {
    const char *twin_path = NULL;
    const char *cold_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--twin") == 0 && i + 1 < argc) twin_path = argv[++i];
        else if (strcmp(argv[i], "--cold") == 0 && i + 1 < argc) cold_path = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { usage(argv[0]); return 0; }
    }

    DRamTileStore store;
    memset(&store, 0, sizeof(store));

    if (twin_path) {
        if (dt_store_init_twin(&store, twin_path, 0) != 0) {
            fprintf(stderr, "Error: cannot load twin %s\n", twin_path);
            return 1;
        }
    } else if (cold_path) {
        if (dt_store_init_cold_twin(&store, cold_path, 0) != 0) {
            fprintf(stderr, "Error: cannot load cold twin %s\n", cold_path);
            return 1;
        }
    } else {
        /* Just init an anonymous store for inspection */
        if (dt_store_init(&store, 0) != 0) {
            fprintf(stderr, "Error: cannot init store\n");
            return 1;
        }
    }

    /* Count stats */
    int n_stored = 0, n_kv = 0, n_bond = 0, n_empty = 0;
    size_t total_weight = 0, total_kv = 0;
    for (uint32_t i = 0; i < DT_HASH_SLOTS; i++) {
        uint32_t addr = store.hash[i].dram_addr;
        if (addr == 0) { n_empty++; continue; }
        if (addr & DT_KV_FLAG) { n_kv++; total_kv += store.hash[i].size; }
        else if (addr & DT_BOND_FLAG) { n_bond++; }
        else { n_stored++; total_weight += store.hash[i].size; }
    }

    printf("═══ DRamTile Store Stats ═══\n");
    printf("Capacity:    %zu bytes (%.2f MB)\n", store.capacity, store.capacity / 1048576.0);
    printf("Used:        %zu bytes (%.2f MB)\n", store.used, store.used / 1048576.0);
    printf("Free:        %zu bytes (%.2f MB)\n", store.capacity - store.used, (store.capacity - store.used) / 1048576.0);
    printf("Fill:        %.1f%%\n", store.capacity > 0 ? 100.0 * store.used / store.capacity : 0.0);
    printf("\n");
    printf("Hash slots:  %d\n", DT_HASH_SLOTS);
    printf("  weight:   %d entries (%zu bytes)\n", n_stored, total_weight);
    printf("  kv:       %d entries (%zu bytes)\n", n_kv, total_kv);
    printf("  bond:     %d entries\n", n_bond);
    printf("  empty:    %d slots\n", n_empty);
    printf("  total:    %u stored\n", store.n_stored);
    printf("\n");

    /* KV region */
    if (store.kv_capacity > 0) {
        printf("KV region:   %zu bytes (%.2f MB)\n", store.kv_capacity, store.kv_capacity / 1048576.0);
        printf("KV used:     %zu bytes (%.2f MB)\n", store.kv_used, store.kv_used / 1048576.0);
        printf("KV fill:     %.1f%%\n", 100.0 * store.kv_used / store.kv_capacity);
    }

    /* Cold region */
    if (store.cold_capacity > 0) {
        printf("Cold region: %zu bytes (%.2f MB)\n", store.cold_capacity, store.cold_capacity / 1048576.0);
        printf("Cold used:   %zu bytes (%.2f MB)\n", store.cold_used, store.cold_used / 1048576.0);
        printf("Cold fill:   %.1f%%\n", 100.0 * store.cold_used / store.cold_capacity);
        printf("Cold twin:   %s\n", store.is_cold_twin ? "YES" : "NO");
    }

    /* Free list */
    if (store.free_count > 0) {
        printf("\nFree list:   %d entries\n", store.free_count);
        size_t free_total = 0;
        for (int i = 0; i < store.free_count; i++) free_total += store.free_sizes[i];
        printf("Free total:  %zu bytes (%.2f MB)\n", free_total, free_total / 1048576.0);
    }

    /* Session tick */
    printf("\nSession tick: %u\n", store.session_tick);
    printf("Twin:        %s\n", store.is_twin ? "YES" : "NO");
    if (store.is_twin) printf("  filepath:  %s\n", store.filepath);

    dt_store_destroy(&store);
    return 0;
}
