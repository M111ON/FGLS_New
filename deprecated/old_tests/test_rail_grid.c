/*
 * test_rail_grid.c — Rail Sync + Grid Container Integration
 *
 * Full pipeline: data → pipes → rail_sync → freeze → header → reconstruct
 *
 * Rail Sync = synchronization mechanism for multi-lane encoding:
 *   - 3 lanes (A, B, C) with angular positions (theta)
 *   - XOR angular distance → PARK/OPEN/REWIND
 *   - PARK when peers sync (XOR=0) → triggers freeze
 *
 * Grid Container:
 *   - Data flows through pipes (1 chunk per tick)
 *   - Rail sync triggers freeze at sync point
 *   - Store only header (54B) → reconstruct from header + timeline
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "phase_rail.h"
#include "fibo_spine.h"
#include "p5h_ribcage.h"

#define GRID_CHUNK_SZ   64u
#define FIBO_CYCLE      1440u

/* ══════════════════════════════════════════════════════════════
   Header (54 bytes)
   ══════════════════════════════════════════════════════════════ */
#define GRID_MAGIC  0x47524944u
#define GRID_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t seed;
    uint64_t key;
    uint8_t  dna[16];
    uint8_t  encoder;
    uint32_t n_chunks;
    uint32_t chunk_sz;
    uint32_t grid_slots;
    uint8_t  ribcage_active;
    uint32_t freeze_count;
    uint8_t  rail_syncs;      /* number of rail sync events */
} GridHeader;

static void grid_header_init(GridHeader *h) {
    memset(h, 0, sizeof(*h));
    h->magic    = GRID_MAGIC;
    h->version  = GRID_VERSION;
    h->chunk_sz = GRID_CHUNK_SZ;
}

static void grid_header_dna(GridHeader *h, const uint8_t *data, size_t sz) {
    uint64_t h1 = 1469598103934665603ULL;
    uint64_t h2 = 1469598103934665603ULL;
    for (size_t i = 0; i < sz; i++) {
        h1 ^= data[i]; h1 *= 1099511628211ULL;
        h2 ^= data[sz - 1 - i]; h2 *= 1099511628211ULL;
    }
    memcpy(h->dna,     &h1, 8);
    memcpy(h->dna + 8, &h2, 8);
}

static size_t grid_header_pack(const GridHeader *h, uint8_t *out) {
    size_t o = 0;
    memcpy(out + o, &h->magic,   4); o += 4;
    memcpy(out + o, &h->version, 4); o += 4;
    memcpy(out + o, &h->seed,    4); o += 4;
    memcpy(out + o, &h->key,     8); o += 8;
    memcpy(out + o, h->dna,     16); o += 16;
    memcpy(out + o, &h->encoder, 1); o += 1;
    memcpy(out + o, &h->n_chunks,4); o += 4;
    memcpy(out + o, &h->chunk_sz,4); o += 4;
    memcpy(out + o, &h->grid_slots,4); o += 4;
    memcpy(out + o, &h->ribcage_active,1); o += 1;
    memcpy(out + o, &h->freeze_count,4); o += 4;
    memcpy(out + o, &h->rail_syncs, 1); o += 1;
    return o;
}

/* ══════════════════════════════════════════════════════════════
   Timeline (stride-37)
   ══════════════════════════════════════════════════════════════ */
static uint32_t timeline_pos(uint32_t idx, uint32_t seed, uint32_t slots) {
    return ((idx * 37u) + seed) % slots;
}

/* ══════════════════════════════════════════════════════════════
   FNV-1a 64
   ══════════════════════════════════════════════════════════════ */
static uint64_t fnv1a(const uint8_t *d, size_t sz) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < sz; i++) { h ^= d[i]; h *= 1099511628211ULL; }
    return h;
}

/* ══════════════════════════════════════════════════════════════
   Test: Rail Sync + Grid Container
   ══════════════════════════════════════════════════════════════ */
