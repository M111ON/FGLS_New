#ifndef KV_REMAP_RAIL_H
#define KV_REMAP_RAIL_H

/*
 * KV Remap Rail — Background skeleton verification
 * ═══════════════════════════════════════════════════
 *
 * Rail system: 3 lanes (same structure as KV layers)
 * Driven by idle detection (step() called when idle)
 * Not clock-driven, not per-decode
 *
 * Interrupt = freeze (rail state saved, resume later)
 *
 * Rail states:
 *   PARK  — no work, waiting for idle
 *   SCAN  — comparing skeleton vs live KV (soft geo first)
 *   PATCH — writing skeleton back to live KV
 *   REBUILD — flush + new skeleton
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "kv_remap.h"

/* ── Rail states ────────────────────────────────────────── */
#define RAIL_PARK    0
#define RAIL_SCAN    1
#define RAIL_PATCH   2
#define RAIL_REBUILD 3
#define RAIL_FREEZE  4

/* ── Rail config ────────────────────────────────────────── */
#define RAIL_SCAN_STRIDE     4096   /* bytes per scan step (idle-friendly) */
#define RAIL_SOFT_THRESH     15     /* below = skeleton fine, no action */
#define RAIL_HARD_THRESH     85     /* above = rebuild */
#define RAIL_PATCH_THRESH    40     /* between soft/hard = patch */

/* ── Per-lane state ─────────────────────────────────────── */
typedef struct {
    uint32_t  pos;          /* current scan position */
    uint64_t  diff_count;   /* bytes changed in this lane */
    uint64_t  checked;      /* bytes checked so far */
    uint64_t  total;        /* total bytes in this lane */
    int       complete;     /* scan finished */
} RailLane;

/* ── Rail context ───────────────────────────────────────── */
typedef struct {
    KVRemapCtx *remap;      /* backlink to remap context */

    /* Rail state */
    int       state;        /* RAIL_PARK / _SCAN / _PATCH / _REBUILD / _FREEZE */
    int       lane;         /* current lane (0-2) */
    RailLane  lanes[3];     /* per-lane scan progress */

    /* Scan results */
    uint64_t  total_diff;   /* total bytes changed across all lanes */
    uint64_t  total_checked; /* total bytes checked */
    int       change_pct;   /* detected change % (from last full scan) */

    /* Patch progress */
    size_t    patch_off;    /* current patch offset */
    uint8_t  *patch_src;    /* source data to patch from */

    /* Freeze state */
    int       freeze_state; /* state to resume after freeze */
    int       freeze_lane;  /* lane to resume after freeze */

    /* Timing */
    double    scan_start_ms;
    double    scan_elapsed_ms;

    int       enabled;
} KVRemapRail;


/* =============================================================
 * Clock helper (ms)
 * ============================================================= */

static inline double rail_clock_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}


/* =============================================================
 * Init
 * ============================================================= */

static inline void kv_remap_rail_init(KVRemapRail *rail, KVRemapCtx *remap) {
    memset(rail, 0, sizeof(*rail));
    rail->remap = remap;
    rail->state = RAIL_PARK;
    rail->enabled = 1;
    fprintf(stderr, "[rail] init: stride=%d bytes/step, thresholds=%d/%d/%d%%\n",
        RAIL_SCAN_STRIDE, RAIL_SOFT_THRESH, RAIL_PATCH_THRESH, RAIL_HARD_THRESH);
}


/* =============================================================
 * Start scan (triggered when idle detected)
 * ============================================================= */

static inline void kv_remap_rail_start_scan(KVRemapRail *rail) {
    if (!rail->enabled || rail->state != RAIL_PARK) return;
    if (!rail->remap->skeleton_valid || !rail->remap->ref_skeleton) return;

    rail->state = RAIL_SCAN;
    rail->lane = 0;
    rail->total_diff = 0;
    rail->total_checked = 0;
    rail->change_pct = -1;
    rail->scan_start_ms = rail_clock_ms();

    /* Init lanes */
    for (int i = 0; i < 3; i++) {
        rail->lanes[i].pos = 0;
        rail->lanes[i].diff_count = 0;
        rail->lanes[i].checked = 0;
        rail->lanes[i].complete = 0;
        rail->lanes[i].total = 0;
    }

    /* Calculate lane totals (split total KV across 3 lanes) */
    size_t per_lane = rail->remap->total_kv_bytes / 3;
    for (int i = 0; i < 3; i++) {
        rail->lanes[i].total = per_lane;
    }
    rail->lanes[2].total += rail->remap->total_kv_bytes % 3;

    fprintf(stderr, "[rail] scan started: lane0=%zu lane1=%zu lane2=%zu bytes\n",
        rail->lanes[0].total, rail->lanes[1].total, rail->lanes[2].total);
}


