/*
 * geo_diamond_field.h — Diamond Field v3 (Shell + Slot + Tring)
 *                       + Classifier v2 + Sparse + Batch mode
 *
 * Core concept:
 *   block  = geometry slot (bit in shell)
 *   data   = Tring timeline (external, tick-indexed, variable-size)
 *   delete = clear flag (data survives until GC)
 *   reshape= remap slots to new shell level
 *   batch  = group K chunks → 5B/chunk (L1=8, L2=64, L3=512)
 *
 * Shell scale: level n → size = 2n+1 → slots = (2n+1)^3
 *   n=0: 1    n=1: 27   n=2: 125  n=3: 343
 *   n=4: 729  n=5: 1331 n=6: 2197 n=7: 3375  n=8: 4913
 *
 * Classifier: score = popcnt(isect) + uniq_count - var_norm
 * Sparse: n <= 2 → [count][pos,val]... format
 * Batch:  group K chunks → shared header + base + adaptive bitmask diffs (≤12 diffs)
 *
 * depends: tring.h, pogls_fold.h
 */
#ifndef GEO_DIAMOND_FIELD_H
#define GEO_DIAMOND_FIELD_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "tring.h"
#include "pogls_fold.h"

/* ── constants ────────────────────────────────────────────────────── */
#define SHELL_MAX_LEVEL   8u
#define SHELL_MAX_SLOTS   4913u          /* 17^3, level 8 */
#define SHELL_IDX_BITS    13u
#define SHELL_IDX_MASK    ((1u << SHELL_IDX_BITS) - 1u)  /* 0x1FFF */
#define TRING_MAX_TICKS   (1u << 20)     /* 1M ticks, caller can extend */
#define SLOT_NULL         0xFFFFFFFFu

/* sparse encoding threshold: n <= SPARSE_MAX_LEVEL → sparse store */
#define SPARSE_MAX_LEVEL  2u

/* batch constants */
#define BATCH_L1          8u
#define BATCH_L2          64u
#define BATCH_L3          512u
#define BATCH_MAX_DIFF    12u            /* max differing bytes from base */

/* ── Shell ─────────────────────────────────────────────────────────── */
typedef struct {
    uint8_t  level;
    uint8_t  size;           /* = 2n+1                            */
    uint16_t slot_count;     /* = size^3 (max 4913)               */
    uint8_t  _pad[4];
    uint64_t flags[77];      /* ceil(4913/64) = 77 words = 616B   */
} Shell;

static inline uint16_t shell_size(uint8_t n)   { return (uint16_t)(2u*n + 1u); }
static inline uint16_t shell_slots(uint8_t n)  { uint16_t s = shell_size(n); return (uint16_t)(s*s*s); }

static inline void shell_init(Shell *sh, uint8_t n) {
    memset(sh, 0, sizeof(*sh));
    sh->level      = n;
    sh->size       = (uint8_t)shell_size(n);
    sh->slot_count = shell_slots(n);
}

static inline void  shell_set(Shell *sh, uint16_t idx)
    { sh->flags[idx >> 6] |=  (1ULL << (idx & 63u)); }
static inline void  shell_clr(Shell *sh, uint16_t idx)
    { sh->flags[idx >> 6] &= ~(1ULL << (idx & 63u)); }
static inline int   shell_get(const Shell *sh, uint16_t idx)
    { return (int)((sh->flags[idx >> 6] >> (idx & 63u)) & 1u); }
static inline int   shell_any(const Shell *sh)
    { for (int i=0;i<77;i++) if (sh->flags[i]) return 1; return 0; }

/* ── Slot ──────────────────────────────────────────────────────────── */
typedef struct {
    uint16_t x, y, z;
    uint16_t idx;
} Slot;

static inline Slot slot_from_idx(uint16_t idx, uint8_t size) {
    Slot s; s.idx = idx;
    s.z = (uint16_t)(idx / ((uint16_t)size * size));
    s.y = (uint16_t)((idx / size) % size);
    s.x = (uint16_t)(idx % size);
    return s;
}
static inline uint16_t slot_to_idx(uint16_t x, uint16_t y, uint16_t z, uint8_t size) {
    return (uint16_t)(x + y*(uint16_t)size + z*(uint16_t)size*(uint16_t)size);
}

