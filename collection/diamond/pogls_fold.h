#pragma once
#include <stdint.h>
#include <string.h>

#define PHI_SCALE          (1u << 20)
#define PHI_UP             1696631u
#define PHI_DOWN           648055u

#define NODE_MAX           162
#define FACES_RAW          32
#define FACES_LOGICAL      256

#define DIAMOND_BLOCK_SIZE   64
#define CORE_SLOT_SIZE        8
#define INVERT_SIZE           8
#define ACTIVE_SIZE          16
#define QUAD_MIRROR_SIZE     32
#define FOLD_SIZE            48

#define WORLD_A_LANES_START   0
#define WORLD_A_LANES_END     3
#define WORLD_B_LANES_START   4
#define WORLD_B_LANES_END     7
#define ENGINE_WORLD_BIT   0x40

#define FOLD_VERSION_CURRENT  1
#define UNIVERSE_HEADER_SIZE  8

#define HONEYCOMB_SLOT_OFFSET 48
#define HONEYCOMB_SLOT_SIZE   16

typedef enum {
    WORLD_A = 0,
    WORLD_B = 1,
} world_t;

typedef struct __attribute__((packed)) {
    uint64_t raw;
} CoreSlot;

static inline uint8_t  core_face_id   (CoreSlot c) { return (uint8_t)((c.raw >> 59) & 0x1F); }
static inline uint8_t  core_engine_id (CoreSlot c) { return (uint8_t)((c.raw >> 52) & 0x7F); }
static inline uint32_t core_vector_pos(CoreSlot c) { return (uint32_t)((c.raw >> 28) & 0xFFFFFF); }
static inline uint8_t  core_fibo_gear (CoreSlot c) { return (uint8_t)((c.raw >> 24) & 0x0F); }
static inline uint8_t  core_quad_flags(CoreSlot c) { return (uint8_t)((c.raw >> 16) & 0xFF); }

static inline world_t core_world(CoreSlot c)
{
    return (core_engine_id(c) & ENGINE_WORLD_BIT) ? WORLD_B : WORLD_A;
}

static inline CoreSlot core_slot_build(uint8_t  face_id,
                                        uint8_t  engine_id,
                                        uint32_t vector_pos,
                                        uint8_t  fibo_gear,
                                        uint8_t  quad_flags)
{
    CoreSlot c;
    c.raw = ((uint64_t)(face_id   & 0x1F) << 59)
          | ((uint64_t)(engine_id & 0x7F) << 52)
          | ((uint64_t)(vector_pos & 0xFFFFFF) << 28)
          | ((uint64_t)(fibo_gear  & 0x0F) << 24)
          | ((uint64_t)(quad_flags & 0xFF) << 16);
    return c;
}

static inline uint64_t core_invert(CoreSlot c)
{
    return ~c.raw;
}

typedef struct __attribute__((aligned(64))) {
    CoreSlot core;
    uint64_t invert;
    uint8_t  quad_mirror[32];
    uint8_t  honeycomb[16];
} DiamondBlock;

typedef char _fold_size_check[sizeof(DiamondBlock) == 64 ? 1 : -1];

static inline void fold_build_quad_mirror(DiamondBlock *b)
{
    const uint8_t *src = (const uint8_t *)&b->core.raw;
    uint8_t       *dst = b->quad_mirror;

    memcpy(dst,      src, 8);
    dst[8]  = src[1]; dst[9]  = src[2]; dst[10] = src[3]; dst[11] = src[4];
    dst[12] = src[5]; dst[13] = src[6]; dst[14] = src[7]; dst[15] = src[0];
    dst[16] = src[2]; dst[17] = src[3]; dst[18] = src[4]; dst[19] = src[5];
    dst[20] = src[6]; dst[21] = src[7]; dst[22] = src[0]; dst[23] = src[1];
    dst[24] = src[3]; dst[25] = src[4]; dst[26] = src[5]; dst[27] = src[6];
    dst[28] = src[7]; dst[29] = src[0]; dst[30] = src[1]; dst[31] = src[2];
}

static inline DiamondBlock fold_block_init(uint8_t  face_id,
                                            uint8_t  engine_id,
                                            uint32_t vector_pos,
                                            uint8_t  fibo_gear,
                                            uint8_t  quad_flags)
{
    DiamondBlock b;
    memset(&b, 0, sizeof(b));
    b.core   = core_slot_build(face_id, engine_id,
                                vector_pos, fibo_gear, quad_flags);
    b.invert = core_invert(b.core);
    fold_build_quad_mirror(&b);
    return b;
}

typedef struct __attribute__((packed)) {
    uint64_t merkle_root;
    uint8_t  algo_id;
    uint8_t  migration;
    uint16_t dna_count;
    uint8_t  reserved[4];
} HoneycombSlot;

typedef char _hcomb_size_check[sizeof(HoneycombSlot) == 16 ? 1 : -1];

static inline HoneycombSlot honeycomb_read(const DiamondBlock *b)
{
    HoneycombSlot s;
    memcpy(&s, b->honeycomb, sizeof(s));
    return s;
}

static inline void honeycomb_write(DiamondBlock *b, const HoneycombSlot *s)
{
    memcpy(b->honeycomb, s, sizeof(*s));
}

static inline int fold_xor_audit(const DiamondBlock *b)
{
    return (b->core.raw ^ b->invert) == 0xFFFFFFFFFFFFFFFFull;
}

static inline uint64_t fold_fibo_intersect(const DiamondBlock *b)
{
    const uint64_t *q = (const uint64_t *)b->quad_mirror;
    return q[0] & q[1] & q[2] & q[3];
}

static inline int fold_fibo_needs_merkle(const DiamondBlock *b)
{
    uint64_t inv = fold_fibo_intersect(b);
    return __builtin_popcountll(inv) == 0;
}

static inline world_t fold_switch_gate(const DiamondBlock *b)
{
    return core_world(b->core);
}

#define TWIN_INVERT_MASK   0x40

static inline uint32_t fold_twin_engine_id(const DiamondBlock *b)
{
    uint8_t eid = core_engine_id(b->core);
    return (uint32_t)(eid ^ TWIN_INVERT_MASK);
}

typedef struct __attribute__((packed)) {
    uint32_t universe_id;
    uint8_t  topo_level;
    uint8_t  fold_version;
    uint8_t  reserved[2];
} UniverseHeader;

typedef char _uhdr_size_check[sizeof(UniverseHeader) == 8 ? 1 : -1];

#define EENTANGLE_OP_WRITE        1
#define EENTANGLE_OP_READ         2
#define EENTANGLE_OP_AUDIT        3
#define EENTANGLE_OP_DETACH       4
#define EENTANGLE_OP_TAILS_SPAWN  5

static inline int fold_tails_spawn_data(const DiamondBlock *block,
                                         HoneycombSlot      *out)
{
    HoneycombSlot s = honeycomb_read(block);
    if (s.merkle_root == 0 && s.dna_count == 0)
        return 0;
    *out = s;
    return 1;
}

typedef enum {
    FOLD_VERIFY_PASS    =  0,
    FOLD_VERIFY_EJECT_1 = -1,
    FOLD_VERIFY_EJECT_2 = -2,
    FOLD_VERIFY_NEED_L3 =  1,
} FoldVerifyResult;

static inline FoldVerifyResult fold_verify(const DiamondBlock *b)
{
    if (!fold_xor_audit(b))
        return FOLD_VERIFY_EJECT_1;

    if (fold_fibo_needs_merkle(b))
        return FOLD_VERIFY_NEED_L3;

    return FOLD_VERIFY_PASS;
}
