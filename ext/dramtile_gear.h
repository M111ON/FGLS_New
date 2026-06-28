#ifndef DRAMTILE_GEAR_H
#define DRAMTILE_GEAR_H

/*
 * dramtile_gear.h — DRamTile Gear Extension (compile-time binding)
 * ═══════════════════════════════════════════════════════════════════
 *
 * Gear = POGLS twin-geometric indexing layer on top of DRamTile.
 *
 * Includes everything needed:
 *   1. POGLS prerequisites (lc_twin_gate real impl, GiantArray, CubeFileStore)
 *   2. twin_gear_bridge → pogls_twin_bridge
 *   3. DRamTile store
 *
 * Usage:
 *   #include "ext/dramtile_gear.h"
 *
 * Build (see ext/dramtile_gear_build.inc):
 *   gcc -I. -Icollection -Icollection/src -Icollection/core/core \
 *       -Icollection/core/pogls_engine \
 *       -Icollection/core/pogls_engine/twin_core \
 *       ...
 *
 * Design decision: compile-time coupling (no vtable).
 *   - twin_gear_bridge.h is included directly
 *   - All required types (GiantArray, LCTwinGateCtx, CubeFileStore) are
 *     resolved at compile time via prerequisite includes below.
 *   - Zero overhead, but requires the full POGLS tree.
 */

/* ══════════════════════════════════════════════════════════════════
   Layer 0: Engine geo_config — MUST come first!

   The engine's core/geo_config.h (collection/core/pogls_engine/core/geo_config.h)
   defines GEO_FACES (=9), GEO_SPOKES (=6), GEO_SLOTS (=576), GEO_FULL_N (=3456)
   needed by geo_pipeline_wire → geo_cylinder and other engine headers.

   There is a SECOND geo_config.h in collection/core/core/geo_config.h that uses
   the SAME include guard name (GEO_CONFIG_H) but includes coord_spine.h instead.
   Including the engine version FIRST ensures its GEO_FACES/etc survive.
   ══════════════════════════════════════════════════════════════ */

#include "core/geo_config.h"        /* collection/core/pogls_engine/core/geo_config.h
                                       — defines GEO_FACES, GEO_SPOKES, GEO_SLOTS  */

/* ══════════════════════════════════════════════════════════════════
   Layer 1: POGLS prerequisites (must precede pogls_twin_bridge.h)

   twin_gear_bridge → pogls_twin_bridge → fgls_twin_store
   needs GiantArray (geo_giant_array), CubeFileStore (geo_cube_file_store).

   lc_twin_gate.h: real impl from collection/src/ defines LCTwinGateCtx
   and LC_GATE_* enums.  The stub in collection/core/pogls_engine/ has
   the same include guard (LC_TWIN_GATE_H) → if we include the real one
   first, the stub is harmlessly skipped.
   ══════════════════════════════════════════════════════════════ */

#include "lc_twin_gate.h"           /* collection/src/lc_twin_gate.h  — real impl   */
#include "geo_giant_array.h"        /* collection/core/core/          — GiantArray   */
#include "geo_cube_file_store.h"    /* collection/core/core/          — CubeFileStore */

/* ══════════════════════════════════════════════════════════════════
   Layer 2: Gear bridge
   ══════════════════════════════════════════════════════════════ */

#include "twin_gear_bridge.h"       /* collection/src/twin_gear_bridge.h */

/* ══════════════════════════════════════════════════════════════════
   Layer 3: DRamTile core
   ══════════════════════════════════════════════════════════════ */

#include "runner/dramtile_store.h"  /* runner/dramtile_store.h */

/* ══════════════════════════════════════════════════════════════════
   DtGearStore — composite: DRamTile + Gear
   ══════════════════════════════════════════════════════════════ */

/* FNV-1a 64-bit — fast fingerprint for geometric indexing */
static inline uint64_t _dg_fingerprint(const uint8_t *d, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++)
        h = (h ^ d[i]) * 0x100000001b3ULL;
    return h;
}

typedef struct {
    DRamTileStore   store;
    TwinGearBridge  gear;
    int             gear_enabled;
    GeoSeed         seed;       /* saved for reopen */
    int             cold_twin;  /* 1 = cold is file-backed */
    uint32_t        epoch;      /* monotonic, incremented per batch */
} DtGearStore;

/* ── Init ──
 *   path:  file path for twin (NULL = anonymous)
 *   cold:  cold cap (0 = no cold region)
 *   cold_path: cold file path (NULL = anonymous cold)
 *   seed, bundle, enable_gpu: passed to TwinGearBridge
 */
static inline int dtg_init(DtGearStore *g,
                            const char *path, size_t max_bytes,
                            size_t cold_cap, const char *cold_path,
                            GeoSeed seed, const uint64_t *bundle,
                            int enable_gpu)
{
    int r = path
        ? dt_store_init_twin(&g->store, path, max_bytes)
        : dt_store_init(&g->store, max_bytes);
    if (r != 0) return r;

    if (cold_cap > 0) {
        r = cold_path
            ? dt_store_init_cold_twin(&g->store, cold_path, cold_cap)
            : dt_store_init_cold(&g->store, cold_cap);
        if (r != 0) { dt_store_destroy(&g->store); return r; }
        g->cold_twin = (cold_path != NULL);
    }

    twin_gear_init(&g->gear, seed, bundle, enable_gpu);
    g->gear_enabled = 1;
    g->seed = seed;
    g->epoch = 0;
    return 0;
}

