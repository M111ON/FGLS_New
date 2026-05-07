--- test_geokey_debug.c (原始)


+++ test_geokey_debug.c (修改后)
#include <stdio.h>
#include <stdint.h>

static inline uint64_t geo_key(uint8_t sector, uint8_t layer, uint8_t cell, uint8_t level){
    return ((uint64_t)sector << 56) |
           ((uint64_t)layer  << 48) |
           ((uint64_t)cell   << 40) |
           ((uint64_t)level  << 32);
}

static inline uint32_t geo_to_shard(uint64_t key){
    return (key >> 58) & 0x3F;
}

int main() {
    // Test case 1: sector=0, layer=0, cell=0, level=0
    uint64_t key1 = geo_key(0, 0, 0, 0);
    printf("Key1 (0,0,0,0): 0x%016lx, shard=%u\n", key1, geo_to_shard(key1));

    // Test case 2: sector=4, layer=0, cell=0, level=0
    uint64_t key2 = geo_key(4, 0, 0, 0);
    printf("Key2 (4,0,0,0): 0x%016lx, shard=%u\n", key2, geo_to_shard(key2));

    // Test case 3: sector=8, layer=0, cell=0, level=0
    uint64_t key3 = geo_key(8, 0, 0, 0);
    printf("Key3 (8,0,0,0): 0x%016lx, shard=%u\n", key3, geo_to_shard(key3));

    // Test case 4: sector=252, layer=0, cell=0, level=0
    uint64_t key4 = geo_key(252, 0, 0, 0);
    printf("Key4 (252,0,0,0): 0x%016lx, shard=%u\n", key4, geo_to_shard(key4));

    // Test with actual values from test
    for (int i = 0; i < 10; i++) {
        uint8_t shard_id = i % 64;
        uint8_t sector = shard_id * 4;
        uint8_t layer = (i / 64) & 0xFF;
        uint8_t cell = (i >> 8) & 0xFF;
        uint8_t level = (i >> 16) & 0xFF;

        uint64_t key = geo_key(sector, layer, cell, level);
        uint32_t shard = geo_to_shard(key);

        printf("i=%d: sector=%d, shard_id=%d, key=0x%016lx, computed_shard=%u\n",
               i, sector, shard_id, key, shard);
    }

    return 0;
}