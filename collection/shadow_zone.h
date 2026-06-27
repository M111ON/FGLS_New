#ifndef SHADOW_ZONE_H
#define SHADOW_ZONE_H

#include <stdint.h>
#include <string.h>
#include "coord_spine.h"

#define SHADOW_OK      0
#define SHADOW_ERR    -1
#define SHADOW_FULL   -2

typedef struct {
    uint64_t bond_key;
    uint32_t tick;
    uint8_t  alive;
    uint8_t  zone_id;      /* 10 or 11 */
    uint8_t  temperature;
    uint8_t  _pad;
} ShadowSlotMeta;

typedef struct {
    ShadowSlotMeta slots[SHADOW_N_SLOTS];
    uint32_t       cursor;
    uint32_t       live_count;
    uint32_t       total_writes;
    uint32_t       evictions;
    uint8_t        zone_id;     /* SHADOW_ZONE_A or SHADOW_ZONE_B */
    uint8_t        _pad[3];
} ShadowZone;

/* total memory for both zones = 2 * 1728 * (16 + 64) = 276,480 bytes
 * data can live in external buffer — we only track metadata */

static inline void shadow_zone_init(ShadowZone *z, uint8_t zone_id)
{
    memset(z, 0, sizeof(*z));
    z->zone_id = zone_id;
}

static inline uint32_t shadow_zone_node_id(const ShadowZone *z, uint32_t slot)
{
    return SHADOW_NODE_BASE(z->zone_id) + slot;
}

static inline uint32_t shadow_zone_slot_from_node(uint32_t node_id)
{
    return node_id % SHADOW_N_SLOTS;
}

static inline int shadow_zone_is_shadow(uint32_t node_id)
{
    uint32_t base_a = SHADOW_NODE_BASE(SHADOW_ZONE_A);
    uint32_t base_b = SHADOW_NODE_BASE(SHADOW_ZONE_B);
    return (node_id >= base_a && node_id < base_b + SHADOW_N_SLOTS);
}

static inline const ShadowSlotMeta *shadow_find_by_bond(
    const ShadowZone *z, uint64_t bond_key)
{
    if (!z || z->live_count == 0) return NULL;
    uint32_t hint = (uint32_t)(bond_key % SHADOW_N_SLOTS);
    if (z->slots[hint].alive && z->slots[hint].bond_key == bond_key)
        return &z->slots[hint];
    for (uint32_t i = 0; i < SHADOW_N_SLOTS; i++)
        if (z->slots[i].alive && z->slots[i].bond_key == bond_key)
            return &z->slots[i];
    return NULL;
}

static inline int shadow_write(ShadowZone *z, uint64_t bond_key,
                                uint32_t tick, uint8_t temperature,
                                const uint8_t *data, size_t size,
                                uint32_t *out_node_id)
{
    if (!z || !data || !out_node_id) return SHADOW_ERR;
    if (size > DIAMOND_BLOCK_SIZE) return SHADOW_ERR;

    const ShadowSlotMeta *prev = shadow_find_by_bond(z, bond_key);
    if (prev) {
        uint32_t ps = (uint32_t)(prev - z->slots);
        z->slots[ps].alive = 0;
        if (z->live_count > 0) z->live_count--;
    }

    uint32_t slot = z->cursor % SHADOW_N_SLOTS;

    ShadowSlotMeta *m = &z->slots[slot];
    if (m->alive) z->evictions++;

    m->bond_key    = bond_key;
    m->tick        = tick;
    m->alive       = 1;
    m->zone_id     = z->zone_id;
    m->temperature = temperature;

    z->cursor = (z->cursor + 1) % SHADOW_N_SLOTS;
    if (z->live_count < SHADOW_N_SLOTS) z->live_count++;
    z->total_writes++;

    *out_node_id = shadow_zone_node_id(z, slot);
    return SHADOW_OK;
}

static inline int shadow_write_data(ShadowZone *z, uint64_t bond_key,
                                     uint32_t tick, uint8_t temperature,
                                     const uint8_t *data,
                                     uint32_t *out_node_id)
{
    return shadow_write(z, bond_key, tick, temperature,
                        data, DIAMOND_BLOCK_SIZE, out_node_id);
}

static inline int shadow_free(ShadowZone *z, uint64_t bond_key)
{
    const ShadowSlotMeta *m = shadow_find_by_bond(z, bond_key);
    if (!m) return SHADOW_ERR;
    uint32_t slot = (uint32_t)(m - z->slots);
    z->slots[slot].alive = 0;
    if (z->live_count > 0) z->live_count--;
    return SHADOW_OK;
}

static inline uint32_t shadow_total_capacity(void)
{
    return (uint32_t)SHADOW_ZONES * SHADOW_N_SLOTS;
}

typedef struct {
    uint32_t total_writes;
    uint32_t live_count;
    uint32_t evictions;
    uint32_t capacity;
    uint8_t  zone_id;
} ShadowZoneStats;

static inline ShadowZoneStats shadow_stats(const ShadowZone *z)
{
    ShadowZoneStats s;
    s.total_writes = z->total_writes;
    s.live_count   = z->live_count;
    s.evictions    = z->evictions;
    s.capacity     = SHADOW_N_SLOTS;
    s.zone_id      = z->zone_id;
    return s;
}

static inline int shadow_verify(void)
{
    /* T1: constants check */
    if (SHADOW_ZONES != 2)        return -1;
    if (SHADOW_N_SLOTS != 1728u)  return -2;

    /* T2: node base sanity */
    if (SHADOW_NODE_BASE(SHADOW_ZONE_A) != 17280u) return -3;
    if (SHADOW_NODE_BASE(SHADOW_ZONE_B) != 19008u) return -4;

    /* T3: is_shadow check */
    if (!shadow_zone_is_shadow(18000u)) return -5;
    if (shadow_zone_is_shadow(5000u))   return -6;

    /* T4: write/read cycle */
    uint8_t data[DIAMOND_BLOCK_SIZE];
    memset(data, 0xAB, DIAMOND_BLOCK_SIZE);
    ShadowZone z;
    shadow_zone_init(&z, SHADOW_ZONE_A);
    uint32_t node_id;
    if (shadow_write(&z, 0xAABBCCDDu, 1, 1, data, DIAMOND_BLOCK_SIZE, &node_id) != SHADOW_OK)
        return -7;
    if (!shadow_zone_is_shadow(node_id)) return -8;
    const ShadowSlotMeta *m = shadow_find_by_bond(&z, 0xAABBCCDDu);
    if (!m || !m->alive) return -9;
    if (m->bond_key != 0xAABBCCDDu) return -10;
    if (m->temperature != 1) return -11;

    /* T5: free */
    if (shadow_free(&z, 0xAABBCCDDu) != SHADOW_OK) return -12;
    if (shadow_find_by_bond(&z, 0xAABBCCDDu) != NULL) return -13;

    /* T6: capacity */
    if (shadow_total_capacity() != 2 * 1728u) return -14;

    return 0;
}

#endif /* SHADOW_ZONE_H */
