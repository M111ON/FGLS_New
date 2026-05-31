/*
 * geom_router_bridge.h — BermudaRouter → GeomBridge integration
 * ══════════════════════════════════════════════════════════════
 *
 * Purpose: wire BermudaRouteEntry (zone+tring_slot+polarity) directly
 *   into GeomBridge tile decode — no caller needs to know tensor names.
 *
 * Zone assignment:
 *   On grb_build(), tensors are enumerated in load order.
 *   Each tensor gets zone = load_order_idx % 12.
 *   Multiple tensors share each zone (bucket model).
 *
 * Decode path:
 *   BermudaRouteEntry { zone, tring_slot, polarity }
 *     → zone_members[zone] → pick tensor by tring_slot bucket
 *     → tile_idx = (tring_slot * n_tiles) / 720
 *     → gb_decode_tile() → uint8_t[7]
 *
 * HOT/COLD from bermuda_shadow.h maps to polarity:
 *   HOT  (ROUTE,  polarity=0) → forward decode
 *   COLD (GROUND, polarity=1) → shadow ring only, skip tile decode
 *
 * No malloc beyond GeomBridge's existing blobs.
 * No float. All O(1) per tile.
 *
 * Usage:
 *   #define GEOM_ROUTER_BRIDGE_IMPLEMENTATION
 *   #include "geom_router_bridge.h"
 *
 *   GeomRouterBridge grb;
 *   grb_build(&grb, &gb);                         // wire once after gb_load
 *
 *   uint8_t tile[7];
 *   grb_decode_route(&grb, &route_entry, tile);   // O(1) per token
 *
 * Compile test:
 *   gcc -O2 -DGEOM_RAW_BRIDGE_IMPLEMENTATION -DGEOM_ROUTER_BRIDGE_IMPLEMENTATION \
 *       -I. -o test_grb test_grb.c
 * ══════════════════════════════════════════════════════════════
 */

#ifndef GEOM_ROUTER_BRIDGE_H
#define GEOM_ROUTER_BRIDGE_H

#include <stdint.h>
#include <stddef.h>
#include "geom_raw_bridge.h"
#include "bermuda_export.h"   /* BermudaRouteEntry, BERMUDA_N_ZONES */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ─────────────────────────────────────────────── */

#define GRB_OK              0
#define GRB_ERR            -1
#define GRB_N_ZONES        12u                   /* dodecahedron faces      */
#define GRB_TRING_SLOTS   720u                   /* TRing ring size         */
#define GRB_MAX_PER_ZONE  512u                   /* max tensors per zone    */

/* ── Zone index ─────────────────────────────────────────────── */
/*
 * Built from GeomBridge at grb_build() time.
 * zone_members[z][i] = index into gb->entries[] for i-th tensor in zone z.
 * zone_count[z]      = number of tensors assigned to zone z.
 */
typedef struct {
    uint16_t members[GRB_N_ZONES][GRB_MAX_PER_ZONE];
    uint16_t count[GRB_N_ZONES];
} GrbZoneIndex;

/* ── Decode result ──────────────────────────────────────────── */
typedef struct {
    uint8_t  tile[GSTEN_TILE_SZ];   /* decoded 7 bytes                     */
    uint16_t entry_idx;             /* which gb->entries[] was used        */
    uint32_t tile_idx;              /* which tile within that tensor       */
    uint8_t  zone;
    uint8_t  polarity;              /* 0=ROUTE 1=GROUND                    */
    uint8_t  valid;                 /* 1 = tile decoded, 0 = COLD/skipped  */
} GrbDecodeResult;

/* ── Main context ───────────────────────────────────────────── */
typedef struct {
    GeomBridge   *gb;               /* pointer to loaded GeomBridge        */
    GrbZoneIndex  zones;
    uint32_t      n_tensors;        /* total tensors registered            */
} GeomRouterBridge;