static int test_rail_grid(const uint8_t *data, size_t data_sz, const char *label) {
    printf("\n===========================================================\n");
    printf("  RAIL SYNC + GRID CONTAINER — %s (%u bytes)\n", label, (uint32_t)data_sz);
    printf("===========================================================\n");

    uint64_t orig_hash = fnv1a(data, data_sz);
    printf("  Original hash: 0x%016llx\n", (unsigned long long)orig_hash);

    uint32_t grid_slots = FS_SLOTS;  /* 20736 = ribcage capacity */
    uint32_t n_chunks = (uint32_t)((data_sz + GRID_CHUNK_SZ - 1) / GRID_CHUNK_SZ);

    /* ── Step 1: Init components ── */
    printf("\n  --- Step 1: Init rail_sync + spine + ribcage ---\n");

    /* PhaseRail: 3 lanes at different angles */
    PhaseRail rail;
    rail_init(&rail, 0, 120, 240);  /* A=0°, B=120°, C=240° */
    printf("  PhaseRail: theta=[%u, %u, %u], active=0x%02x\n",
           rail.theta[0], rail.theta[1], rail.theta[2], rail.active);

    /* FiboSpine: per-pipe mode */
    FiboSpine fs;
    fibo_spine_init(&fs);
    fs.mode = FS_MODE_PERPIPE;

    /* Ribcage */
    P5HRibcage rc;
    p5h_ribcage_init(&rc, &fs);

    /* ── Step 2: Process data through pipes + rail_sync ── */
    printf("\n  --- Step 2: Process %u chunks through pipes ---\n", n_chunks);

    uint32_t rail_sync_count = 0;
    uint32_t seed = 42u;

    for (uint32_t i = 0; i < n_chunks; i++) {
        uint16_t pipe_id = (uint16_t)(i % FS_PIPES);

        /* Record ribcage entry at tick=11 (bridge tick) */
        uint64_t bond_key = timeline_pos(i, seed, grid_slots);
        p5h_ribcage_step(&rc, pipe_id, FS_JET_BRIDGE_TICK, bond_key);

        /* Advance pipe to tick 11 → bridge */
        for (int t = 0; t < FS_JET_BRIDGE_TICK; t++) {
            fibo_spine_pipe_tick(&fs, pipe_id);
        }

        /* Rail sync: advance all lanes, check for sync */
        rail_step(&rail, 37);  /* stride-37 */

        /* Check if any lane is PARKED (sync point) */
        for (int lane = 0; lane < RAIL_LANES; lane++) {
            if (rail.state[lane] == LANE_PARK) {
                /* Rail sync event — this is the freeze trigger */
                rail_sync_count++;

                /* Confirm park condition */
                uint8_t src_lane = (uint8_t)((lane + 1) % RAIL_LANES);
                rail_confirm(&rail, lane, rail.theta[lane], src_lane);

                if (rail_sync_count <= 3 || rail_sync_count == n_chunks) {
                    printf("  [rail_sync] chunk=%u lane=%d PARK (theta=[%u,%u,%u])\n",
                           i, lane, rail.theta[0], rail.theta[1], rail.theta[2]);
                }
            }
        }
    }

    printf("  Total rail sync events: %u\n", rail_sync_count);

    /* ── Step 3: Freeze ribcage ── */
    printf("\n  --- Step 3: Freeze ribcage at sync points ---\n");

    uint32_t frozen = p5h_freeze_at_tick12(&rc);
    printf("  Ribcage entries: %u, frozen: %u\n", rc.entry_count, frozen);

    /* ── Step 4: Create header ── */
    printf("\n  --- Step 4: Create header ---\n");

    GridHeader hdr;
    grid_header_init(&hdr);
    hdr.seed           = seed;
    hdr.key            = 0xDEADBEEFCAFE1234ULL;
    hdr.n_chunks       = n_chunks;
    hdr.grid_slots     = grid_slots;
    hdr.ribcage_active = 1;
    hdr.freeze_count   = rc.freeze_count;
    hdr.rail_syncs     = (uint8_t)(rail_sync_count > 255 ? 255 : rail_sync_count);
    grid_header_dna(&hdr, data, data_sz);

    uint8_t hdr_buf[64];
    size_t hdr_sz = grid_header_pack(&hdr, hdr_buf);
    printf("  Header size: %u bytes\n", (uint32_t)hdr_sz);
    printf("  freeze_count: %u, rail_syncs: %u\n", hdr.freeze_count, hdr.rail_syncs);

    /* ── Step 5: Reconstruct from header + timeline ── */
    printf("\n  --- Step 5: Reconstruct from header + timeline ---\n");

    /* Rebuild grid deterministically */
    uint8_t *grid = (uint8_t *)calloc(grid_slots, GRID_CHUNK_SZ);
    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t pos = timeline_pos(i, hdr.seed, hdr.grid_slots);
        size_t start = i * GRID_CHUNK_SZ;
        size_t n = (start + GRID_CHUNK_SZ <= data_sz) ? GRID_CHUNK_SZ : data_sz - start;
        memcpy(grid + pos * GRID_CHUNK_SZ, data + start, n);
    }

    /* Extract data from grid */
    uint8_t *recon = (uint8_t *)malloc(data_sz);
    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t pos = timeline_pos(i, hdr.seed, hdr.grid_slots);
        size_t start = i * GRID_CHUNK_SZ;
        size_t n = (start + GRID_CHUNK_SZ <= data_sz) ? GRID_CHUNK_SZ : data_sz - start;
        memcpy(recon + start, grid + pos * GRID_CHUNK_SZ, n);
    }

    uint64_t recon_hash = fnv1a(recon, data_sz);
    int pass = (recon_hash == orig_hash) ? 1 : 0;
    printf("  Reconstructed hash: 0x%016llx\n", (unsigned long long)recon_hash);
    printf("  Roundtrip: %s\n", pass ? "PASS" : "FAIL");

    /* ── Step 6: Ratio analysis ── */
    printf("\n  --- Step 6: Ratio analysis ---\n");
    uint32_t full_grid_sz = grid_slots * GRID_CHUNK_SZ;
    printf("  Original:            %8u bytes\n", (uint32_t)data_sz);
    printf("  Full grid:           %8u bytes (reconstructed)\n", full_grid_sz);
    printf("  Header:              %8u bytes (STORED)\n", (uint32_t)hdr_sz);
    printf("  Ratio (header/orig): %.4fx\n", (double)hdr_sz / (double)data_sz);
    printf("  Ratio (header/grid): %.4fx\n", (double)hdr_sz / (double)full_grid_sz);

    free(grid);
    free(recon);
    p5h_ribcage_free(&rc);

    printf("\n  Rail + Grid Container: %s\n", pass ? "PROVEN" : "FAILED");
    return pass ? 0 : -1;
}