static inline uint32_t slot_global(uint8_t n, uint16_t local_idx) {
    return ((uint32_t)n << SHELL_IDX_BITS) | (local_idx & SHELL_IDX_MASK);
}
static inline uint8_t  global_level(uint32_t gidx) { return (uint8_t)(gidx >> SHELL_IDX_BITS); }
static inline uint16_t global_local(uint32_t gidx) { return (uint16_t)(gidx & SHELL_IDX_MASK); }

/* ── Level classifier v2 ─────────────────────────────────────────── */
static inline int shell_classify_score(const uint8_t chunk[64]) {
    const DiamondBlock *b = (const DiamondBlock *)chunk;
    uint64_t isect = fold_fibo_intersect(b);
    int pc = __builtin_popcountll(isect);
    uint16_t hist[256] = {0};
    uint32_t sum = 0; int uniq = 0;
    for (int i = 0; i < 64; i++) {
        uint8_t v = chunk[i];
        if (hist[v] == 0) uniq++;
        hist[v]++; sum += v;
    }
    uint8_t mean = (uint8_t)(sum / 64);
    uint32_t abs_dev = 0;
    for (int i = 0; i < 64; i++) {
        int d = (int)chunk[i] - mean;
        abs_dev += d < 0 ? (uint32_t)(-d) : (uint32_t)d;
    }
    int var_norm = (int)(abs_dev / 128);
    return pc + uniq - var_norm;
}

static const int8_t SCORE_THRESH[9] = { 2, 6, 12, 20, 30, 40, 50, 60, 120 };

static inline uint8_t shell_classify_level(const uint8_t chunk[64]) {
    int score = shell_classify_score(chunk);
    for (uint8_t n = 0; n <= SHELL_MAX_LEVEL; n++)
        if (score <= SCORE_THRESH[n]) return n;
    return SHELL_MAX_LEVEL;
}

/* ── map chunk → slot index ───────────────────────────────────────── */
#define FNV64_OFFSET 14695981039346656037ULL
#define FNV64_PRIME  1099511628211ULL

static inline uint64_t _fnv64(const uint8_t *data, uint32_t len) {
    uint64_t h = FNV64_OFFSET;
    for (uint32_t i = 0; i < len; i++)
        h = (h ^ data[i]) * FNV64_PRIME;
    return h;
}

static inline uint16_t chunk_to_slot_idx(const uint8_t chunk[64], uint8_t n) {
    uint16_t cap = shell_slots(n);
    if (cap == 0) return 0;
    return (uint16_t)(_fnv64(chunk, 64) % cap);
}

/* ── Index — slot → tick mapping ──────────────────────────────────── */
#define INDEX_SIZE ((9u << SHELL_IDX_BITS))   /* 9*8192 = 73728 entries */

typedef struct {
    uint32_t tick[INDEX_SIZE];  /* SLOT_NULL if empty */
} SlotIndex;

static inline void sidx_init(SlotIndex *si) {
    memset(si->tick, 0xFF, sizeof(si->tick));
}
static inline void    sidx_set(SlotIndex *si, uint32_t gidx, uint32_t tick)
    { if (gidx < INDEX_SIZE) si->tick[gidx] = tick; }
static inline uint32_t sidx_get(const SlotIndex *si, uint32_t gidx)
    { return (gidx < INDEX_SIZE) ? si->tick[gidx] : SLOT_NULL; }
static inline void    sidx_clear(SlotIndex *si, uint32_t gidx)
    { if (gidx < INDEX_SIZE) si->tick[gidx] = SLOT_NULL; }

/* batch sidx pack/unpack: non-batch = tick directly; batch = bit31|(tick<<8)|pos */
#define BATCH_SIDX_FLAG (1u << 31)