/* =============================================================
 * Step: one idle-friendly scan chunk
 * Returns: 0 = still scanning, 1 = scan complete, -1 = error
 * ============================================================= */

static inline int kv_remap_rail_step(KVRemapRail *rail) {
    if (!rail->enabled) return -1;

    /* ── PARK ── */
    if (rail->state == RAIL_PARK) {
        return 0;
    }

    /* ── FREEZE ── */
    if (rail->state == RAIL_FREEZE) {
        /* Save state, return to park */
        fprintf(stderr, "[rail] frozen at lane=%d pos=%u, returning to park\n",
            rail->lane, rail->lanes[rail->lane].pos);
        rail->state = RAIL_PARK;
        return 0;
    }

    /* ── SCAN ── */
    if (rail->state == RAIL_SCAN) {
        KVRemapCtx *ctx = rail->remap;
        RailLane *cl = &rail->lanes[rail->lane];

        if (cl->complete) {
            /* Move to next lane */
            rail->lane++;
            if (rail->lane >= 3) {
                /* All lanes done — classify and decide */
                rail->total_checked = 0;
                rail->total_diff = 0;
                for (int i = 0; i < 3; i++) {
                    rail->total_checked += rail->lanes[i].checked;
                    rail->total_diff += rail->lanes[i].diff_count;
                }

                if (rail->total_checked > 0) {
                    rail->change_pct = (int)(rail->total_diff * 100 / rail->total_checked);
                } else {
                    rail->change_pct = 0;
                }

                rail->scan_elapsed_ms = rail_clock_ms() - rail->scan_start_ms;

                fprintf(stderr, "[rail] scan complete: %llu/%llu changed = %d%% (%.1f ms)\n",
                    (unsigned long long)rail->total_diff,
                    (unsigned long long)rail->total_checked,
                    rail->change_pct, rail->scan_elapsed_ms);

                /* Decide action */
                if (rail->change_pct >= RAIL_HARD_THRESH) {
                    rail->state = RAIL_REBUILD;
                    fprintf(stderr, "[rail] decision: REBUILD (%d%% >= %d%%)\n",
                        rail->change_pct, RAIL_HARD_THRESH);
                } else if (rail->change_pct > RAIL_SOFT_THRESH) {
                    rail->state = RAIL_PATCH;
                    rail->patch_off = 0;
                    fprintf(stderr, "[rail] decision: PATCH (%d%% > %d%%)\n",
                        rail->change_pct, RAIL_SOFT_THRESH);
                } else {
                    rail->state = RAIL_PARK;
                    fprintf(stderr, "[rail] decision: PARK (skeleton valid, %d%%)\n",
                        rail->change_pct);
                }
                return 1;
            }
            return 0;
        }

        /* Scan one stride of current lane */
        size_t start = cl->pos;
        size_t end = start + RAIL_SCAN_STRIDE;
        if (end > cl->total) end = cl->total;

        /* Calculate byte offsets into ref_skeleton and live KV */
        size_t lane_byte_offset = 0;
        for (int i = 0; i < rail->lane; i++) {
            lane_byte_offset += ctx->total_kv_bytes / 3;
        }

        for (size_t i = start; i < end; i++) {
            size_t byte_idx = lane_byte_offset + i;
            if (byte_idx >= ctx->total_kv_bytes) break;

            /* Find which layer this byte belongs to */
            size_t accum = 0;
            int layer = 0;
            for (int l = 0; l < ctx->n_layers; l++) {
                size_t layer_total = ctx->layers[l].k_size + ctx->layers[l].v_size;
                if (byte_idx < accum + layer_total) {
                    layer = l;
                    break;
                }
                accum += layer_total;
                if (l == ctx->n_layers - 1) layer = l;
            }

            /* Find offset within layer */
            size_t in_layer = byte_idx - accum;
            const uint8_t *sk = ctx->ref_skeleton + byte_idx;
            const uint8_t *live;

            if (in_layer < ctx->layers[layer].k_size) {
                live = (const uint8_t *)ctx->layers[layer].k_data + in_layer;
            } else {
                live = (const uint8_t *)ctx->layers[layer].v_data +
                    (in_layer - ctx->layers[layer].k_size);
            }

            if (*sk != *live) cl->diff_count++;
            cl->checked++;
        }

        cl->pos = (uint32_t)end;
        if (cl->pos >= cl->total) cl->complete = 1;

        return 0;
    }

    /* ── PATCH ── */
    if (rail->state == RAIL_PATCH) {
        KVRemapCtx *ctx = rail->remap;
        size_t chunk = 4096;

        /* Decompress skeleton incrementally and write back */
        size_t remaining = ctx->total_kv_bytes - rail->patch_off;
        size_t to_write = remaining > chunk ? chunk : remaining;

        if (to_write == 0) {
            rail->state = RAIL_PARK;
            fprintf(stderr, "[rail] patch complete\n");
            return 1;
        }

        /* For now, do full decompress + write (could be incremental) */
        size_t dec_size = 0;
        void *dec = kv_remap_decompress(ctx->skeleton_data, ctx->skeleton_comp, &dec_size);
        if (dec) {
            /* Write chunk from offset */
            size_t woff = 0;
            size_t written = 0;
            for (int l = 0; l < ctx->n_layers; l++) {
                size_t layer_total = ctx->layers[l].k_size + ctx->layers[l].v_size;
                if (rail->patch_off >= woff + layer_total) {
                    woff += layer_total;
                    continue;
                }
                size_t in_layer = rail->patch_off - woff;
                size_t can_write = layer_total - in_layer;
                if (can_write > to_write) can_write = to_write;

                if (in_layer < ctx->layers[l].k_size) {
                    size_t k_write = can_write;
                    if (k_write > ctx->layers[l].k_size - in_layer)
                        k_write = ctx->layers[l].k_size - in_layer;
                    memcpy((uint8_t *)ctx->layers[l].k_data + in_layer,
                           (uint8_t *)dec + rail->patch_off, k_write);
                    written += k_write;
                } else {
                    size_t v_off = in_layer - ctx->layers[l].k_size;
                    size_t v_write = can_write;
                    if (v_write > ctx->layers[l].v_size - v_off)
                        v_write = ctx->layers[l].v_size - v_off;
                    memcpy((uint8_t *)ctx->layers[l].v_data + v_off,
                           (uint8_t *)dec + rail->patch_off, v_write);
                    written += v_write;
                }
                woff += layer_total;
                if (written >= to_write) break;
            }
            free(dec);
            rail->patch_off += written;
        } else {
            rail->state = RAIL_PARK;
            fprintf(stderr, "[rail] patch failed (decompress error)\n");
            return -1;
        }

        return 0;
    }

    /* ── REBUILD ── */
    if (rail->state == RAIL_REBUILD) {
        kv_remap_rebuild(rail->remap);
        rail->state = RAIL_PARK;
        fprintf(stderr, "[rail] rebuild complete\n");
        return 1;
    }

    return -1;
}


