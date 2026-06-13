/*
 * zone_card_sid.h — ZoneCard + SID coordinate + inference signal
 * Layout: card(12) + coord(12) + inf(8) = 32B flat
 *
 * Two-phase:
 *   1. zcsid_make()       — static capture (weight-time)
 *   2. zcsid_hook_logits() — runtime update (forward pass)
 *
 * NOTE: ZoneCardExt in zone_card.h needs __attribute__((packed))
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "zone_card.h"
#include "tw_capture_int.h"

#define ZCSID_FLAG_FROZEN 0x01
#define ZCSID_FLAG_DRAIN  0x02
#define ZCSID_FLAG_ACTIVE 0x04

/* 8B — filled by runtime hook, proxy values until then */
typedef struct {
    uint8_t  logit_entropy; /* perplexity proxy: high=confused, low=certain */
    uint8_t  confidence;    /* 255 - logit_entropy                           */
    uint16_t tick;          /* token position in sequence                    */
    uint8_t  flags;         /* FROZEN | DRAIN | ACTIVE                       */
    uint8_t  face;          /* dodeca face 0-11                              */
    uint16_t tring_pos;     /* 0-719                                         */
} ZCSIDInference;

/* 32B — core unit for geometric KV compression */
typedef struct {
    ZoneCard       card;      /* 12B: pattern/entropy/neighbors   */
    uint8_t        zone;      /* 0-9                              */
    uint8_t        slot;      /* 0-5                              */
    uint16_t       tring_pos; /* face*60 + zone*6 + slot          */
    int32_t        resid_x;
    int32_t        resid_y;
    ZCSIDInference inf;       /* 8B: runtime signal               */
} ZoneCardSID;

_Static_assert(sizeof(ZCSIDInference) == 8,  "ZCSIDInference must be 8B");
_Static_assert(sizeof(ZoneCardSID)    == 32, "ZoneCardSID must be 32B");

/* Phase 1: static capture from weight data */
static inline ZoneCardSID zcsid_make(
    const float *arr, size_t n,
    uint16_t id, uint16_t nl, uint16_t nr,
    uint8_t face, const TWCaptureInt *cap)
{
    ZoneCardSID z;
    z.card      = zone_card_make(arr, n, id, nl, nr);
    z.zone      = cap->zone;
    z.slot      = cap->slot % 6u;
    z.tring_pos = (uint16_t)(face * 60u + cap->zone * 6u + cap->slot % 6u);
    z.resid_x   = (int32_t)cap->resid_x;
    z.resid_y   = (int32_t)cap->resid_y;
    z.inf = (ZCSIDInference){
        .logit_entropy = z.card.entropy,
        .confidence    = (uint8_t)(255 - z.card.entropy),
        .tick          = id,
        .flags         = cap->drain ? ZCSID_FLAG_DRAIN : ZCSID_FLAG_ACTIVE,
        .face          = face,
        .tring_pos     = z.tring_pos,
    };
    return z;
}

/* Phase 2: update with real logits during forward pass */
static inline void zcsid_hook_logits(ZoneCardSID *z,
                                     const float *logits, size_t n_vocab,
                                     uint16_t tick)
{
    z->inf.logit_entropy = zone_card_entropy((const uint8_t*)logits,
                                              n_vocab * sizeof(float));
    z->inf.confidence    = (uint8_t)(255 - z->inf.logit_entropy);
    z->inf.tick          = tick;
    z->inf.flags        |= ZCSID_FLAG_ACTIVE;
}

/* Topology check: same face + adjacent zone = same dependency cluster */
static inline int zcsid_linked(const ZoneCardSID *a, const ZoneCardSID *b)
{
    if (a->inf.face != b->inf.face) return 0;
    int dz = (int)a->zone - (int)b->zone;
    return (dz < 0 ? -dz : dz) <= 1;
}