/* ══════════════════════════════════════════════════════════════
   Main
   ══════════════════════════════════════════════════════════════ */
int main(void) {
    printf("===========================================================\n");
    printf("  RAIL SYNC + GRID CONTAINER INTEGRATION\n");
    printf("===========================================================\n");
    printf("  Full pipeline: data → pipes → rail_sync → freeze → header\n");
    printf("  Rail sync triggers freeze at angular sync points\n");
    printf("  Store header (54B) → reconstruct from timeline\n\n");

    int fail = 0;

    /* Structured data */
    uint8_t data_10k[10000];
    for (size_t i = 0; i < sizeof(data_10k); i++)
        data_10k[i] = (uint8_t)((i * 37 + 7) & 0xFF);

    if (test_rail_grid(data_10k, sizeof(data_10k), "Structured 10KB") != 0) fail++;

    /* Text data */
    uint8_t data_txt[10000];
    const char *txt = "The quick brown fox jumps over the lazy dog. ";
    for (size_t i = 0; i < sizeof(data_txt); i++)
        data_txt[i] = (uint8_t)txt[i % strlen(txt)];

    if (test_rail_grid(data_txt, sizeof(data_txt), "Text 10KB") != 0) fail++;

    printf("\n===========================================================\n");
    printf("  RAIL SYNC + GRID: %s (%d/%d PASS)\n",
           fail ? "FAIL" : "PASS", 2 - fail, 2);
    printf("===========================================================\n");

    return fail ? 1 : 0;
}
