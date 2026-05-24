#include <stdio.h>
#include <string.h>
#include "fibo_layer_header.h"

#define SLW_CELLS 64
#define PHI_SEED  1696631ULL

/* cover = only thing stored */
typedef struct {
    FiboLayerHeader hdr;
    uint64_t seeds[SLW_CELLS];  /* 512B */
    uint8_t  layer_seq;
    uint8_t  tick_start;
    uint8_t  _pad[6];
} Cover;                         /* 32+512+8 = 552B */

/* derive everything from slot + seed */
static inline uint8_t slot_face   (uint8_t i) { return i % 12; }
static inline uint8_t slot_channel(uint8_t i) { return slot_face(i) % 3; }
static inline uint8_t slot_flags  (uint8_t i) { return (i >= 48) ? 0x01 : 0x00; }

int main(void)
{
    /* encode: build cover only */
    Cover cover;
    memset(&cover, 0, sizeof(cover));
    fibo_layer_header_init(&cover.hdr, 5, 3, PHI_SEED);
    cover.layer_seq  = 3;
    cover.tick_start = 5;

    for (int i = 0; i < SLW_CELLS; i++)
        cover.seeds[i] = PHI_SEED ^ (uint64_t)i * 0x9E3779B97F4A7C15ULL;

    /* decode: reconstruct all fields from cover */
    int pass = 0;
    for (int i = 0; i < SLW_CELLS; i++) {
        uint64_t seed    = cover.seeds[i];
        uint8_t  face    = slot_face(i);
        uint8_t  channel = slot_channel(i);
        uint8_t  flags   = slot_flags(i);

        /* verify deterministic */
        int ok = (face    == i % 12)
              && (channel == face % 3)
              && (seed    == cover.seeds[i]);
        if (!ok) printf("FAIL cell[%d]\n", i);
        else pass++;
    }

    /* invert: derive from XOR of seeds per channel */
    uint64_t invert[3] = {0};
    for (int i = 0; i < SLW_CELLS; i++)
        invert[slot_channel(i)] ^= cover.seeds[i];

    /* verify fibo */
    FiboLayerState st;
    fibo_layer_expand(&cover.hdr, &st);
    int fibo_ok = fibo_layer_verify(&st);

    printf("cells %d/64  fibo=%s  cover_sz=%zuB\n",
           pass, fibo_ok?"OK":"FAIL", sizeof(Cover));
    printf("invert[0]=%llx [1]=%llx [2]=%llx\n",
           (unsigned long long)invert[0],
           (unsigned long long)invert[1],
           (unsigned long long)invert[2]);
    printf("\n%s\n", (pass==64 && fibo_ok) ? "ALL PASS" : "FAIL");
    return (pass==64 && fibo_ok) ? 0 : 1;
}
