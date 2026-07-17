/*
 * test_tensor_field.c — Tensor Field Pipeline Test
 *
 * Tests Grid Container with realistic tensor field data:
 *   1. ClimateField (seed + key + attractors) — zone classification
 *   2. RDH addressing (Ring-Wedge-Mirror) — tensor position mapping
 *   3. Tensor data patterns — Q8_0-like quantized weights
 *   4. Geo field climate — node → zone → climate classification
 *
 * Modalities tested:
 *   TEXT:  sparse 1D field (sequential attractors)
 *   IMAGE: dense 2D field (spatial attractor map)
 *   GEO:   raw geo_jump address space
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "geo_jump.h"
#include "geo_field_climate.h"
#include "rdh_addr.h"
#include "diamond_shell_codec.h"
#include "fibo_spine.h"
#include "p5h_ribcage.h"

#define GRID_CHUNK_SZ 64u
#define FS_SLOTS      20736u

/* ══════════════════════════════════════════════════════════════
   Header
   ══════════════════════════════════════════════════════════════ */
#define GRID_MAGIC   0x47524944u
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
    uint8_t  rail_syncs;
    uint32_t flat_count;
    uint32_t sparse_count;
    uint32_t dense_count;
    uint32_t shell_stream_sz;
    uint8_t  modality;        /* TEXT/AUDIO/IMAGE/VIDEO/GEO */
    uint32_t n_attractors;    /* ClimateField attractors */
    uint32_t rdh_capacity;    /* RDH address space capacity */
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
    memcpy(out + o, &h->magic,     4); o += 4;
    memcpy(out + o, &h->version,   4); o += 4;
    memcpy(out + o, &h->seed,      4); o += 4;
    memcpy(out + o, &h->key,       8); o += 8;
    memcpy(out + o, h->dna,       16); o += 16;
    memcpy(out + o, &h->encoder,   1); o += 1;
    memcpy(out + o, &h->n_chunks,  4); o += 4;
    memcpy(out + o, &h->chunk_sz,  4); o += 4;
    memcpy(out + o, &h->grid_slots,4); o += 4;
    memcpy(out + o, &h->ribcage_active, 1); o += 1;
    memcpy(out + o, &h->freeze_count,  4); o += 4;
    memcpy(out + o, &h->rail_syncs,    1); o += 1;
    memcpy(out + o, &h->flat_count,    4); o += 4;
    memcpy(out + o, &h->sparse_count,  4); o += 4;
    memcpy(out + o, &h->dense_count,   4); o += 4;
    memcpy(out + o, &h->shell_stream_sz, 4); o += 4;
    memcpy(out + o, &h->modality,      1); o += 1;
    memcpy(out + o, &h->n_attractors,  4); o += 4;
    memcpy(out + o, &h->rdh_capacity,  4); o += 4;
    return o;
}

/* ══════════════════════════════════════════════════════════════
   Timeline
   ══════════════════════════════════════════════════════════════ */
static uint32_t timeline_pos(uint32_t idx, uint32_t seed, uint32_t slots) {
    return ((idx * 37u) + seed) % slots;
}

static uint64_t fnv1a(const uint8_t *d, size_t sz) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < sz; i++) { h ^= d[i]; h *= 1099511628211ULL; }
    return h;
}

/* ══════════════════════════════════════════════════════════════
   Tensor data generators (realistic patterns)
   ══════════════════════════════════════════════════════════════ */

/* Q8_0-like: 32 int8 quants per block, ~40% zeros (sparse weights) */
static void gen_q80_tensor(uint8_t *buf, size_t sz, uint32_t seed) {
    uint32_t state = seed;
    for (size_t i = 0; i < sz; i++) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        uint8_t r = (uint8_t)(state & 0xFF);
        /* Q8_0 pattern: ~40% zeros, ~30% small (-8..8), ~30% full range */
        if (r < 102)      buf[i] = 0;
        else if (r < 179) buf[i] = (uint8_t)((int8_t)(r & 0x0F) - 8);
        else               buf[i] = r;
    }
}

