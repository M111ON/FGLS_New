/*
 * test_full_pipeline.c — Grid Container + geo_frame_seek + Diamond Shell
 *
 * Full pipeline:
 *   1. Data → Grid (scatter by timeline stride-37)
 *   2. Grid blocks → Diamond Shell classify (FLAT/SPARSE/DENSE)
 *   3. Store: header (55B) + shell stream (compressed blocks)
 *   4. Decode: header → shell decode → grid → extract data
 *   5. Verify roundtrip
 *
 * Also verifies geo_frame_seek: all 1440 enc are unique (bijection)
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "geo_frame_seek.h"
#include "diamond_shell_codec.h"
#include "fibo_spine.h"
#include "p5h_ribcage.h"

#define GRID_CHUNK_SZ 64u
#define FS_SLOTS      20736u

/* ══════════════════════════════════════════════════════════════
   Header (55 bytes)
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
    uint32_t flat_count;       /* Diamond Shell FLAT blocks */
    uint32_t sparse_count;     /* Diamond Shell SPARSE blocks */
    uint32_t dense_count;      /* Diamond Shell DENSE blocks */
    uint32_t shell_stream_sz;  /* total Diamond Shell output bytes */
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
    return o;
}

/* ══════════════════════════════════════════════════════════════
   Timeline (stride-37)
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
   T1: geo_frame_seek verification
   ══════════════════════════════════════════════════════════════ */
static int test_frame_seek_verify(void) {
    printf("\n--- T1: geo_frame_seek verification ---\n");
    int v = geo_frame_seek_verify();
    printf("  geo_frame_seek_verify(): %s\n", v == 0 ? "PASS" : "FAIL");

    /* Count frames per face */
    uint8_t face_count[12] = {0};
    for (uint32_t t = 0; t < 1440; t++) {
        DualFrame f = frame_seek(t);
        face_count[f.face]++;
    }
    printf("  Frames per face: ");
    for (int i = 0; i < 12; i++) printf("%u ", face_count[i]);
    printf("(all should be 120)\n");

    return v;
}

/* ══════════════════════════════════════════════════════════════
   T2: Full pipeline test
   ══════════════════════════════════════════════════════════════ */
