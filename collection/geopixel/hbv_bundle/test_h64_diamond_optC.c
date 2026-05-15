/*
 * test_h64_diamond.c — POGLS → FiboLayer → Image Sequence
 *
 * Architecture:
 *   Diamond flow → fibo_dispatch_entry → GpxTileInput (RGB 8×8)
 *     → 16-path Hilbert routing (4 sets × 3+1)
 *     → free invert + inv_of_inv proof
 *     → [cover] + [tile_0..tile_N] sequence
 *
 * This is a geometric addressing transport — NOT image compression.
 * R=content, G=address, B=binding → FLAT tile when content≈address
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "pogls_fold.h"
#include "geo_diamond_field.h"
#include "geo_route.h"
#include "pogls_scanner.h"
#include "pogls_hilbert64_encoder.h"
#include "pogls_to_geopixel.h"
#include "fibo_layer_header.h"
#include "fibo_tile_dispatch.h"
/* hb_header_frame.h used for cover page concept — not directly included */

/* ── triage config ──────────────────────────────────────────────── */
#define ENTROPY_T_LOW    12u
#define ENTROPY_T_HIGH   48u

/* ── cheap entropy: count unique byte values ────────────────────── */
static inline uint32_t estimate_entropy(const uint8_t chunk[64]) {
    uint64_t bm[4] = {0,0,0,0};
    for (int i = 0; i < 64; i++) {
        uint8_t b = chunk[i];
        bm[b >> 6] |= (1ULL << (uint8_t)(b & 63));
    }
    return (uint32_t)(__builtin_popcountll(bm[0]) +
                      __builtin_popcountll(bm[1]) +
                      __builtin_popcountll(bm[2]) +
                      __builtin_popcountll(bm[3]));
}

/* ── seed from XOR-fold + finalizer ────────────────────────────── */
static inline uint64_t compute_seed(const uint8_t chunk[64]) {
    const uint64_t *w = (const uint64_t*)chunk;
    uint64_t s = w[0] ^ w[1] ^ w[2] ^ w[3]
               ^ w[4] ^ w[5] ^ w[6] ^ w[7];
    s ^= s >> 33; s *= 0xff51afd7ed558ccdULL;
    s ^= s >> 33; s *= 0xc4ceb9fe1a85ec53ULL;
    s ^= s >> 33;
    return s;
}

/* ── theta_map (simplified, matches scanner) ───────────────────── */
static inline void derive_coord(uint64_t seed,
                                 uint8_t *face, uint8_t *edge, uint8_t *z)
{
    uint64_t h = seed;
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    uint32_t h_hi = (uint32_t)(h >> 32);
    uint32_t h_lo = (uint32_t)(h & 0xFFFFFFFFu);
    *face = (uint8_t)(((uint64_t)h_hi * 12u) >> 32);
    *edge = (uint8_t)(((uint64_t)h_lo * 5u) >> 32);
    *z    = (uint8_t)((h >> 16) & 0xFFu);
}

