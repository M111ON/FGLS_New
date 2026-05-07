--- pogls_engine_test.c (原始)


+++ pogls_engine_test.c (修改后)
/*
 * pogls_engine_test.c — POGLS Real Engine End-to-End Performance Test
 * ════════════════════════════════════════════════════════════════════
 *
 * ทดสอบประสิทธิภาพจริงของ POGLS Engine ทั้งระบบ:
 * 1. GeoKey Generation (64-bit)
 * 2. Shard Mapping (Bitwise, no modulo)
 * 3. Offset Calculation (O(1), fixed record size)
 * 4. Multi-shard File I/O (64 shards)
 * 5. Full Pipeline: Write → Read → Verify
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

/* ════════════════════════════════════════════════════════════════════
 * GeoKey Design (64-bit) — หัวใจของ GSC
 * layout: [ shard:6 | index:58 ]
 * where shard = sector >> 2, index = remaining bits
 * ════════════════════════════════════════════════════════════════════ */

#define SHARD_BITS 6   /* 2^6 = 64 shards */

static inline uint64_t geo_key(
    uint8_t sector,
    uint8_t layer,
    uint8_t cell,
    uint8_t level
){
    /* Extract shard from sector top 6 bits */
    uint64_t shard = (sector >> 2) & 0x3F;

    /* Build 58-bit local index from: sector[1:0], layer[7:0], cell[7:0], level[7:0] */
    uint64_t idx = ((uint64_t)(sector & 0x03) << 24) |
                   ((uint64_t)layer << 16) |
                   ((uint64_t)cell << 8) |
                   ((uint64_t)level);

    return (shard << 58) | idx;
}

/* ════════════════════════════════════════════════════════════════════
 * Shard Mapping — ใช้ top 6 bits (bits 63-58) ของ key
 * ════════════════════════════════════════════════════════════════════ */

static inline uint32_t geo_to_shard(uint64_t key){
    return (key >> 58) & 0x3F;
}

/* ════════════════════════════════════════════════════════════════════
 * Offset Calculation — Fixed Record Size, O(1)
 * ใช้ lower 58 bits เป็น index ภายใน shard
 * ════════════════════════════════════════════════════════════════════ */

#define RECORD_SIZE 32      /* bytes per record */
#define HEADER_SIZE 4096    /* bytes per shard header */

static inline uint64_t geo_to_offset(uint64_t key){
    /* Mask out top 6 shard bits, use remaining as local index */
    uint64_t local = key & 0x03FFFFFFFFFFFFFFULL;
    return HEADER_SIZE + (local * RECORD_SIZE);
}

/* ════════════════════════════════════════════════════════════════════
 * Record Structure
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t key;
    uint64_t value;
    uint64_t timestamp;
    uint32_t checksum;
    uint32_t reserved;
} GeoRecord;

static inline uint32_t compute_checksum(uint64_t key, uint64_t val, uint64_t ts) {
    return (uint32_t)((key ^ val ^ ts) & 0xFFFFFFFF);
}

/* ════════════════════════════════════════════════════════════════════
 * Shard File Management
 * ════════════════════════════════════════════════════════════════════ */

#define NUM_SHARDS (1 << SHARD_BITS)

typedef struct {
    int fds[NUM_SHARDS];
    char* filenames[NUM_SHARDS];
    uint64_t records_written[NUM_SHARDS];
    uint64_t records_read[NUM_SHARDS];
} ShardManager;

static void init_shard_manager(ShardManager* sm, const char* base_dir) {
    memset(sm, 0, sizeof(*sm));

    for (int i = 0; i < NUM_SHARDS; i++) {
        char filename[256];
        snprintf(filename, sizeof(filename), "%s/shard_%02d.dat", base_dir, i);
        sm->filenames[i] = strdup(filename);

        sm->fds[i] = open(filename, O_RDWR | O_CREAT, 0644);
        if (sm->fds[i] < 0) {
            perror("open");
            exit(1);
        }

        struct stat st;
        if (fstat(sm->fds[i], &st) == 0 && st.st_size < HEADER_SIZE) {
            char header[HEADER_SIZE];
            memset(header, 0, HEADER_SIZE);
            memcpy(header, "POGL", 4);
            ((uint32_t*)header)[1] = SHARD_BITS;
            ((uint32_t*)header)[2] = i;
            write(sm->fds[i], header, HEADER_SIZE);
        }
    }
}