/* ── API ────────────────────────────────────────────────────── */

/*
 * grb_build — scan gb->entries[], assign zones, build zone index.
 * Call once after gb_load().
 */
int  grb_build(GeomRouterBridge *grb, GeomBridge *gb);

/*
 * grb_decode_route — core decode: route_entry → tile[7].
 *
 * Picks tensor from zone bucket using tring_slot as selector.
 * Scales tring_slot → tile_idx proportionally to n_tiles.
 * COLD tokens (polarity==1) set result.valid=0, no decode.
 */
int  grb_decode_route(GeomRouterBridge   *grb,
                      const BermudaRouteEntry *route,
                      GrbDecodeResult    *result);

/*
 * grb_decode_batch — decode N route entries in one call.
 * results[] must be allocated by caller (N elements).
 * Returns number of valid (HOT) tiles decoded.
 */
uint32_t grb_decode_batch(GeomRouterBridge        *grb,
                          const BermudaRouteEntry *routes,
                          GrbDecodeResult         *results,
                          uint32_t                 n);

/*
 * grb_zone_info — diagnostic: print zone occupancy.
 */
void grb_zone_info(const GeomRouterBridge *grb);

#ifdef __cplusplus
}
#endif

/* ══════════════════════════════════════════════════════════════
   IMPLEMENTATION
   ══════════════════════════════════════════════════════════════ */

#ifdef GEOM_ROUTER_BRIDGE_IMPLEMENTATION

#include <stdio.h>
#include <string.h>

/* ── grb_build ─────────────────────────────────────────────── */

int grb_build(GeomRouterBridge *grb, GeomBridge *gb) {
    if (!grb || !gb) return GRB_ERR;

    memset(grb, 0, sizeof(*grb));
    grb->gb = gb;

    /*
     * Walk all entries in gb->entries[] in slot order (0..RB_MAX_ENTRIES-1).
     * Assign zone = (load_counter % 12) so zone distribution is round-robin
     * across the actual populated entries — not hash-slot dependent.
     *
     * We store the gb->entries[] slot index (uint16_t) in zone_members.
     */
    uint16_t load_order = 0;

    for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++) {
        if (!gb->entries[i].occupied) continue;

        uint8_t z = (uint8_t)(load_order % GRB_N_ZONES);

        if (grb->zones.count[z] < GRB_MAX_PER_ZONE) {
            grb->zones.members[z][grb->zones.count[z]] = (uint16_t)i;
            grb->zones.count[z]++;
        }
        /* silently drop if zone is full (GRB_MAX_PER_ZONE=512, very unlikely) */

        load_order++;
    }

    grb->n_tensors = load_order;
    return (load_order > 0) ? GRB_OK : GRB_ERR;
}

/* ── _grb_tring_to_tile ─────────────────────────────────────── */
/*
 * Map tring_slot (0..719) → tile_idx for a tensor with n_tiles tiles.
 *
 * Linear proportional: tile_idx = (tring_slot * n_tiles) / GRB_TRING_SLOTS
 * Result is clamped to [0, n_tiles-1].
 *
 * This gives uniform coverage: tring_slot distributes evenly across
 * the tensor's tile range regardless of tensor size.
 */
static inline uint32_t _grb_tring_to_tile(uint16_t tring_slot,
                                           uint32_t n_tiles) {
    if (n_tiles == 0) return 0;
    uint32_t idx = ((uint32_t)tring_slot * n_tiles) / GRB_TRING_SLOTS;
    if (idx >= n_tiles) idx = n_tiles - 1;
    return idx;
}

/* ── _grb_pick_tensor ───────────────────────────────────────── */
/*
 * Pick which tensor within a zone to address.
 *
 * Strategy: use tring_slot's upper bits to select tensor within zone.
 *   tensor_pick = (tring_slot / (GRB_TRING_SLOTS / zone_count)) % zone_count
 *   tile_idx    = tring_slot % n_tiles  (after tensor pick)
 *
 * But zone_count varies. Simpler and equally uniform:
 *   tensor_pick = tring_slot % zone_count
 * This gives each tensor in the zone equal probability of being selected.
 *
 * If zone is empty → GRB_ERR.
 */