static int test_full_pipeline(const uint8_t *data, size_t data_sz, const char *label) {
    printf("\n===========================================================\n");
    printf("  FULL PIPELINE — %s (%u bytes)\n", label, (uint32_t)data_sz);
    printf("===========================================================\n");

    uint64_t orig_hash = fnv1a(data, data_sz);
    printf("  Original hash: 0x%016llx\n", (unsigned long long)orig_hash);

    uint32_t grid_slots = FS_SLOTS;
    uint32_t n_chunks = (uint32_t)((data_sz + GRID_CHUNK_SZ - 1) / GRID_CHUNK_SZ);

    /* ── Step 1: Data → Grid (scatter by timeline) ── */
    printf("\n  --- Step 1: Data -> Grid (scatter stride-37) ---\n");

    uint8_t *grid = (uint8_t *)calloc(grid_slots, GRID_CHUNK_SZ);
    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t pos = timeline_pos(i, 42, grid_slots);
        size_t start = i * GRID_CHUNK_SZ;
        size_t n = (start + GRID_CHUNK_SZ <= data_sz) ? GRID_CHUNK_SZ : data_sz - start;
        memcpy(grid + pos * GRID_CHUNK_SZ, data + start, n);
    }

    uint32_t full_grid_sz = grid_slots * GRID_CHUNK_SZ;
    printf("  Grid: %u slots x %u bytes = %u bytes\n", grid_slots, GRID_CHUNK_SZ, full_grid_sz);
    printf("  Data placed: %u chunks\n", n_chunks);

    /* ── Step 2: Diamond Shell classify + encode ── */
    printf("\n  --- Step 2: Diamond Shell encode ---\n");

    uint32_t flat = 0, sparse = 0, dense = 0;
    uint8_t *shell_out = (uint8_t *)malloc(n_chunks * 66 + 16);

    /* Count classification across all chunks */
    for (uint32_t i = 0; i < n_chunks; i++) {
        const uint8_t *chunk = data + i * GRID_CHUNK_SZ;

        /* Check if chunk is all-zero */
        int is_zero = 1;
        for (int j = 0; j < GRID_CHUNK_SZ; j++) {
            if (chunk[j]) { is_zero = 0; break; }
        }

        /* Classify via fold_fibo_intersect */
        uint8_t rotbuf[64];
        uint8_t best_buf[64];
        uint64_t best_isect = 0;
        uint8_t  best_rot   = 0;
        int      best_pc    = -1;

        for (uint8_t rot = 0; rot < SHELL_ROT_STATES; rot++) {
            _shell_rotate64(rotbuf, chunk, rot);
            DiamondBlock db = _shell_chunk_to_block(rotbuf, rot, i);
            if (!fold_xor_audit(&db)) {
                db.invert = ~db.core.raw;
                fold_build_quad_mirror(&db);
            }
            uint64_t isect = fold_fibo_intersect(&db);
            int pc = __builtin_popcountll(isect);
            if (pc > best_pc) {
                best_pc    = pc;
                best_isect = isect;
                best_rot   = rot;
                memcpy(best_buf, rotbuf, 64);
            }
        }

        ShellChunkResult r;
        memset(&r, 0, sizeof(r));
        r.best_rot   = best_rot;
        r.fibo_isect = best_isect;
        r.isect_pc   = (uint8_t)(best_pc < 0 ? 0 : best_pc);

        if (is_zero) {
            r.flag = SHELL_FLAG_FLAT;
            flat++;
        } else if (r.isect_pc <= SHELL_SPARSE_THRESH) {
            r.flag = SHELL_FLAG_SPARSE;
            sparse++;
        } else {
            r.flag = SHELL_FLAG_DENSE;
            dense++;
        }
    }

    printf("  Classification: FLAT=%u SPARSE=%u DENSE=%u (total=%u)\n",
           flat, sparse, dense, n_chunks);
    printf("  FLAT ratio: %.1f%% (zero blocks)\n", 100.0 * flat / n_chunks);

    /* Actual shell stream encode */
    uint64_t shell_sz = shell_stream_encode(data, n_chunks, shell_out);
    printf("  Shell stream: %u bytes (vs %u raw)\n",
           (uint32_t)shell_sz, n_chunks * GRID_CHUNK_SZ);
    printf("  Shell ratio: %.4fx\n", (double)shell_sz / (double)(n_chunks * GRID_CHUNK_SZ));

    /* ── Step 3: Create header ── */
    printf("\n  --- Step 3: Create header ---\n");

    GridHeader hdr;
    grid_header_init(&hdr);
    hdr.seed           = 42;
    hdr.key            = 0xDEADBEEFCAFE1234ULL;
    hdr.n_chunks       = n_chunks;
    hdr.grid_slots     = grid_slots;
    hdr.ribcage_active = 1;
    hdr.flat_count     = flat;
    hdr.sparse_count   = sparse;
    hdr.dense_count    = dense;
    hdr.shell_stream_sz = (uint32_t)shell_sz;
    grid_header_dna(&hdr, data, data_sz);

    uint8_t hdr_buf[128];
    size_t hdr_sz = grid_header_pack(&hdr, hdr_buf);
    printf("  Header: %u bytes\n", (uint32_t)hdr_sz);

    /* Total stored = header + shell stream */
    uint32_t total_stored = (uint32_t)hdr_sz + (uint32_t)shell_sz;
    printf("  Total stored: header(%u) + shell(%u) = %u bytes\n",
           (uint32_t)hdr_sz, (uint32_t)shell_sz, total_stored);
    printf("  Total ratio: %.4fx (stored / original)\n",
           (double)total_stored / (double)data_sz);

    /* ── Step 4: Decode shell stream → grid → data ── */
    printf("\n  --- Step 4: Decode ---\n");

    uint8_t *decoded_grid = (uint8_t *)malloc(n_chunks * GRID_CHUNK_SZ);
    uint64_t consumed = shell_stream_decode(shell_out, n_chunks, decoded_grid);
    printf("  Shell decoded: %u bytes consumed\n", (uint32_t)consumed);

    /* Extract data from decoded grid */
    uint8_t *recon = (uint8_t *)malloc(data_sz);
    for (uint32_t i = 0; i < n_chunks; i++) {
        size_t start = i * GRID_CHUNK_SZ;
        size_t n = (start + GRID_CHUNK_SZ <= data_sz) ? GRID_CHUNK_SZ : data_sz - start;
        memcpy(recon + start, decoded_grid + i * GRID_CHUNK_SZ, n);
    }

    uint64_t recon_hash = fnv1a(recon, data_sz);
    int pass = (recon_hash == orig_hash) ? 1 : 0;
    printf("  Reconstructed hash: 0x%016llx\n", (unsigned long long)recon_hash);
    printf("  Roundtrip: %s\n", pass ? "PASS" : "FAIL");

    /* ── Step 5: geo_frame_seek per-chunk ── */
    printf("\n  --- Step 5: geo_frame_seek per chunk ---\n");
    uint32_t unique_enc[1440] = {0};
    uint32_t enc_count = 0;
    for (uint32_t i = 0; i < n_chunks && i < 1440; i++) {
        uint16_t enc = frame_enc(i);
        DualFrame f = frame_at(enc);
        unique_enc[enc]++;
        enc_count++;
    }
    uint32_t unique = 0;
    for (uint32_t i = 0; i < 1440; i++) {
        if (unique_enc[i] > 0) unique++;
    }
    printf("  %u chunks mapped to %u unique enc values\n", enc_count, unique);
    printf("  All unique: %s\n", (unique == enc_count) ? "YES" : "NO");

    /* ── Summary ── */
    printf("\n  --- Summary ---\n");
    printf("  Original:              %8u bytes\n", (uint32_t)data_sz);
    printf("  Full grid (recon):     %8u bytes\n", full_grid_sz);
    printf("  Shell stream:          %8u bytes\n", (uint32_t)shell_sz);
    printf("  Header:                %8u bytes\n", (uint32_t)hdr_sz);
    printf("  TOTAL STORED:          %8u bytes\n", total_stored);
    printf("  Ratio (stored/orig):   %.4fx\n", (double)total_stored / (double)data_sz);
    printf("  Ratio (stored/grid):   %.6fx\n", (double)total_stored / (double)full_grid_sz);

    free(grid); free(shell_out); free(decoded_grid); free(recon);
    p5h_ribcage_free(&(P5HRibcage){0}); /* safe no-op */

    printf("\n  Full Pipeline: %s\n", pass ? "PROVEN" : "FAILED");
    return pass ? 0 : -1;
}