static inline uint32_t sidx_pack_batch(uint32_t tick, uint8_t pos) {
    return BATCH_SIDX_FLAG | (tick << 8) | pos;
}
static inline int      sidx_is_batch(uint32_t v)   { return (v & BATCH_SIDX_FLAG) != 0; }
static inline uint32_t sidx_batch_tick(uint32_t v) { return (v >> 8) & 0x7FFFFF; }
static inline uint8_t  sidx_batch_pos(uint32_t v)  { return (uint8_t)(v & 0xFF); }

/* ── DiamondField ──────────────────────────────────────────────────── */
typedef struct {
    Shell     shell[9];
    Tring     tring;
    SlotIndex sidx;
} DiamondField;

static inline int dfield_init(DiamondField *df, uint32_t tring_cap) {
    for (uint8_t n = 0; n <= SHELL_MAX_LEVEL; n++)
        shell_init(&df->shell[n], n);
    sidx_init(&df->sidx);
    return tring_init(&df->tring, tring_cap ? tring_cap : TRING_MAX_TICKS);
}

static inline void dfield_free(DiamondField *df) { tring_destroy(&df->tring); }

/* ── Sparse encode / decode ───────────────────────────────────────── */
static inline uint32_t sparse_encode(uint8_t out[66], const uint8_t chunk[64]) {
    uint8_t idx[64], val[64]; int count = 0;
    for (int i = 0; i < 64; i++) { if (chunk[i]) { idx[count]=(uint8_t)i; val[count]=chunk[i]; count++; } }
    if (count == 0) { out[0] = 0; return 1; }
    if (1 + 2 * count >= 64) return 0;
    out[0] = (uint8_t)count;
    for (int i = 0; i < count; i++) { out[1+i*2]=idx[i]; out[1+i*2+1]=val[i]; }
    return (uint32_t)(1 + count * 2);
}

static inline void sparse_decode(uint8_t out[64], const uint8_t *in, uint32_t size) {
    memset(out, 0, 64);
    if (!in || size < 1) return;
    int count = (int)in[0]; if (count > 31) count = 31;
    for (int i = 0; i < count; i++) { uint8_t p = in[1+i*2]; if (p < 64) out[p] = in[1+i*2+1]; }
}

/* ── Batch adaptive bitmask diff ──────────────────────────────────── */
/*
 * Wire format per chunk: [mask:8B][values:count*1B]
 *   mask    = 64-bit bitmask: bit i set → byte i differs from base
 *   values  = new byte values for each set bit (in position order)
 *   Returns 8 + count bytes, or 0 if count > BATCH_MAX_DIFF (fallback)
 */
static inline uint32_t batch_encode_diff(uint8_t out[], const uint8_t base[64], const uint8_t chunk[64]) {
    uint64_t mask = 0;
    uint8_t vals[64];
    int count = 0;
    for (int i = 0; i < 64; i++) {
        if (chunk[i] != base[i]) {
            mask |= (1ULL << i);
            vals[count++] = chunk[i];
        }
    }
    if (count > BATCH_MAX_DIFF) return 0;
    memcpy(out, &mask, 8);
    memcpy(out + 8, vals, (size_t)count);
    return 8 + (uint32_t)count;
}

/* Reconstruct chunk from base + bitmask diff. diff must be valid. */
static inline void batch_decode_diff(uint8_t out[64], const uint8_t base[64], const uint8_t diff_data[]) {
    memcpy(out, base, 64);
    uint64_t mask;
    memcpy(&mask, diff_data, 8);
    const uint8_t *vals = diff_data + 8;
    int vi = 0;
    for (int i = 0; i < 64; i++)
        if (mask & (1ULL << i)) out[i] = vals[vi++];
}

/* Given batch_data (tring node content), find byte offset of diff for chunk pos */
static inline uint32_t batch_diff_offset(const uint8_t *batch_data, uint8_t pos) {
    uint32_t off = 4 + 64; /* skip header + base */
    for (uint8_t j = 0; j < pos; j++) {
        uint64_t mask;
        memcpy(&mask, batch_data + off, 8);
        off += 8 + __builtin_popcountll(mask);
    }
    return off;
}