/* Climate field pattern: attractor-based spatial distribution */
static void gen_climate_tensor(uint8_t *buf, size_t sz, ClimateField *cf) {
    memset(buf, 0, sz);
    /* Place attractor data at node positions */
    for (uint8_t i = 0; i < cf->n_attract; i++) {
        uint32_t node = cf->attract[i].node % GEO_FULL;
        uint8_t weight = cf->attract[i].weight;
        /* Map node to buffer position */
        size_t pos = (node * 64u) % sz;
        for (size_t j = 0; j < 64 && pos + j < sz; j++) {
            buf[pos + j] = (uint8_t)((weight * (j + 1)) & 0xFF);
        }
    }
}

/* RDH-addressed tensor: ring/wedge/mirror structured data */
static void gen_rdh_tensor(uint8_t *buf, size_t sz, const RDHConfig *cfg) {
    memset(buf, 0, sz);
    size_t idx = 0;
    for (int64_t r = 0; r < cfg->n_rings && idx < sz; r++) {
        for (int64_t w = 0; w < cfg->n_wedges && idx < sz; w++) {
            for (int64_t m = 0; m < cfg->n_mirror && idx < sz; m++) {
                for (int64_t u = 0; u < cfg->max_u && idx < sz; u++) {
                    /* RDH key encodes position — use as data pattern */
                    int64_t key = rdh_key(cfg, r, w, m, u, 0);
                    buf[idx] = (uint8_t)(key & 0xFF);
                    idx++;
                }
            }
        }
    }
}

/* Geo field: dodecahedron face-based spatial data */
static void gen_geo_tensor(uint8_t *buf, size_t sz) {
    for (size_t i = 0; i < sz; i++) {
        uint32_t node = (uint32_t)(i * 64u / sz * GEO_FULL);
        GeoFieldClimate c = geo_field_climate(node, 0);
        /* Zone → data pattern */
        switch (c.zone) {
            case GEO_INCIRCLE: buf[i] = 0xFF; break;  /* tropical: hot */
            case GEO_MIDDLE:   buf[i] = 0xAA; break;  /* temperate: warm */
            case GEO_BETWEEN:  buf[i] = 0x55; break;  /* boreal: cool */
            default:           buf[i] = 0x00; break;  /* tundra: cold */
        }
    }
}

/* ══════════════════════════════════════════════════════════════
   Test one tensor field
   ══════════════════════════════════════════════════════════════ */
