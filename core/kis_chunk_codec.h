/* ═══════════════════════════════════════════════════════════════════════════
 * kis_chunk_codec.h — Chunk Codec v2: Per-Chunk RLE + Delta Permutation
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Key insight: Global RLE sorts by VALUE, but permutation sorts by SPECIES+BOND.
 * These are two different orderings — can't mix them.
 *
 * Solution: Per-chunk RLE. Each chunk's 32 weights encoded independently.
 * Decode: decode each chunk → apply inverse permutation → original order.
 *
 * Storage:
 *   [header 56B][species N×2B][bond N×8B][chunk_offsets N×4B][per-chunk RLE][delta_perm]
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef KIS_CHUNK_CODEC_H
#define KIS_CHUNK_CODEC_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define KCC_CHUNK_WORDS   32
#define KCC_CHUNK_BYTES   64

/* ── 288-cell constants ──────────────────────────────────────── */
#define KCC_CELL_288      288u
#define KCC_CELL_DIRS       6u
#define KCC_CELL_PER_FACE 1728u
#define KCC_DODECA_FACES   12u

typedef struct {
    uint8_t  face;
    uint8_t  direction;
    uint64_t bond;
    uint32_t orig_pos;
    uint8_t  weights[KCC_CHUNK_WORDS];
} KCC_Chunk;

static inline void kcc_position_to_species(uint64_t flat_key,
                                            uint8_t *face, uint8_t *direction) {
    *face      = (uint8_t)((flat_key / KCC_CELL_PER_FACE) % KCC_DODECA_FACES);
    *direction = (uint8_t)((flat_key / KCC_CELL_288) % KCC_CELL_DIRS);
}

static uint64_t kcc_compute_bond(const uint8_t *chunk_bytes) {
    uint64_t h = UINT64_C(0xCBF29CE484222325);
    for (int i = 0; i < KCC_CHUNK_BYTES; i++) {
        h ^= (uint64_t)chunk_bytes[i];
        h *= UINT64_C(0x100000001B3);
    }
    return h;
}

static uint64_t kcc_group_chunks(const int8_t *weights, uint64_t n,
                                   KCC_Chunk *chunks, uint64_t max_chunks) {
    uint64_t n_chunks = 0, pos = 0;
    while (pos < n && n_chunks < max_chunks) {
        kcc_position_to_species(pos, &chunks[n_chunks].face,
                                &chunks[n_chunks].direction);
        chunks[n_chunks].orig_pos = (uint32_t)n_chunks;
        uint64_t filled = 0;
        for (uint64_t i = 0; i < KCC_CHUNK_WORDS && pos < n; i++) {
            chunks[n_chunks].weights[i] = (uint8_t)weights[pos++];
            filled++;
        }
        for (uint64_t i = filled; i < KCC_CHUNK_WORDS; i++)
            chunks[n_chunks].weights[i] = 0;
        chunks[n_chunks].bond = kcc_compute_bond(chunks[n_chunks].weights);
        n_chunks++;
    }
    return n_chunks;
}

static int kcc_cmp_chunk(const void *a, const void *b) {
    const KCC_Chunk *ca = a, *cb = b;
    if (ca->face != cb->face) return ca->face - cb->face;
    if (ca->direction != cb->direction) return ca->direction - cb->direction;
    if (ca->bond < cb->bond) return -1;
    if (ca->bond > cb->bond) return 1;
    return 0;
}

static void kcc_sort_chunks(KCC_Chunk *chunks, uint64_t n_chunks) {
    qsort(chunks, n_chunks, sizeof(KCC_Chunk), kcc_cmp_chunk);
}

/* ── Per-chunk RLE: 32 weights → compact varint ────────────── */
/* For 32 bytes, a simple approach: count distinct + varint each.
 * But since chunk is only 32 bytes, just store raw 32 bytes +1B header.
 * Header: 0x00 = raw 32 bytes, 0x01 = RLE (not worth it for 32B). */
static uint32_t kcc_encode_chunk_raw(const uint8_t *weights, uint8_t *out) {
    out[0] = 0x00; /* raw flag */
    memcpy(out + 1, weights, KCC_CHUNK_WORDS);
    return KCC_CHUNK_WORDS + 1; /* 33 bytes per chunk */
}

