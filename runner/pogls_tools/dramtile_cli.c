/*
 * dramtile_cli.c — DRamTile Store CLI Tool
 *
 * Usage:
 *   dramtile_cli create <store.dt> --capacity <MB>
 *   dramtile_cli put    <store.dt> --name <tensor> --file <data.bin>
 *   dramtile_cli get    <store.dt> --name <tensor> --output <out.bin>
 *   dramtile_cli list   <store.dt>
 *   dramtile_cli stats  <store.dt>
 *   dramtile_cli delete <store.dt> --name <tensor>
 *
 * Build:
 *   gcc -O2 -std=c11 -o dramtile_cli.exe dramtile_cli.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pogls_core/pogls_platform.h"
#include "dramtile_store.h"

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <command> <store.dt> [options]\n\n"
        "Commands:\n"
        "  create  <store.dt> --capacity <MB>     Create a new store\n"
        "  put     <store.dt> --name <N> --file <F>  Store a tensor\n"
        "  get     <store.dt> --name <N> --output <F> Retrieve a tensor\n"
        "  list    <store.dt>                      List all stored tensors\n"
        "  stats   <store.dt>                      Show store statistics\n"
        "  delete  <store.dt> --name <N>           Delete a tensor\n",
        prog);
}

/* Command: create */
static int cmd_create(int argc, char **argv) {
    if (argc < 4) { usage(argv[0]); return 1; }

    const char *path = argv[2];
    const char *cap_str = NULL;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--capacity") == 0 && i + 1 < argc)
            cap_str = argv[++i];
    }
    if (!cap_str) { fprintf(stderr, "Error: --capacity required\n"); return 1; }

    uint64_t capacity = (uint64_t)atoll(cap_str) * 1024 * 1024;
    printf("Creating anonymous store: %s (%llu MB)\n", path, (unsigned long long)(capacity / 1048576));
    printf("Note: Anonymous stores are not persisted to disk.\n");
    printf("Use twin mode for file-backed stores.\n");

    DRamTileStore store;
    memset(&store, 0, sizeof(store));
    if (dt_store_init(&store, (size_t)capacity) != 0) {
        fprintf(stderr, "Error: dt_store_init failed\n");
        return 1;
    }

    printf("Created: %s\n", path);
    printf("  Capacity: %llu bytes (%.2f MB)\n",
           (unsigned long long)store.capacity, store.capacity / 1048576.0);
    printf("  Hash slots: %d\n", DT_HASH_SLOTS);

    dt_store_destroy(&store);
    return 0;
}

/* Command: put */
static int cmd_put(int argc, char **argv) {
    if (argc < 6) { usage(argv[0]); return 1; }

    const char *store_path = argv[2];
    const char *name = NULL;
    const char *file_path = NULL;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc)
            name = argv[++i];
        else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc)
            file_path = argv[++i];
    }
    if (!name || !file_path) { fprintf(stderr, "Error: --name and --file required\n"); return 1; }

    /* Read input file */
    FILE *f = pogls_fopen(file_path, "rb");
    if (!f) { fprintf(stderr, "Error: cannot open %s\n", file_path); return 1; }
    pogls_fseek(f, 0, SEEK_END);
    size_t sz = (size_t)pogls_ftell(f);
    pogls_fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t*)malloc(sz);
    if (!data) { fclose(f); return 1; }
    fread(data, 1, sz, f);
    fclose(f);

    printf("Storing: %s → %s (%zu bytes)\n", name, store_path, sz);

    /* Open anonymous store (not file-backed) */
    DRamTileStore store;
    memset(&store, 0, sizeof(store));
    if (dt_store_init(&store, 0) != 0) {
        fprintf(stderr, "Error: dt_store_init failed\n");
        free(data); return 1;
    }

    /* Put */
    uint8_t *result = dt_put(&store, name, data, sz);
    if (!result) {
        fprintf(stderr, "Error: dt_put failed\n");
        dt_store_destroy(&store);
        free(data);
        return 1;
    }

    printf("  Stored: name=%s  size=%zu bytes\n", name, sz);

    dt_store_destroy(&store);
    free(data);
    return 0;
}