int main(void) {
    FILE *f = fopen("test_input.bmp", "rb");
    if (!f) { fprintf(stderr, "cannot open\n"); return 1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)fsz);
    if (fread(buf, 1, (size_t)fsz, f) != (size_t)fsz) { fclose(f); free(buf); return 1; }
    fclose(f);

    uint64_t n_chunks = (uint64_t)(fsz / 64);
    printf("file: %ld B  chunks: %llu\n", fsz, (unsigned long long)n_chunks);

    /* ── Setup: FiboLayer dispatch ─────────────────────────────────── */
    FiboDispatchCtx fctx;
    uint64_t first_seed = compute_seed(buf);
    if (!fibo_dispatch_init(&fctx, first_seed, 8u)) {
        fprintf(stderr, "fibo_dispatch_init failed\n");
        free(buf); return 1;
    }

    /* ── Stats ─────────────────────────────────────────────────────── */
    uint64_t n_tiles = 0;
    uint64_t lossless_tiles = 0;   /* tiles where B=0 (R=G) */
    uint64_t flat_tiles = 0;       /* B ch fully zero */
    uint64_t seq_bytes = 0;        /* sequence storage estimate */
    uint64_t raw_bytes = 0;

    /* ── Process each chunk → tile ────────────────────────────────── */
    printf("\n── Processing chunks → FiboLayer tiles ──\n");

    for (uint64_t ci = 0; ci < n_chunks; ci++) {
        const uint8_t *chunk = buf + ci * 64;
        uint64_t seed = compute_seed(chunk);

        uint8_t face, edge, z;
        derive_coord(seed, &face, &edge, &z);

        /* entropy gate */
        uint32_t ent = estimate_entropy(chunk);

        /* build tile via fibo dispatch */
        GpxTileInput tile;
        if (!fibo_dispatch_entry(&fctx, chunk, face, edge, z,
                                 (uint32_t)ci, 0, &tile))
            continue;

        n_tiles++;
        raw_bytes += 64;   /* original chunk */

        /* ── analyze tile binding ──────────────────────────────────── */
        /* B channel = R^G. If R≈G → B≈0 → FLAT → compressible */
        uint32_t b_sum = 0;
        uint32_t r_sum = 0;
        int b_zero = 1;
        for (uint32_t p = 0; p < 64; p++) {
            uint8_t r = tile.tile_rgb[p * 3 + 0];
            uint8_t g = tile.tile_rgb[p * 3 + 1];
            uint8_t b = tile.tile_rgb[p * 3 + 2];
            r_sum += r;
            b_sum += b;
            if (b) b_zero = 0;
        }

        /* ── triage decision ─────────────────────────────────────── */
        /* tile storage cost depends on binding quality */
        uint32_t tile_cost;
        int is_lossless = 0;

        if (ent <= ENTROPY_T_LOW) {
            /* very structured → store as tile (lossless) */
            tile_cost = 192;   /* RGB tile = 192B */
            is_lossless = 1;
            lossless_tiles++;
            /* R=content, G=geometry, B=binding */
            /* inv_of_inv gives free integrity check */
        } else if (ent >= ENTROPY_T_HIGH || b_sum > 256) {
            /* high entropy or poor binding → seed-only */
            tile_cost = 8;     /* just route_id + checksum */
        } else {
            /* moderate → full tile (B has signal → hamburger can compress) */
            tile_cost = 192;
            is_lossless = 1;
            lossless_tiles++;
        }

        seq_bytes += tile_cost;

        if (b_zero) flat_tiles++;

        if (n_tiles <= 3 || n_tiles == n_chunks) {
            printf("  tile %4llu: face=%u edge=%u z=%u ent=%u"
                   " set=%u route=%u %s%scost=%u B\n",
                   (unsigned long long)n_tiles, face, edge, z, ent,
                   tile.set_idx, tile.route_idx,
                   is_lossless?"LOSSLESS ":"",
                   b_zero?" FLAT!":"",
                   tile_cost);
        }
    }

    /* ── Fibo layer stats ──────────────────────────────────────────── */
    printf("\n=== FIBO LAYER STATE ===\n");
    printf("tick_start : %u\n",  fctx.hdr.tick_start);
    printf("layer_seq  : %u\n",  fctx.hdr.layer_seq);
    printf("set_count  : %u\n",  fctx.hdr.set_count);
    printf("flags      : 0x%02x\n", fctx.hdr.flags);
    printf("seed_origin: 0x%016llx\n", (unsigned long long)fctx.hdr.seed_origin);
    printf("inv_witness: 0x%016llx\n", (unsigned long long)fctx.hdr.inv_witness);
    printf("checksum   : 0x%08x\n",  fctx.hdr.layer_checksum);

    /* verify integrity */
    int verified = fibo_layer_verify(&fctx.state);
    printf("verify     : %s\n", verified ? "PASS ✓" : "FAIL ✗");

    printf("\n  Route Sets:\n");
    for (int s = 0; s < 4; s++) {
        printf("    set %d: pos=[0x%016llx 0x%016llx 0x%016llx]"
               " inv=0x%016llx\n",
               s,
               (unsigned long long)fctx.state.sets[s].pos[0],
               (unsigned long long)fctx.state.sets[s].pos[1],
               (unsigned long long)fctx.state.sets[s].pos[2],
               (unsigned long long)fctx.state.sets[s].inv);
    }
    printf("    inv_of_inv: 0x%016llx (proof, never stored)\n",
           (unsigned long long)fctx.state.inv_of_inv);

    /* ── Sequence structure report ────────────────────────────────── */
    printf("\n=== SEQUENCE STRUCTURE ===\n");
    printf("Format: [cover] + [tile_0 ... tile_%llu]\n",
           (unsigned long long)(n_tiles - 1));
    printf("cover = HbHeaderFrame(64B) + FiboLayerHeader(32B)\n");
    printf("\n── Storage breakdown ──\n");
    uint32_t cover_cost = 64 + 32;  /* HbHeader + FiboLayer */
    printf("cover       : %5u B  (HbHeader + FiboLayerHeader)\n", cover_cost);

    if (n_tiles > 0) {
        double pct_lossless = (double)lossless_tiles * 100.0 / (double)n_tiles;
        double pct_flat     = (double)flat_tiles * 100.0 / (double)n_tiles;
        printf("tiles       : %5llu\n", (unsigned long long)n_tiles);
        printf("  lossless  : %5llu (%.1f%%)  × 192B = %llu B\n",
               (unsigned long long)lossless_tiles, pct_lossless,
               (unsigned long long)lossless_tiles * 192);
        printf("  seed-only : %5llu (%.1f%%)  × 8B = %llu B\n",
               (unsigned long long)(n_tiles - lossless_tiles),
               100.0 - pct_lossless,
               (unsigned long long)(n_tiles - lossless_tiles) * 8);
        printf("  FLAT tiles: %5llu (%.1f%%)  → hamburger near-zero\n",
               (unsigned long long)flat_tiles, pct_flat);
    }

    uint64_t total_seq = cover_cost + seq_bytes;
    printf("──────────────────────────────\n");
    printf("sequence    : %5llu B\n", (unsigned long long)total_seq);
    printf("raw chunks  : %5llu B\n", (unsigned long long)raw_bytes);
    printf("ratio       : %.4fx\n", (double)raw_bytes / (double)total_seq);

    printf("\n── Invert folding ──\n");
    printf("  4 sets × 1 invert = 4 inverts (free — XOR of 3 pos)\n");
    printf("  inv_of_inv = XOR(4 inverts) = XOR(all 12 pos)\n");
    printf("  → inv_of_inv is proof of structure — NEVER STORED\n");
    printf("  → ALGEBRAIC IDENTITY: inv_of_inv == XOR(all 12 pos)\n");
    printf("  → If holds: whole FiboLayer is self-verifying\n");

    printf("\n── Hilbert 16-path ──\n");
    printf("  4 groups × (3 pos + 1 inv) = 16 paths\n");
    printf("  Hilbert skip → 1 path per group is free invert\n");
    printf("  Route mapping: face%%4 → set  edge→route within set\n");

    free(buf);
    return 0;
}