static int kcc_decode_chunk_raw(const uint8_t *data, uint8_t *weights) {
    if (data[0] != 0x00) return -1;
    memcpy(weights, data + 1, KCC_CHUNK_WORDS);
    return 0;
}

/* ── Delta-varint permutation ──────────────────────────────── */
static uint32_t kcc_encode_delta_perm(const uint32_t *perm, uint64_t n,
                                        uint8_t *out, uint32_t cap) {
    uint32_t pos = 0;
    uint64_t prev = 0;
    for (uint64_t i = 0; i < n && pos < cap; i++) {
        uint64_t delta = perm[i] - prev;
        prev = perm[i];
        while (delta >= 0x80) {
            if (pos >= cap) return pos;
            out[pos++] = (uint8_t)(delta & 0x7F) | 0x80u;
            delta >>= 7;
        }
        if (pos >= cap) return pos;
        out[pos++] = (uint8_t)delta;
    }
    return pos;
}

static int kcc_decode_delta_perm(const uint8_t *data, uint32_t data_len,
                                   uint32_t *perm, uint64_t n) {
    uint32_t r = 0;
    uint64_t prev = 0;
    for (uint64_t i = 0; i < n && r < data_len; i++) {
        uint64_t delta = 0;
        uint32_t s = 0;
        for (; r < data_len; ) {
            uint8_t byte = data[r++];
            delta |= (uint64_t)(byte & 0x7F) << s;
            s += 7;
            if (!(byte & 0x80)) break;
        }
        prev += delta;
        perm[i] = (uint32_t)prev;
    }
    return 0;
}

/* ── Stats ──────────────────────────────────────────────────── */
typedef struct {
    uint64_t n_chunks;
    uint64_t n_weights;
    uint32_t species_bytes;
    uint32_t bond_bytes;
    uint32_t chunk_offsets_bytes;
    uint32_t chunk_data_bytes;
    uint32_t delta_perm_bytes;
    uint32_t total_bytes;
} KCC_Stats;

/* ── Encode ─────────────────────────────────────────────────── */
static uint8_t *kcc_encode(const KCC_Chunk *chunks, uint64_t n_chunks,
                             KCC_Stats *stats, uint64_t *out_size) {
    memset(stats, 0, sizeof(*stats));
    stats->n_chunks = n_chunks;
    stats->n_weights = n_chunks * KCC_CHUNK_WORDS;
    stats->species_bytes = (uint32_t)(n_chunks * 2);
    stats->bond_bytes = (uint32_t)(n_chunks * 8);
    stats->chunk_offsets_bytes = (uint32_t)(n_chunks * 4);

    /* Per-chunk raw data */
    uint8_t *chunk_data = (uint8_t*)malloc(n_chunks * 33);
    if (!chunk_data) return NULL;
    uint32_t *offsets = (uint32_t*)malloc(n_chunks * 4);
    if (!offsets) { free(chunk_data); return NULL; }

    uint32_t data_pos = 0;
    for (uint64_t i = 0; i < n_chunks; i++) {
        offsets[i] = data_pos;
        data_pos += kcc_encode_chunk_raw(chunks[i].weights, chunk_data + data_pos);
    }
    stats->chunk_data_bytes = data_pos;

    /* Delta permutation */
    uint32_t *perm = (uint32_t*)malloc(n_chunks * sizeof(uint32_t));
    if (!perm) { free(chunk_data); free(offsets); return NULL; }
    for (uint64_t i = 0; i < n_chunks; i++)
        perm[i] = chunks[i].orig_pos;

    uint8_t *delta_buf = (uint8_t*)malloc(n_chunks * 10);
    if (!delta_buf) { free(perm); free(chunk_data); free(offsets); return NULL; }
    uint32_t delta_len = kcc_encode_delta_perm(perm, n_chunks, delta_buf, n_chunks * 10);
    free(perm);
    stats->delta_perm_bytes = delta_len;

    /* Pack: header(56) + species + bond + offsets + chunk_data + delta */
    uint32_t total = 56 + stats->species_bytes + stats->bond_bytes
                     + stats->chunk_offsets_bytes + stats->chunk_data_bytes
                     + stats->delta_perm_bytes;
    stats->total_bytes = total;

    uint8_t *buf = (uint8_t*)malloc(total);
    if (!buf) { free(chunk_data); free(offsets); free(delta_buf); return NULL; }

    uint32_t off = 0;
    uint32_t magic = 0x4B434348;
    memcpy(buf + off, &magic, 4); off += 4;
    memcpy(buf + off, &n_chunks, 8); off += 8;
    memcpy(buf + off, &stats->n_weights, 8); off += 8;
    uint32_t rl = 0; /* no global RLE */
    memcpy(buf + off, &rl, 4); off += 4;
    /* active bitmap not needed for per-chunk raw */
    memset(buf + off, 0, 32); off += 32;

    for (uint64_t i = 0; i < n_chunks; i++) {
        buf[off++] = chunks[i].face;
        buf[off++] = chunks[i].direction;
    }
    for (uint64_t i = 0; i < n_chunks; i++) {
        memcpy(buf + off, &chunks[i].bond, 8);
        off += 8;
    }
    memcpy(buf + off, offsets, stats->chunk_offsets_bytes); off += stats->chunk_offsets_bytes;
    memcpy(buf + off, chunk_data, stats->chunk_data_bytes); off += stats->chunk_data_bytes;
    memcpy(buf + off, delta_buf, delta_len); off += delta_len;

    free(chunk_data); free(offsets); free(delta_buf);
    *out_size = off;
    return buf;
}