/* Command: get */
static int cmd_get(int argc, char **argv) {
    if (argc < 6) { usage(argv[0]); return 1; }

    const char *store_path = argv[2];
    const char *name = NULL;
    const char *out_path = NULL;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc)
            name = argv[++i];
        else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc)
            out_path = argv[++i];
    }
    if (!name || !out_path) { fprintf(stderr, "Error: --name and --output required\n"); return 1; }

    printf("Retrieving: %s from %s\n", name, store_path);

    DRamTileStore store;
    memset(&store, 0, sizeof(store));
    if (dt_store_init(&store, 0) != 0) {
        fprintf(stderr, "Error: dt_store_init failed\n");
        return 1;
    }

    uint8_t *data = dt_get(&store, name);
    if (!data) {
        fprintf(stderr, "Error: tensor '%s' not found\n", name);
        dt_store_destroy(&store);
        return 1;
    }

    size_t sz = dt_get_size(&store, name);
    printf("  Found: %zu bytes\n", sz);

    FILE *of = pogls_fopen(out_path, "wb");
    if (!of) { fprintf(stderr, "Error: cannot create %s\n", out_path); dt_store_destroy(&store); return 1; }
    fwrite(data, 1, sz, of);
    fclose(of);

    printf("  Written: %s (%zu bytes)\n", out_path, sz);

    dt_store_destroy(&store);
    return 0;
}

/* Command: list */
static int cmd_list(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 1; }

    const char *store_path = argv[2];
    DRamTileStore store;
    if (dt_store_init(&store, 1024 * 1024) != 0) { fprintf(stderr, "Error: init failed\n"); return 1; }

    printf("═══ Store: %s ═══\n", store_path);
    printf("  Capacity: %llu bytes (%.2f MB)\n",
           (unsigned long long)store.capacity, store.capacity / 1048576.0);
    printf("  Used:     %llu bytes (%.2f%%)\n",
           (unsigned long long)store.used, store.used * 100.0 / store.capacity);

    /* Iterate hash table */
    printf("\n  Entries:\n");
    int n = 0;
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        DRamTileHashEntry *e = &store.hash[i];
        if (e->dram_addr == 0) continue;
        if (e->dram_addr & DT_KV_FLAG) {
            printf("    [%d] addr=0x%08X  size=%zu  name=%s  (KV)\n", n, e->dram_addr, e->size, e->name);
        } else {
            printf("    [%d] addr=0x%08X  size=%zu  name=%s\n", n, e->dram_addr, e->size, e->name);
        }
        n++;
    }
    if (n == 0) printf("    (empty)\n");

    printf("\n  Stats:\n");
    printf("    Total bytes: %zu\n", dt_store_total_bytes(&store));

    dt_store_destroy(&store);
    return 0;
}

/* Command: stats */
static int cmd_stats(int argc, char **argv) {
    return cmd_list(argc, argv);  /* Same as list */
}

/* Command: delete */
static int cmd_delete(int argc, char **argv) {
    if (argc < 5) { usage(argv[0]); return 1; }

    const char *store_path = argv[2];
    const char *name = NULL;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc)
            name = argv[++i];
    }
    if (!name) { fprintf(stderr, "Error: --name required\n"); return 1; }

    printf("Deleting: %s from %s\n", name, store_path);

    DRamTileStore store;
    if (dt_store_init(&store, 1024 * 1024) != 0) { fprintf(stderr, "Error: init failed\n"); return 1; }

    dt_free(&store, name);

    printf("  Deleted: %s\n", name);
    dt_store_destroy(&store);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 1; }

    const char *cmd = argv[1];

    if (strcmp(cmd, "create") == 0) return cmd_create(argc, argv);
    if (strcmp(cmd, "put") == 0)    return cmd_put(argc, argv);
    if (strcmp(cmd, "get") == 0)    return cmd_get(argc, argv);
    if (strcmp(cmd, "list") == 0)   return cmd_list(argc, argv);
    if (strcmp(cmd, "stats") == 0)  return cmd_stats(argc, argv);
    if (strcmp(cmd, "delete") == 0) return cmd_delete(argc, argv);

    fprintf(stderr, "Unknown command: %s\n", cmd);
    usage(argv[0]);
    return 1;
}