/* Compute base chunk as byte-wise median across N chunks */
static inline void batch_compute_base(uint8_t base[64], const uint8_t *chunks, uint32_t count, uint32_t stride) {
    /* for each byte position, pick median value */
    uint8_t vals[512]; /* max BATCH_L3 = 512 */
    for (int pos = 0; pos < 64; pos++) {
        for (uint32_t i = 0; i < count; i++) vals[i] = chunks[i * stride + pos];
        /* simple median: sort partial */
        for (uint32_t i = 0; i < count; i++)
            for (uint32_t j = i + 1; j < count; j++)
                if (vals[i] > vals[j]) { uint8_t t = vals[i]; vals[i] = vals[j]; vals[j] = t; }
        base[pos] = vals[count / 2];
    }
}

/* ── ENCODE (single chunk) ────────────────────────────────────────── */
static inline uint32_t dfield_encode(DiamondField *df,
                                      const uint8_t  chunk[64],
                                      uint8_t       *out_level)
{
    uint8_t n = shell_classify_level(chunk);
    if (out_level) *out_level = n;
    uint16_t local_idx = chunk_to_slot_idx(chunk, n);
    uint32_t gidx      = slot_global(n, local_idx);
    while (n <= SHELL_MAX_LEVEL) {
        uint16_t cap = df->shell[n].slot_count;
        uint16_t probe = chunk_to_slot_idx(chunk, n);
        int found = 0;
        for (uint16_t i = 0; i < cap; i++) {
            uint32_t g_probe = slot_global(n, probe);
            if (!shell_get(&df->shell[n], probe) &&
                sidx_get(&df->sidx, g_probe) == SLOT_NULL) {
                local_idx = probe; gidx = g_probe; found = 1; break;
            }
            probe = (uint16_t)((probe + 1u) % cap);
        }
        if (found) { if (out_level) *out_level = n; break; }
        n++;
    }
    if (n > SHELL_MAX_LEVEL) return SLOT_NULL;

    uint32_t tick;
    if (n <= SPARSE_MAX_LEVEL) {
        uint8_t sbuf[66]; uint32_t ssz = sparse_encode(sbuf, chunk);
        if (ssz > 0 && ssz < 64) tick = tring_push(&df->tring, sbuf, ssz);
        else                     tick = tring_push(&df->tring, chunk, 64);
    } else {
        tick = tring_push(&df->tring, chunk, 64);
    }
    if (tick == UINT32_MAX) return SLOT_NULL;
    shell_set(&df->shell[n], local_idx);
    sidx_set(&df->sidx, gidx, tick);
    return gidx;
}

/* ── ENCODE BATCH (adaptive bitmask diff) ─────────────────────────── */
/*
 * Encode K consecutive chunks as a batch in one tring node.
 * Wire format (variable size):
 *   [layer:1B][best_rot:1B][count:1B][flags:1B] = 4B header
 *   [base_chunk:64B]                              = base (full 64B)
 *   [mask0:8B][vals0:popcnt(mask0)*1B]            = diff for chunk 0
 *   [mask1:8B][vals1:popcnt(mask1)*1B]            = diff for chunk 1
 *   ...
 *
 * Adaptive: up to BATCH_MAX_DIFF (12) differing bytes per chunk.
 * If any chunk exceeds BATCH_MAX_DIFF, returns 0 (don't batch).
 *
 * Returns number of chunks encoded, or 0 on error/fallback.
 */