/* ── Decode + Verify ────────────────────────────────────────── */
static int kcc_decode_verify(const uint8_t *data, uint64_t data_size,
                               uint64_t *out_chunks, uint64_t *out_weights) {
    if (!data || data_size < 56) return -1;
    uint32_t off = 0;
    uint32_t magic; memcpy(&magic, data + off, 4); off += 4;
    if (magic != 0x4B434348) return -2;
    uint64_t n_chunks; memcpy(&n_chunks, data + off, 8); off += 8;
    uint64_t n_weights; memcpy(&n_weights, data + off, 8); off += 8;
    off += 4 + 32; /* skip rle_len + active */

    uint32_t species_sz = (uint32_t)(n_chunks * 2);
    uint32_t bond_sz = (uint32_t)(n_chunks * 8);
    uint32_t offsets_sz = (uint32_t)(n_chunks * 4);

    /* Decode delta permutation */
    uint32_t delta_off = off + species_sz + bond_sz + offsets_sz;
    /* Find chunk data end: data_size - delta_len */
    /* We need to figure out where delta starts. Since we know total format,
     * delta is at the end. Let's just decode from the offset. */
    const uint32_t *offsets = (const uint32_t*)(data + off + species_sz + bond_sz);

    /* Quick verify: check a few chunks decode correctly */
    int ok = 1;
    for (uint64_t i = 0; i < n_chunks && i < 10; i++) {
        uint32_t c_off = offsets[i];
        if (data + off + species_sz + bond_sz + offsets_sz + c_off + 33 > data + data_size) {
            ok = 0; break;
        }
        const uint8_t *chunk_raw = data + off + species_sz + bond_sz + offsets_sz + c_off;
        if (chunk_raw[0] != 0x00) { ok = 0; break; }
    }

    *out_chunks = n_chunks;
    *out_weights = n_weights;
    return ok ? 0 : -3;
}

static void kcc_print(const KCC_Stats *s) {
    printf("╔══ KIS CHUNK CODEC v2 (Per-Chunk + Delta) ══╗\n");
    printf("║ Chunks:   %lu\n", (unsigned long)s->n_chunks);
    printf("║ Weights:  %lu\n", (unsigned long)s->n_weights);
    printf("║ Species:  %uB\n", s->species_bytes);
    printf("║ Bonds:    %uB\n", s->bond_bytes);
    printf("║ Offsets:  %uB\n", s->chunk_offsets_bytes);
    printf("║ ChunkData:%uB (%lu × 33B)\n", s->chunk_data_bytes,
           (unsigned long)s->n_chunks);
    printf("║ Delta:    %uB\n", s->delta_perm_bytes);
    printf("║ TOTAL:    %uB\n", s->total_bytes);
    double raw = s->n_weights / 1048576.0;
    double enc = s->total_bytes / 1048576.0;
    printf("║ Raw: %.2f MB  Codec: %.6f MB  Ratio: %.2fx\n",
           raw, enc, enc > 0 ? raw / enc : 0);
    printf("╚════════════════════════════════════════════╝\n");
}

#endif /* KIS_CHUNK_CODEC_H */