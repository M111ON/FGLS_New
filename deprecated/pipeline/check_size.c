#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint16_t id;
    uint8_t  card_type;
    uint8_t  entropy;
    uint16_t pattern;
    uint8_t  locality;
    uint8_t  stability;
    uint16_t neighbor_left;
    uint16_t neighbor_right;
    uint64_t hash_val;
} ZoneCardExt;

int main(void) {
    printf("sizeof(ZoneCardExt) = %zu\n", sizeof(ZoneCardExt));
    return 0;
}