static int test_tensor_field(const uint8_t *data, size_t data_sz,
                              const char *label, uint8_t modality,
                              ClimateField *cf, const RDHConfig *rdh) {
    printf("\n===========================================================\n");
    printf("  TENSOR FIELD — %s (%u bytes)\n", label, (uint32_t)data_sz);
    printf("===========================================================\n");

    uint64_t orig_hash = fnv1a(data, data_sz);
    printf("  Original hash: 0x%016llx\n", (unsigned long long)orig_hash);
    printf("  Modality: %u (%s)\n", modality,
           modality == 0 ? "TEXT" : modality == 2 ? "IMAGE" : modality == 4 ? "GEO" : "OTHER");

    uint32_t grid_slots = FS_SLOTS;
    uint32_t n_chunks = (uint32_t)((data_sz + GRID_CHUNK_SZ - 1) / GRID_CHUNK_SZ);

    /* ── Step 1: ClimateField analysis ── */
    printf("\n  --- Step 1: ClimateField ---\n");
    if (cf) {
        printf("  Seed: %u, Key: %u, Attractors: %u\n",
               cf->seed, cf->key, cf->n_attract);
        printf("  Centroid: %u (zone=%s)\n", cf->centroid,
               geo_field_climate(cf->centroid, 0).zone == GEO_INCIRCLE ? "INCIRCLE" :
               geo_field_climate(cf->centroid, 0).zone == GEO_MIDDLE ? "MIDDLE" :
               geo_field_climate(cf->centroid, 0).zone == GEO_BETWEEN ? "BETWEEN" : "OUTSIDE");
        for (uint8_t i = 0; i < cf->n_attract; i++) {
            GeoFieldClimate ac = geo_field_climate(cf->attract[i].node, 0);
            printf("    [%u] node=%u weight=%u layer=%u pent=%u zone=%d\n",
                   i, cf->attract[i].node, cf->attract[i].weight,
                   cf->attract[i].layer, cf->attract[i].pent_id, ac.zone);
        }
    }

    /* ── Step 2: RDH capacity ── */
    printf("\n  --- Step 2: RDH Address Space ---\n");
    if (rdh) {
        int64_t cap = rdh_capacity(rdh);
        printf("  RDH config: rings=%lld wedges=%lld mirror=%lld max_u=%lld\n",
               (long long)rdh->n_rings, (long long)rdh->n_wedges,
               (long long)rdh->n_mirror, (long long)rdh->max_u);
        printf("  RDH capacity: %lld keys\n", (long long)cap);
    }

    /* ── Step 3: Grid scatter ── */
    printf("\n  --- Step 3: Grid scatter ---\n");
    uint8_t *grid = (uint8_t *)calloc(grid_slots, GRID_CHUNK_SZ);
    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t pos = timeline_pos(i, 42, grid_slots);
        size_t start = i * GRID_CHUNK_SZ;
        size_t n = (start + GRID_CHUNK_SZ <= data_sz) ? GRID_CHUNK_SZ : data_sz - start;
        memcpy(grid + pos * GRID_CHUNK_SZ, data + start, n);
    }

    /* ── Step 4: Diamond Shell classify ── */
    printf("\n  --- Step 4: Diamond Shell ---\n");
    uint32_t flat = 0, sparse = 0, dense = 0;
    for (uint32_t i = 0; i < n_chunks; i++) {
        const uint8_t *chunk = data + i * GRID_CHUNK_SZ;
        int is_zero = 1;
        for (int j = 0; j < GRID_CHUNK_SZ; j++) { if (chunk[j]) { is_zero = 0; break; } }
        if (is_zero) { flat++; continue; }

        uint8_t rotbuf[64];
        uint64_t best_isect = 0;
        uint8_t best_rot = 0;
        int best_pc = -1;
        for (uint8_t rot = 0; rot < SHELL_ROT_STATES; rot++) {
            _shell_rotate64(rotbuf, chunk, rot);
            DiamondBlock db = _shell_chunk_to_block(rotbuf, rot, i);
            if (!fold_xor_audit(&db)) { db.invert = ~db.core.raw; fold_build_quad_mirror(&db); }
            uint64_t isect = fold_fibo_intersect(&db);
            int pc = __builtin_popcountll(isect);
            if (pc > best_pc) { best_pc = pc; best_isect = isect; best_rot = rot; }
        }
        if (best_pc <= SHELL_SPARSE_THRESH) sparse++;
        else dense++;
    }
    printf("  FLAT=%u SPARSE=%u DENSE=%u (total=%u)\n", flat, sparse, dense, n_chunks);

    /* ── Step 5: Shell encode + decode roundtrip ── */
    printf("\n  --- Step 5: Shell roundtrip ---\n");
    uint8_t *shell_out = (uint8_t *)malloc(n_chunks * 66 + 16);
    uint64_t shell_sz = shell_stream_encode(data, n_chunks, shell_out);
    printf("  Shell stream: %u bytes\n", (uint32_t)shell_sz);

    uint8_t *decoded = (uint8_t *)malloc(n_chunks * GRID_CHUNK_SZ);
    shell_stream_decode(shell_out, n_chunks, decoded);
    uint64_t decoded_hash = fnv1a(decoded, data_sz);
    int pass = (decoded_hash == orig_hash) ? 1 : 0;
    printf("  Shell roundtrip: %s\n", pass ? "PASS" : "FAIL");

    /* ── Step 6: Header + total ── */
    printf("\n  --- Step 6: Header + total ---\n");
    GridHeader hdr;
    grid_header_init(&hdr);
    hdr.seed = 42; hdr.key = 0xDEADBEEFCAFE1234ULL;
    hdr.n_chunks = n_chunks; hdr.grid_slots = grid_slots;
    hdr.ribcage_active = 1; hdr.flat_count = flat;
    hdr.sparse_count = sparse; hdr.dense_count = dense;
    hdr.shell_stream_sz = (uint32_t)shell_sz;
    hdr.modality = modality;
    hdr.n_attractors = cf ? cf->n_attract : 0;
    hdr.rdh_capacity = rdh ? (uint32_t)rdh_capacity(rdh) : 0;
    grid_header_dna(&hdr, data, data_sz);

    uint8_t hdr_buf[128];
    size_t hdr_sz = grid_header_pack(&hdr, hdr_buf);
    uint32_t total = (uint32_t)hdr_sz + (uint32_t)shell_sz;

    printf("  Header: %u bytes\n", (uint32_t)hdr_sz);
    printf("  Shell:  %u bytes\n", (uint32_t)shell_sz);
    printf("  Total:  %u bytes (%.4fx of original)\n", total, (double)total / (double)data_sz);

    free(grid); free(shell_out); free(decoded);
    printf("\n  Tensor Field: %s\n", pass ? "PROVEN" : "FAILED");
    return pass ? 0 : -1;
}