/* ── Single tensor put + geometric indexing ── */
static inline uint8_t *dtg_put(DtGearStore *g,
                                const char *name,
                                const uint8_t *data, size_t sz)
{
    uint8_t *ptr = dt_put(&g->store, name, data, sz);
    if (ptr && g->gear_enabled) {
        uint64_t addr = dt_name_to_addr(name);
        uint64_t sig  = _dg_fingerprint(data, sz);
        twin_gear_write(&g->gear, addr, sig, (uint8_t)(addr & 0x7F));
    }
    return ptr;
}

/* ── KV tensor put + twin ── */
static inline uint8_t *dtg_put_kv(DtGearStore *g,
                                    const char *name,
                                    const uint8_t *data, size_t sz)
{
    uint8_t *ptr = dt_put_kv(&g->store, name, data, sz);
    if (ptr && g->gear_enabled) {
        uint64_t addr = dt_name_to_addr(name) | DT_KV_FLAG;
        uint64_t sig  = _dg_fingerprint(data, sz);
        twin_gear_write(&g->gear, addr, sig, (uint8_t)(addr & 0x7F));
    }
    return ptr;
}

/* ── Batch put — all tensors, single twin_gear_batch fence ── */
static inline int dtg_put_batch(DtGearStore *g,
                                 const char **names,
                                 const uint8_t **datas,
                                 const size_t *szs, uint32_t n)
{
    if (n == 0) return 0;
    uint64_t *addrs = (uint64_t*)alloca(n * sizeof(uint64_t));
    uint64_t *sigs  = (uint64_t*)alloca(n * sizeof(uint64_t));

    for (uint32_t i = 0; i < n; i++) {
        if (!dt_put(&g->store, names[i], datas[i], szs[i])) return -1;
        addrs[i] = dt_name_to_addr(names[i]);
        sigs[i]  = _dg_fingerprint(datas[i], szs[i]);
    }
    if (g->gear_enabled)
        twin_gear_batch(&g->gear, addrs, sigs, n, 0);
    g->epoch++;
    return (int)n;
}

/* ── Gear-aware eviction ──
 *   Skips entries GPU hasn't caught up to.
 *   Returns number evicted.  0 = nothing evictable.
 */
static inline int dtg_evict_gear(DtGearStore *g, int max_entries) {
    if (!g->gear_enabled || max_entries <= 0) return 0;

    uint32_t gpu_w = g->gear.lock.gpu_worlds;

    int evicted = 0;
    for (int pass = 0; pass < max_entries; pass++) {
        int worst = -1;
        uint32_t worst_tick = UINT32_MAX;

        for (int i = 0; i < DT_HASH_SLOTS; i++) {
            if (!(g->store.hash[i].dram_addr & DT_BOND_FLAG)) continue;
            if (g->store.hash[i].dram_addr & DT_KV_FLAG) continue;
            if (gpu_w < g->store.hash[i].session_tick)
                continue;
            uint32_t t = g->store.hash[i].session_tick;
            if (worst < 0 || t < worst_tick) {
                worst = i;
                worst_tick = t;
            }
        }
        if (worst < 0) break;
        g->store.hash[worst].dram_addr = 0;
        g->store.n_stored--;
        evicted++;
    }
    if (evicted > 0)
        dt_cold_rebuild_used(&g->store);
    return evicted;
}

/* ── Read — no gear overhead, pure DRamTile ── */
static inline uint8_t *dtg_get(DtGearStore *g, const char *name) {
    return dt_get(&g->store, name);
}

static inline size_t dtg_get_size(DtGearStore *g, const char *name) {
    return dt_get_size(&g->store, name);
}

/* ── Flush + sync ── */
static inline void dtg_flush(DtGearStore *g) {
    if (g->gear_enabled) {
        twin_gear_flush(&g->gear);
        gear_gpu_tick(&g->gear.lock, GEAR_GPU_WORLD);
    }
    dt_store_sync(&g->store, 0);
}

/* ── Destroy ── */
static inline void dtg_destroy(DtGearStore *g) {
    dtg_flush(g);
    if (g->store.is_twin)
        dt_store_destroy_twinv(&g->store);
    else
        dt_store_destroy(&g->store);
}

/* ── Stats ── */
static inline void dtg_stats(DtGearStore *g, FILE *fp) {
    fprintf(fp, "DtGear: epoch=%u tensors=%u bytes=%zu cap=%zu cold=%zu\n",
            g->epoch, g->store.n_stored, dt_store_total_bytes(&g->store),
            g->store.capacity, g->store.cold_capacity);
    twin_gear_stats(&g->gear, fp);
}

#endif /* DRAMTILE_GEAR_H */