static void close_shard_manager(ShardManager* sm) {
    for (int i = 0; i < NUM_SHARDS; i++) {
        if (sm->fds[i] >= 0) close(sm->fds[i]);
        if (sm->filenames[i]) free(sm->filenames[i]);
    }
}

/* ════════════════════════════════════════════════════════════════════
 * Core Operations
 * ════════════════════════════════════════════════════════════════════ */

static inline int geo_write_record(ShardManager* sm, uint64_t key, uint64_t value) {
    uint32_t shard = geo_to_shard(key);
    uint64_t offset = geo_to_offset(key);

    GeoRecord rec;
    rec.key = key;
    rec.value = value;
    rec.timestamp = (uint64_t)time(NULL);
    rec.checksum = compute_checksum(key, value, rec.timestamp);
    rec.reserved = 0;

    ssize_t written = pwrite(sm->fds[shard], &rec, sizeof(rec), offset);
    if (written == sizeof(rec)) {
        sm->records_written[shard]++;
        return 1;
    }
    return 0;
}

static inline int geo_read_record(ShardManager* sm, uint64_t key, GeoRecord* out) {
    uint32_t shard = geo_to_shard(key);
    uint64_t offset = geo_to_offset(key);

    ssize_t read_bytes = pread(sm->fds[shard], out, sizeof(*out), offset);
    if (read_bytes == sizeof(*out)) {
        uint32_t expected = compute_checksum(out->key, out->value, out->timestamp);
        if (out->checksum == expected && out->key == key) {
            sm->records_read[shard]++;
            return 1;
        }
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Tests
 * ════════════════════════════════════════════════════════════════════ */

static double random_geo_seek_test(ShardManager* sm, size_t num_seeks) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    GeoRecord rec;
    size_t success = 0;

    /* Random seek using pre-generated keys from write phase */
    for (size_t i = 0; i < num_seeks; i++) {
        /* Pick a random index from the written records */
        size_t rand_idx = rand() % num_seeks;

        uint8_t shard_id = rand_idx % 64;
        uint8_t sector = shard_id * 4;
        uint8_t layer = (rand_idx / 64) & 0xFF;
        uint8_t cell = (rand_idx >> 8) & 0xFF;
        uint8_t level = (rand_idx >> 16) & 0xFF;

        uint64_t key = geo_key(sector, layer, cell, level);
        if (geo_read_record(sm, key, &rec)) {
            success++;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    printf("  Random seek success: %zu/%zu\n", success, num_seeks);
    return elapsed > 0 ? success / elapsed : 0;
}

static double sequential_access_test(ShardManager* sm, size_t num_records) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    GeoRecord rec;
    size_t success = 0;

    /* Read back records using same key generation as write */
    for (size_t i = 0; i < num_records && success < num_records; i++) {
        uint8_t shard_id = i % 64;
        uint8_t sector = shard_id * 4;
        uint8_t layer = (i / 64) & 0xFF;
        uint8_t cell = (i >> 8) & 0xFF;
        uint8_t level = (i >> 16) & 0xFF;

        uint64_t key = geo_key(sector, layer, cell, level);
        if (geo_read_record(sm, key, &rec)) {
            success++;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    printf("  Sequential success: %zu/%zu\n", success, num_records);
    return elapsed > 0 ? success / elapsed : 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Main
 * ════════════════════════════════════════════════════════════════════ */

int main(int argc, char** argv) {
    const char* base_dir = "./pogls_data";
    size_t test_size = 100000;

    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║   POGLS Real Engine — End-to-End Performance Test        ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");

    if (argc > 1) base_dir = argv[1];
    if (argc > 2) test_size = atol(argv[2]);

    printf("Configuration:\n");
    printf("  Base Directory: %s\n", base_dir);
    printf("  Test Size: %zu records\n", test_size);
    printf("  Num Shards: %d\n", NUM_SHARDS);
    printf("  Record Size: %d bytes\n", RECORD_SIZE);
    printf("\n");

    mkdir(base_dir, 0755);

    ShardManager sm;
    init_shard_manager(&sm, base_dir);

    printf("Phase 1: Writing test data...\n");
    {
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        size_t total_written = 0;
        for (size_t i = 0; i < test_size; i++) {
            /* Unique key distribution: spread evenly across all 64 shards */
            /* Each shard gets ~1562 records (100000/64) */
            /* Use Morton-like distribution for better spatial locality */

            uint8_t shard_id = i % 64;  /* Distribute evenly across 64 shards */
            uint8_t sector = shard_id * 4;  /* Map shard to sector (0, 4, 8, ..., 252) */
            uint8_t layer = (i / 64) & 0xFF;
            uint8_t cell = (i >> 8) & 0xFF;
            uint8_t level = (i >> 16) & 0xFF;

            uint64_t key = geo_key(sector, layer, cell, level);
            uint64_t value = i * 0x123456789ABCDEF0ULL;

            if (geo_write_record(&sm, key, value)) {
                total_written++;
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
        printf("  Written: %zu/%zu records (%.2f sec)\n", total_written, test_size, elapsed);
        printf("  Write throughput: %.0f writes/sec\n", elapsed > 0 ? total_written / elapsed : 0);
    }
    printf("\n");

    for (int i = 0; i < NUM_SHARDS; i++) fsync(sm.fds[i]);

    printf("Phase 2: Sequential Access Test...\n");
    {
        double rate = sequential_access_test(&sm, test_size);
        printf("  Sequential throughput: %.0f reads/sec\n", rate);
    }
    printf("\n");

    printf("Phase 3: Random Geometric Seek Test...\n");
    {
        double rate = random_geo_seek_test(&sm, test_size);
        printf("  Random seek throughput: %.0f seeks/sec\n", rate);
    }
    printf("\n");

    printf("Phase 4: Data Integrity Verification...\n");
    {
        size_t verified = 0;
        GeoRecord rec;

        for (size_t i = 0; i < 1000; i++) {
            uint8_t shard_id = i % 64;
            uint8_t sector = shard_id * 4;
            uint8_t layer = (i / 64) & 0xFF;
            uint8_t cell = (i >> 8) & 0xFF;
            uint8_t level = (i >> 16) & 0xFF;

            uint64_t key = geo_key(sector, layer, cell, level);
            uint64_t expected_value = i * 0x123456789ABCDEF0ULL;

            if (geo_read_record(&sm, key, &rec)) {
                if (rec.value == expected_value && rec.key == key) {
                    verified++;
                }
            }
        }
        printf("  Verified: %zu/1000 records\n", verified);
        printf("  Data Integrity: %s\n", verified == 1000 ? "✓ 100%" : "✗ FAILED");
    }
    printf("\n");

    printf("Phase 5: Shard Statistics...\n");
    {
        uint64_t total_written = 0, total_read = 0;
        for (int i = 0; i < NUM_SHARDS; i++) {
            total_written += sm.records_written[i];
            total_read += sm.records_read[i];
        }
        printf("  Total written: %lu, Total read: %lu\n", total_written, total_read);

        int non_empty = 0;
        for (int i = 0; i < NUM_SHARDS; i++) {
            if (sm.records_written[i] > 0) non_empty++;
        }
        printf("  Active shards: %d/%d\n", non_empty, NUM_SHARDS);
    }
    printf("\n");

    close_shard_manager(&sm);

    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║              TEST COMPLETE                                ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n");

    return 0;
}