/* =============================================================
 * Freeze / Resume
 * ============================================================= */

static inline void kv_remap_rail_freeze(KVRemapRail *rail) {
    if (rail->state == RAIL_PARK) return;
    rail->freeze_state = rail->state;
    rail->freeze_lane = rail->lane;
    rail->state = RAIL_FREEZE;
    fprintf(stderr, "[rail] FROZEN at state=%d lane=%d\n",
        rail->freeze_state, rail->freeze_lane);
}

static inline void kv_remap_rail_resume(KVRemapRail *rail) {
    if (rail->state != RAIL_FREEZE) return;
    rail->state = rail->freeze_state;
    rail->lane = rail->freeze_lane;
    fprintf(stderr, "[rail] RESUMED state=%d lane=%d\n",
        rail->state, rail->lane);
}


/* =============================================================
 * Print status
 * ============================================================= */

static inline void kv_remap_rail_print_status(const KVRemapRail *rail) {
    static const char *state_names[] = {"PARK", "SCAN", "PATCH", "REBUILD", "FREEZE"};
    const char *s = (rail->state >= 0 && rail->state <= RAIL_FREEZE) ?
        state_names[rail->state] : "?";
    fprintf(stderr, "[rail] status: state=%s lane=%d\n", s, rail->lane);
    if (rail->state == RAIL_SCAN) {
        for (int i = 0; i < 3; i++) {
            fprintf(stderr, "  lane%d: pos=%u/%zu diff=%llu checked=%llu %s\n",
                i, rail->lanes[i].pos, rail->lanes[i].total,
                (unsigned long long)rail->lanes[i].diff_count,
                (unsigned long long)rail->lanes[i].checked,
                rail->lanes[i].complete ? "[done]" : "");
        }
    }
    fprintf(stderr, "  last scan: %d%% (%.1f ms)\n",
        rail->change_pct, rail->scan_elapsed_ms);
}


/* =============================================================
 * Destroy
 * ============================================================= */

static inline void kv_remap_rail_destroy(KVRemapRail *rail) {
    fprintf(stderr, "[rail] destroyed (final state=%d, last_pct=%d%%)\n",
        rail->state, rail->change_pct);
    memset(rail, 0, sizeof(*rail));
}


#endif /* KV_REMAP_RAIL_H */
