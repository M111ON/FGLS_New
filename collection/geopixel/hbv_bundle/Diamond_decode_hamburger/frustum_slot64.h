/*
 * frustum_slot64.h — FrustumSlot64 Storage (F2)
 * depends: frustum_trit.h
 * 54 slots × 64B = 3456B = GEO_FULL_N
 */

#ifndef FRUSTUM_SLOT64_H
#define FRUSTUM_SLOT64_H

#include <stdint.h>
#include <string.h>
#include "frustum_trit.h"

/* ── slot: fits exactly in 1 DiamondBlock (64B) ── */
typedef struct __attribute__((packed)) {
    uint32_t core[4];       /* level 0..3 = merkle_root per depth  */
    uint16_t reserved_mask; /* 9 bits — coset silence bitmap       */
    uint8_t  face;          /* 0..5                                */
    uint8_t  coset;         /* 0..8                                */
    uint8_t  letter;        /* 0..25                               */
    uint8_t  _pad[3];
    uint64_t slope;         /* apex fingerprint                    */
    uint8_t  _rest[32];     /* reserved / future                   */
} FrustumSlot64;            /* = 64B exactly                       */

_Static_assert(sizeof(FrustumSlot64) == 64, "FrustumSlot64 must be 64B");

/* ── store: 54 slots = GEO_FULL_N (3456B) ── */
typedef struct {
    FrustumSlot64 slot[GEAR_MESH]; /* [54] */
} FrustumStore;

/* write addr+value into store */
static inline void fstore_write(FrustumStore *s,
                                 uint64_t addr,
                                 uint32_t value,
                                 uint64_t fibo_seed)
{
    TritAddr t   = trit_decompose(addr, value, fibo_seed);
    uint8_t  idx = (uint8_t)(addr % GEAR_MESH); /* slot index 0..53 */

    FrustumSlot64 *sl = &s->slot[idx];
    sl->core[t.level]   = value;
    sl->reserved_mask  &= ~(1u << t.coset); /* clear silence bit  */
    sl->face            = t.face;
    sl->coset           = t.coset;
    sl->letter          = t.letter;
    sl->slope           = t.slope;
}

/* read: recover value at addr */
static inline uint32_t fstore_read(const FrustumStore *s,
                                    uint64_t addr,
                                    uint32_t value,
                                    uint64_t fibo_seed)
{
    TritAddr      t   = trit_decompose(addr, value, fibo_seed);
    uint8_t       idx = (uint8_t)(addr % GEAR_MESH);
    return s->slot[idx].core[t.level];
}

/* silence a coset */
static inline void fstore_silence(FrustumStore *s, uint8_t coset)
{
    for (uint8_t i = 0; i < GEAR_MESH; i++)
        s->slot[i].reserved_mask |= (uint16_t)(1u << (coset & 0x8));
}

static inline void fstore_init(FrustumStore *s)
{
    memset(s, 0, sizeof(*s));
}

#endif /* FRUSTUM_SLOT64_H */