static inline uint32_t dfield_encode_batch(DiamondField *df,
                                             const uint8_t *chunks,  /* K × 64B */
                                             uint32_t       K,
                                             uint8_t        layer,   /* 0=L1,1=L2,2=L3 */
                                             uint32_t       gidxs_out[])
{
    if (K == 0 || K > BATCH_L3) return 0;
    uint32_t cap = layer == 0 ? BATCH_L1 : layer == 1 ? BATCH_L2 : BATCH_L3;
    if (K > cap) K = cap;

    /* 1. classify */
    uint8_t n = shell_classify_level(chunks);

    /* 2. compute base + pre-encode all diffs, check if all pass */
    uint8_t base[64];
    batch_compute_base(base, chunks, K, 64);

    uint8_t diff_cache[512][12];  /* max 512 × (8+12) = 10KB */
    uint32_t diff_sizes[512];
    uint32_t total_diff_size = 0;
    int all_pass = 1;
    for (uint32_t i = 0; i < K; i++) {
        diff_sizes[i] = batch_encode_diff(diff_cache[i], base, chunks + i * 64);
        if (diff_sizes[i] == 0) { all_pass = 0; break; }
        total_diff_size += diff_sizes[i];
    }
    if (!all_pass) return 0;  /* fallback: don't batch */

    /* 3. build wire format */
    uint32_t wire_size = 4 + 64 + total_diff_size;
    uint8_t wire[4 + 64 + 512 * (8 + BATCH_MAX_DIFF)]; /* max L3 */
    wire[0] = (uint8_t)layer;
    wire[1] = 0; /* best_rot=0 for now */
    wire[2] = (uint8_t)K;
    wire[3] = 0;
    memcpy(wire + 4, base, 64);
    uint32_t woff = 4 + 64;
    for (uint32_t i = 0; i < K; i++) {
        memcpy(wire + woff, diff_cache[i], diff_sizes[i]);
        woff += diff_sizes[i];
    }

    uint32_t batch_tick = tring_push(&df->tring, wire, wire_size);
    if (batch_tick == UINT32_MAX) return 0;

    /* 4. for each chunk: map + probe slot + set shell + sidx */
    uint32_t encoded = 0;
    for (uint32_t i = 0; i < K; i++) {
        const uint8_t *chunk = chunks + i * 64;
        uint8_t probe_n = n;
        uint16_t local_idx;
        uint32_t gidx;
        while (probe_n <= SHELL_MAX_LEVEL) {
            uint16_t cap2 = df->shell[probe_n].slot_count;
            uint16_t probe = chunk_to_slot_idx(chunk, probe_n);
            int found = 0;
            for (uint16_t j = 0; j < cap2; j++) {
                uint32_t gp = slot_global(probe_n, probe);
                if (!shell_get(&df->shell[probe_n], probe) &&
                    sidx_get(&df->sidx, gp) == SLOT_NULL) {
                    local_idx = probe; gidx = gp; found = 1; break;
                }
                probe = (uint16_t)((probe + 1u) % cap2);
            }
            if (found) break;
            probe_n++;
        }
        if (probe_n > SHELL_MAX_LEVEL) break;

        uint32_t packed = sidx_pack_batch(batch_tick, (uint8_t)i);
        shell_set(&df->shell[probe_n], local_idx);
        sidx_set(&df->sidx, gidx, packed);
        gidxs_out[encoded++] = gidx;
    }
    return encoded;
}

/* ── DECODE (single, handles batch and non-batch) ──────────────────── */
static inline int dfield_decode(const DiamondField *df,
                                 uint32_t            gidx,
                                 uint8_t             out[64])
{
    uint8_t  n   = global_level(gidx);
    uint16_t idx = global_local(gidx);
    if (n > SHELL_MAX_LEVEL) return -1;
    if (!shell_get(&df->shell[n], idx)) return -1;

    uint32_t sidx_val = sidx_get(&df->sidx, gidx);
    if (sidx_val == SLOT_NULL) return -1;

    if (!sidx_is_batch(sidx_val)) {
        /* non-batch: direct tick */
        uint32_t stored_size;
        const uint8_t *data = tring_read(&df->tring, sidx_val, &stored_size);
        if (!data) return -1;
        if (stored_size < 64) sparse_decode(out, data, stored_size);
        else                  memcpy(out, data, 64);
        return 0;
    }

    /* batch: extract chunk from batch (adaptive variable-size diff) */
    uint32_t batch_tick = sidx_batch_tick(sidx_val);
    uint8_t  batch_pos  = sidx_batch_pos(sidx_val);
    uint32_t sz;
    const uint8_t *batch_data = tring_read(&df->tring, batch_tick, &sz);
    if (!batch_data) return -1;

    uint32_t doff = batch_diff_offset(batch_data, batch_pos);
    if (doff + 8 > sz) return -1;  /* at least need mask */
    const uint8_t *base = batch_data + 4;
    batch_decode_diff(out, base, batch_data + doff);
    return 0;
}