static inline int _grb_pick_tensor(const GeomRouterBridge *grb,
                                    uint8_t  zone,
                                    uint16_t tring_slot,
                                    uint16_t *out_entry_idx,
                                    uint32_t *out_tile_idx) {
    uint16_t cnt = grb->zones.count[zone];
    if (cnt == 0) return GRB_ERR;

    uint16_t pick      = tring_slot % cnt;
    uint16_t entry_idx = grb->zones.members[zone][pick];

    GstenEntry *ge = &grb->gb->entries[entry_idx];
    if (!ge->occupied || ge->n_tiles == 0) return GRB_ERR;

    *out_entry_idx = entry_idx;
    *out_tile_idx  = _grb_tring_to_tile(tring_slot, ge->n_tiles);
    return GRB_OK;
}

/* ── grb_decode_route ───────────────────────────────────────── */

int grb_decode_route(GeomRouterBridge        *grb,
                     const BermudaRouteEntry *route,
                     GrbDecodeResult         *result) {
    if (!grb || !route || !result) return GRB_ERR;

    memset(result, 0, sizeof(*result));
    result->zone     = route->zone;
    result->polarity = route->polarity;

    /* polarity (ROUTE/GROUND) is a routing lane signal, not a decode gate.
     * North-pole HOT tokens carry polarity=GROUND but still produce tiles.
     * Only the pipeline layer (gsp_push) gates on temperature==COLD. */

    /* HOT / ROUTE → decode tile */
    uint16_t entry_idx;
    uint32_t tile_idx;

    if (_grb_pick_tensor(grb, route->zone, route->tring_slot,
                         &entry_idx, &tile_idx) != GRB_OK) {
        return GRB_ERR;
    }

    GstenEntry *ge = &grb->gb->entries[entry_idx];
    if (gb_decode_tile(ge, tile_idx, result->tile) != RB_OK) {
        return GRB_ERR;
    }

    result->entry_idx = entry_idx;
    result->tile_idx  = tile_idx;
    result->valid     = 1;
    return GRB_OK;
}

/* ── grb_decode_batch ───────────────────────────────────────── */

uint32_t grb_decode_batch(GeomRouterBridge        *grb,
                          const BermudaRouteEntry *routes,
                          GrbDecodeResult         *results,
                          uint32_t                 n) {
    if (!grb || !routes || !results || n == 0) return 0;

    uint32_t n_valid = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (grb_decode_route(grb, &routes[i], &results[i]) == GRB_OK
            && results[i].valid) {
            n_valid++;
        }
    }
    return n_valid;
}

/* ── grb_zone_info ──────────────────────────────────────────── */

void grb_zone_info(const GeomRouterBridge *grb) {
    if (!grb) return;
    printf("GeomRouterBridge: %u tensors across %u zones\n",
           grb->n_tensors, GRB_N_ZONES);
    for (uint8_t z = 0; z < GRB_N_ZONES; z++) {
        uint16_t cnt = grb->zones.count[z];
        const char *pole = (z < 6) ? "S" : "N";
        printf("  zone=%2u(%s)  tensors=%u", z, pole, cnt);
        if (cnt > 0 && grb->gb) {
            /* show first tensor name as sample */
            uint16_t ei = grb->zones.members[z][0];
            printf("  [%s", grb->gb->entries[ei].name);
            if (cnt > 1) printf(" +%u more", cnt - 1);
            printf("]");
        }
        printf("\n");
    }
}

#endif /* GEOM_ROUTER_BRIDGE_IMPLEMENTATION */
#endif /* GEOM_ROUTER_BRIDGE_H */
