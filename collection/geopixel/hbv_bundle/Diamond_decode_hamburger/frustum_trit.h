/*
 * frustum_trit.h — Trit Decomposition Engine
 * standalone, no deps
 * addr + value + fibo_seed → TritAddr (all roles, zero redundancy)
 */

#ifndef FRUSTUM_TRIT_H
#define FRUSTUM_TRIT_H

#include <stdint.h>

#define TRIT_MOD      27u   /* 3³ */
#define COSET_COUNT    9u   /* 3² */
#define FACE_COUNT     6u   /* cube faces */
#define LEVEL_COUNT    4u   /* 2² */
#define LETTER_COUNT  26u   /* A..Z */
#define DIAMOND_BLOCK 64u   /* 2⁶ bytes */
#define GEAR_MESH     54u   /* 2×3³ */
#define GCFS_FILE_SZ 4896u  /* 2⁵×3²×17 */

typedef struct {
    uint8_t  trit;    /* 0..26  — (addr ^ value) % 27          */
    uint8_t  coset;   /* 0..8   — trit / 3                     */
    uint8_t  face;    /* 0..5   — trit % 6                     */
    uint8_t  level;   /* 0..3   — trit % 4                     */
    uint8_t  letter;  /* 0..25  — addr % 26                    */
    uint8_t  _pad[3];
    uint64_t slope;   /* fibo_seed ^ addr  (apex fingerprint)  */
} TritAddr;

/*
 * trit_decompose — core function
 * fibo_seed: derive from PHI_UP=1,696,631 or caller-supplied
 */
static inline TritAddr trit_decompose(uint64_t addr,
                                       uint32_t value,
                                       uint64_t fibo_seed)
{
    TritAddr t;
    t.trit   = (uint8_t)((addr ^ (uint64_t)value) % TRIT_MOD);
    t.coset  = t.trit / 3;
    t.face   = t.trit % 6;
    t.level  = t.trit % 4;
    t.letter = (uint8_t)(addr % LETTER_COUNT);
    t.slope  = fibo_seed ^ addr;
    t._pad[0] = t._pad[1] = t._pad[2] = 0;
    return t;
}

/*
 * trit_recover_addr — slope + fibo_seed → original addr
 * (1 XOR, O(1))
 */
static inline uint64_t trit_recover_addr(uint64_t slope, uint64_t fibo_seed)
{
    return slope ^ fibo_seed;
}

/*
 * trit_verify — confirm round-trip
 * returns 1 if addr+value reproduces same trit
 */
static inline int trit_verify(const TritAddr *t,
                               uint64_t addr,
                               uint32_t value,
                               uint64_t fibo_seed)
{
    TritAddr chk = trit_decompose(addr, value, fibo_seed);
    return (chk.trit  == t->trit)
        && (chk.slope == t->slope)
        && (chk.face  == t->face)
        && (chk.level == t->level);
}

#endif /* FRUSTUM_TRIT_H */
