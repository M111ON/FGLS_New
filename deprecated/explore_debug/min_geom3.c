#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Minimal test without the header - inline only what we need */
#define FT_FRAME_CYCLE 1440u
#define FT_FRAME_STRIDE 37u

static inline uint16_t ft_enc_to_pipe(uint16_t enc) {
    return (uint16_t)((enc * 973u) % FT_FRAME_CYCLE);
}

static inline uint8_t ft_enc_to_tick(uint16_t enc) {
    return (uint8_t)(enc % 12u);
}

static inline uint16_t ft_slot_index(uint16_t pipe, uint8_t tick) {
    return (uint16_t)(pipe * 12u + tick);
}

/* BeamCode */
typedef uint8_t BeamCode;
static inline BeamCode beam_code_from_weight(int32_t weight) {
    return (BeamCode)((uint8_t)((int32_t)(weight) + 128));
}
static inline int32_t beam_weight_from_code(BeamCode c) {
    return (int32_t)((int8_t)((int32_t)(c) - 128));
}

/* BeamCoord */
typedef struct {
    uint32_t capo_id;
    uint32_t param_index;
    uint32_t abs_value;
    uint8_t  sign;
} BeamCoord;

static inline BeamCoord beam_index_to_coord(uint32_t index, int32_t weight) {
    BeamCoord c;
    c.capo_id = index / 1000000u;
    c.param_index = index % 1000000u;
    c.abs_value = (uint32_t)((weight < 0) ? -weight : weight);
    c.sign = (uint8_t)((weight >= 0) ? 1 : 0);
    return c;
}

static inline uint32_t beam_coord_to_fibo_slot(BeamCoord c) {
    uint16_t enc = (uint16_t)(c.param_index % FT_FRAME_CYCLE);
    uint16_t pipe = ft_enc_to_pipe(enc);
    uint8_t tick = ft_enc_to_tick(enc);
    return ft_slot_index(pipe, tick);
}

/* POGLS bond */
#define POGLS_GEO_MAGIC  UINT64_C(0x00120090024005A0)
#define POGLS_FNV_PRIME  UINT64_C(0x00000100000001B3)
#define POGLS_FNV_OFFSET UINT64_C(0xCBF29CE484222325)
#define POGLS_BOND_SALT_L UINT64_C(0xAAAAAAAAAAAAAAAA)
#define POGLS_BOND_SALT_R UINT64_C(0x5555555555555555)

static const uint64_t POGLS_FIBO[16] = {
    1,1,2,3,5,8,13,21,34,55,89,144,233,377,610,987
};

static inline uint64_t pogls_fibo_addr(uint64_t seed) {
    uint64_t h = POGLS_FNV_OFFSET;
    uint8_t b[8];
    memcpy(b, &seed, 8);
    for (int i = 0; i < 8; i++) {
        h ^= (uint64_t)b[i];
        h *= POGLS_FNV_PRIME;
    }
    for (int i = 0; i < 16; i++) {
        h ^= POGLS_FIBO[i] * (seed >> (i & 7));
        h = (h << 13) | (h >> 51);
    }
    h ^= POGLS_GEO_MAGIC;
    h *= POGLS_FNV_PRIME;
    h ^= h >> 33;
    return h;
}

static inline uint64_t pogls_bond_key_from_seed(uint64_t origin_seed) {
    uint64_t geo_key = pogls_fibo_addr(origin_seed);
    uint64_t bond_L  = pogls_fibo_addr(geo_key ^ POGLS_BOND_SALT_L);
    uint64_t bond_R  = pogls_fibo_addr(geo_key ^ POGLS_BOND_SALT_R);
    return bond_L ^ bond_R;
}

/* GeomSig */
typedef struct {
    uint64_t bond_key;
    uint32_t fibo_slot;
    uint16_t flower;
    uint8_t  texture;
    uint8_t  face;
    uint8_t  slot;
    uint8_t  ico_idx;
    uint8_t  phase;
    BeamCode code;
    uint8_t  zone;
    uint8_t  position;
} GeomSig;

static inline GeomSig geom_sig_from_index(uint32_t index, int32_t weight) {
    GeomSig s = {0};
    BeamCode code = beam_code_from_weight(weight);
    s.code = code;
    s.zone = code >> 4;
    s.position = code & 0xF;

    BeamCoord c = beam_index_to_coord(index, weight);
    s.fibo_slot = beam_coord_to_fibo_slot(c);
    s.flower = (uint16_t)((c.param_index % FT_FRAME_CYCLE) / 12u % 144u);
    s.texture = (uint8_t)((c.param_index % FT_FRAME_CYCLE) / 144u % 10u);

    uint16_t enc = (uint16_t)(c.param_index % FT_FRAME_CYCLE);
    s.face = (uint8_t)(enc / 120u);
    s.slot = (uint8_t)(enc % 120u);
    s.ico_idx = (uint8_t)(enc % 162u);
    s.phase = (uint8_t)((enc / 12u) % 12u);

    s.bond_key = pogls_bond_key_from_seed(index);

    return s;
}

int main(void) {
    printf("Testing primitives...\n");
    
    int8_t weights[4] = {1, -1, 2, -2};
    
    for (uint32_t i = 0; i < 4; i++) {
        GeomSig s = geom_sig_from_index(i, weights[i]);
        printf("Index %u, weight %d:\n", i, weights[i]);
        printf("  code=%d zone=%d pos=%d\n", s.code, s.zone, s.position);
        printf("  fibo_slot=%u flower=%u texture=%u\n", s.fibo_slot, s.flower, s.texture);
        printf("  face=%u slot=%u ico=%u phase=%u\n", s.face, s.slot, s.ico_idx, s.phase);
        printf("  bond_key=0x%016lx\n", s.bond_key);
    }
    
    printf("Done\n");
    return 0;
}