/* ── DELETE O(1) ────────────────────────────────────────────────────── */
static inline void dfield_delete(DiamondField *df, uint32_t gidx) {
    uint8_t n   = global_level(gidx);
    uint16_t idx = global_local(gidx);
    if (n > SHELL_MAX_LEVEL) return;
    shell_clr(&df->shell[n], idx);
}

/* ── GC callback ──────────────────────────────────────────────────── */
static inline int _dfield_tick_referenced(uint32_t tick, void *ctx) {
    DiamondField *df = (DiamondField *)ctx;
    for (uint32_t gidx = 0; gidx < INDEX_SIZE; gidx++) {
        uint32_t sidx_val = sidx_get(&df->sidx, gidx);
        if (sidx_val == SLOT_NULL) continue;
        uint32_t entry_tick = sidx_is_batch(sidx_val) ? sidx_batch_tick(sidx_val) : sidx_val;
        if (entry_tick != tick) continue;
        uint8_t  n   = global_level(gidx);
        uint16_t idx = global_local(gidx);
        if (n <= SHELL_MAX_LEVEL && shell_get(&df->shell[n], idx))
            return 1;
    }
    return 0;
}

static inline uint32_t dfield_gc(DiamondField *df) {
    return tring_gc_scan(&df->tring, _dfield_tick_referenced, df);
}

/* ── RESHAPE ───────────────────────────────────────────────────────── */
static inline uint32_t dfield_reshape(DiamondField *df, uint8_t n_old, uint8_t n_new) {
    if (n_old > SHELL_MAX_LEVEL || n_new > SHELL_MAX_LEVEL) return 0;
    uint32_t remapped = 0;
    uint16_t cap = df->shell[n_old].slot_count;
    for (uint16_t idx = 0; idx < cap; idx++) {
        if (!shell_get(&df->shell[n_old], idx)) continue;
        uint32_t gidx_old = slot_global(n_old, idx);
        uint32_t sidx_val = sidx_get(&df->sidx, gidx_old);
        if (sidx_val == SLOT_NULL) continue;

        uint32_t tick;
        uint8_t  batch_pos;
        int      is_batch;
        if (sidx_is_batch(sidx_val)) {
            tick = sidx_batch_tick(sidx_val);
            batch_pos = sidx_batch_pos(sidx_val);
            is_batch = 1;
        } else {
            tick = sidx_val;
            batch_pos = 0;
            is_batch = 0;
        }

        uint8_t chunk[64];
        if (is_batch) {
            uint32_t sz;
            const uint8_t *bd = tring_read(&df->tring, tick, &sz);
            if (!bd) continue;
            uint32_t doff = batch_diff_offset(bd, batch_pos);
            if (doff + 8 > sz) continue;
            batch_decode_diff(chunk, bd + 4, bd + doff);
        } else {
            uint32_t sz;
            const uint8_t *raw = tring_read(&df->tring, tick, &sz);
            if (!raw) continue;
            if (sz < 64) sparse_decode(chunk, raw, sz);
            else         memcpy(chunk, raw, 64);
        }

        uint16_t new_local = chunk_to_slot_idx(chunk, n_new);
        uint16_t new_cap = df->shell[n_new].slot_count;
        for (uint16_t p = 0; p < new_cap; p++) {
            if (!shell_get(&df->shell[n_new], new_local)) break;
            new_local = (uint16_t)((new_local + 1u) % new_cap);
        }
        uint32_t gidx_new = slot_global(n_new, new_local);

        shell_clr(&df->shell[n_old], idx);
        sidx_clear(&df->sidx, gidx_old);
        shell_set(&df->shell[n_new], new_local);
        sidx_set(&df->sidx, gidx_new, sidx_val);  /* keep same sidx encoding */
        remapped++;
    }
    return remapped;
}

/* ── Fingerprint — byte-similarity grouping key ──────────────────── */
/*
 * Uses chunk byte-sum (coarse average) XOR content hash to group
 * byte-similar chunks regardless of position in the stream.
 * For repetitive data (geometric, structs, logs), same-fingerprint
 * chunks share most bytes → few diffs → batch succeeds.
 */