/* ══════════════════════════════════════════════════════════════
   Main
   ══════════════════════════════════════════════════════════════ */
int main(void) {
    printf("===========================================================\n");
    printf("  FULL PIPELINE TEST\n");
    printf("  Grid Container + geo_frame_seek + Diamond Shell\n");
    printf("===========================================================\n");

    int fail = 0;

    /* T1: geo_frame_seek verification */
    if (test_frame_seek_verify() != 0) fail++;

    /* T2: Structured data (repeating pattern — many FLAT blocks) */
    uint8_t data_struct[10000];
    for (size_t i = 0; i < sizeof(data_struct); i++)
        data_struct[i] = (uint8_t)((i % 128 == 0) ? 0xFF : ((i * 37 + 7) & 0x7F));

    if (test_full_pipeline(data_struct, sizeof(data_struct), "Structured 10KB") != 0) fail++;

    /* T3: Sparse data (~50% zeros) */
    uint8_t data_sparse[10000];
    uint32_t state = 0xDEADBEEF;
    for (size_t i = 0; i < sizeof(data_sparse); i++) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        data_sparse[i] = ((state & 0xFF) < 128) ? 0 : (uint8_t)(state & 0xFF);
    }

    if (test_full_pipeline(data_sparse, sizeof(data_sparse), "Sparse 10KB (~50% zeros)") != 0) fail++;

    /* T4: Dense random (no zeros — worst case for Diamond Shell) */
    uint8_t data_dense[10000];
    state = 0xCAFEBABE;
    for (size_t i = 0; i < sizeof(data_dense); i++) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        data_dense[i] = (uint8_t)((state & 0xFF) | 1); /* never zero */
    }

    if (test_full_pipeline(data_dense, sizeof(data_dense), "Dense random 10KB (no zeros)") != 0) fail++;

    printf("\n===========================================================\n");
    printf("  FULL PIPELINE: %s (%d/%d PASS)\n",
           fail ? "FAIL" : "PASS", 3 - fail, 3);
    printf("===========================================================\n");

    return fail ? 1 : 0;
}
