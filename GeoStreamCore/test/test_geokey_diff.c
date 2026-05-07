--- test_geokey.c (原始)


+++ test_geokey.c (修改后)
#include <stdio.h>
#include <stdint.h>

static inline uint64_t geo_key(
    uint8_t sector,
    uint8_t layer,
    uint8_t cell,
    uint8_t level
){
    return ((uint64_t)sector << 56) |
           ((uint64_t)layer  << 48) |
           ((uint64_t)cell   << 40) |
           ((uint64_t)level  << 32);
}

static inline uint32_t geo_to_shard(uint64_t key){
    return (key >> 58) & 0x3F;
}

int main() {
    // ทดสอบ key แรกๆ
    for (int i = 0; i < 10; i++) {
        uint8_t sector = (i * 7) % 256;
        uint8_t layer = (i * 13) % 256;
        uint8_t cell = (i * 23) % 256;
        uint8_t level = (i * 31) % 256;

        uint64_t key = geo_key(sector, layer, cell, level);
        uint32_t shard = geo_to_shard(key);

        printf("i=%d: sector=%d layer=%d cell=%d level=%d -> key=0x%016lx shard=%d\n",
               i, sector, layer, cell, level, key, shard);
    }

    // ทดสอบ key ที่กระจายไปทุก shard
    printf("\nTesting keys that should span all shards:\n");
    for (int s = 0; s < 64; s++) {
        uint64_t key = geo_key(s, 0, 0, 0);
        uint32_t shard = geo_to_shard(key);
        printf("sector=%d -> key=0x%016lx shard=%d\n", s, key, shard);
    }

    return 0;
}