/* ══════════════════════════════════════════════════════════════
   Main
   ══════════════════════════════════════════════════════════════ */
int main(void) {
    printf("===========================================================\n");
    printf("  TENSOR FIELD PIPELINE TEST\n");
    printf("  ClimateField + RDH + geo_field_climate + Diamond Shell\n");
    printf("===========================================================\n");

    int fail = 0;
    uint8_t data[10000];

    /* ── T1: Q8_0 tensor (LLM weights, ~40% zeros) ── */
    gen_q80_tensor(data, sizeof(data), 0xDEADBEEF);
    ClimateField cf_text;
    climate_init(&cf_text, 100, 42, CLIMATE_MODALITY_TEXT, 0, 0);
    climate_add(&cf_text, 500, 10);
    climate_add(&cf_text, 1200, 8);
    climate_add(&cf_text, 3000, 6);
    RDHConfig rdh_kv = RDH_KV_PAGE;
    if (test_tensor_field(data, sizeof(data), "Q8_0 LLM Weights (sparse)", CLIMATE_MODALITY_TEXT, &cf_text, &rdh_kv) != 0) fail++;

    /* ── T2: Climate field (attractor-based spatial) ── */
    ClimateField cf_img;
    climate_init(&cf_img, 200, 77, CLIMATE_MODALITY_IMAGE, 1, 100);
    climate_add(&cf_img, 1000, 12);
    climate_add(&cf_img, 5000, 10);
    climate_add(&cf_img, 10000, 8);
    climate_add(&cf_img, 15000, 6);
    climate_add(&cf_img, 20000, 4);
    gen_climate_tensor(data, sizeof(data), &cf_img);
    if (test_tensor_field(data, sizeof(data), "Climate Field (spatial attractors)", CLIMATE_MODALITY_IMAGE, &cf_img, NULL) != 0) fail++;

    /* ── T3: RDH-addressed tensor ── */
    RDHConfig rdh_tier0 = RDH_TIER0;
    gen_rdh_tensor(data, sizeof(data), &rdh_tier0);
    if (test_tensor_field(data, sizeof(data), "RDH Tier0 (128x162 addressing)", CLIMATE_MODALITY_GEO, NULL, &rdh_tier0) != 0) fail++;

    /* ── T4: Geo field (dodecahedron zones) ── */
    gen_geo_tensor(data, sizeof(data));
    if (test_tensor_field(data, sizeof(data), "Geo Field (dodecahedron zones)", CLIMATE_MODALITY_GEO, NULL, NULL) != 0) fail++;

    /* ── T5: Dense random (worst case, no pattern) ── */
    uint32_t state = 0xCAFEBABE;
    for (size_t i = 0; i < sizeof(data); i++) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        data[i] = (uint8_t)(state & 0xFF);
    }
    if (test_tensor_field(data, sizeof(data), "Dense Random (worst case)", CLIMATE_MODALITY_GEO, NULL, NULL) != 0) fail++;

    printf("\n===========================================================\n");
    printf("  TENSOR FIELD: %s (%d/%d PASS)\n",
           fail ? "FAIL" : "PASS", 5 - fail, 5);
    printf("===========================================================\n");

    return fail ? 1 : 0;
}