static inline uint8_t shell_fingerprint(const uint8_t chunk[64], uint8_t num_buckets) {
    uint32_t sum = 0;
    uint64_t w0, w1;
    memcpy(&w0, chunk, 8);
    memcpy(&w1, chunk + 8, 8);
    for (int i = 0; i < 64; i++) sum += chunk[i];
    /* coarse key: byte-sum high bits XOR first 16 bytes rotated */
    uint8_t h = (uint8_t)(((sum >> 6) ^ (w0 >> 40) ^ (w1 >> 24)) % num_buckets);
    return h;
}

/* ── ENCODE WINDOWED — group by fingerprint, batch per group ────── */
static inline uint32_t dfield_encode_windowed(DiamondField *df,
                                               const uint8_t *chunks,
                                               uint32_t       window_size,
                                               uint8_t        num_buckets,
                                               uint32_t       gidxs_out[])
{
    if (window_size == 0) return 0;
    if (window_size > 256) window_size = 256;
    if (num_buckets == 0) num_buckets = 8;

    uint8_t fps[256];
    for (uint32_t i = 0; i < window_size; i++)
        fps[i] = shell_fingerprint(chunks + i * 64, num_buckets);

    uint8_t  bucket_fp[256]; uint32_t bucket_cnt[256] = {0};
    uint32_t num_buckets_used = 0;
    for (uint32_t i = 0; i < window_size; i++) {
        uint8_t fp = fps[i];
        uint32_t b;
        for (b = 0; b < num_buckets_used; b++) if (bucket_fp[b] == fp) break;
        if (b == num_buckets_used) { if (b >= 256) break; bucket_fp[b] = fp; num_buckets_used++; }
        bucket_cnt[b]++;
    }

    uint32_t bucket_chunks[256][256] = {{0}};
    uint32_t bucket_idx[256] = {0};
    for (uint32_t i = 0; i < window_size; i++) {
        uint8_t fp = fps[i];
        uint32_t b;
        for (b = 0; b < num_buckets_used; b++) if (bucket_fp[b] == fp) break;
        bucket_chunks[b][bucket_idx[b]++] = i;
    }

    /* encode each bucket, map results back to original position */
    uint32_t gidx_map[256];
    memset(gidx_map, 0xFF, sizeof(gidx_map)); /* SLOT_NULL fill */
    for (uint32_t b = 0; b < num_buckets_used; b++) {
        uint32_t cnt = bucket_cnt[b];
        if (cnt < 2) {
            for (uint32_t j = 0; j < cnt; j++) {
                uint32_t ci = bucket_chunks[b][j];
                gidx_map[ci] = dfield_encode(df, chunks + ci * 64, NULL);
            }
        } else {
            uint8_t temp[256 * 64];
            for (uint32_t j = 0; j < cnt; j++) {
                uint32_t ci = bucket_chunks[b][j];
                memcpy(temp + j * 64, chunks + ci * 64, 64);
            }
            uint32_t gb[256];
            uint32_t e = dfield_encode_batch(df, temp, cnt, 0, gb);
            if (e > 0) {
                for (uint32_t j = 0; j < e; j++) {
                    uint32_t ci = bucket_chunks[b][j];
                    gidx_map[ci] = gb[j];
                }
                for (uint32_t j = e; j < cnt; j++) {
                    uint32_t ci = bucket_chunks[b][j];
                    gidx_map[ci] = dfield_encode(df, chunks + ci * 64, NULL);
                }
            } else {
                for (uint32_t j = 0; j < cnt; j++) {
                    uint32_t ci = bucket_chunks[b][j];
                    gidx_map[ci] = dfield_encode(df, chunks + ci * 64, NULL);
                }
            }
        }
    }
    uint32_t total = 0;
    for (uint32_t i = 0; i < window_size; i++)
        if (gidx_map[i] != SLOT_NULL)
            gidxs_out[total++] = gidx_map[i];
    return total;
}

#endif /* GEO_DIAMOND_FIELD_H */
