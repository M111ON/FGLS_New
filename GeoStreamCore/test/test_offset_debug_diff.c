--- test_offset_debug.c (原始)


+++ test_offset_debug.c (修改后)
#include <stdio.h>
#include <stdint.h>

#define RECORD_SIZE 32
#define HEADER_SIZE 4096

static inline uint64_t geo_key(uint8_t sector, uint8_t layer, uint8_t cell, uint8_t level){
    return ((uint64_t)sector << 56) |
           ((uint64_t)layer  << 48) |
           ((uint64_t)cell   << 40) |
           ((uint64_t)level  << 32);
}

static inline uint32_t geo_to_shard(uint64_t key){
    return (key >> 58) & 0x3F;
}

static inline uint64_t geo_to_offset(uint64_t key){
    uint64_t local = key & 0x03FFFFFFFFFFFFFFULL;
    return HEADER_SIZE + (local * RECORD_SIZE);
}

int main() {
    // Test with actual values from test
    for (int i = 0; i < 10; i++) {
        uint8_t shard_id = i % 64;
        uint8_t sector = shard_id * 4;
        uint8_t layer = (i / 64) & 0xFF;
        uint8_t cell = (i >> 8) & 0xFF;
        uint8_t level = (i >> 16) & 0xFF;

        uint64_t key = geo_key(sector, layer, cell, level);
        uint32_t shard = geo_to_shard(key);
        uint64_t offset = geo_to_offset(key);

        printf("i=%d: sector=%d, key=0x%016lx, shard=%u, offset=%lu\n",
               i, sector, key, shard, offset);
    }

    // Check if two different keys map to same offset in same shard
    uint64_t key1 = geo_key(0, 0, 0, 0);
    uint64_t key2 = geo_key(0, 0, 0, 1);

    printf("\nCollision check:\n");
    printf("Key1 (0,0,0,0): shard=%u, offset=%lu\n", geo_to_shard(key1), geo_to_offset(key1));
    printf("Key2 (0,0,0,1): shard=%u, offset=%lu\n", geo_to_shard(key2), geo_to_offset(key2));
    printf("Offset diff: %ld bytes (%d records)\n",
           (long)(geo_to_offset(key2) - geo_to_offset(key1)),
           (int)((geo_to_offset(key2) - geo_to_offset(key1)) / RECORD_SIZE));

    return 0;
}