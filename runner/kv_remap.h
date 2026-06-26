#ifndef KV_REMAP_H
#define KV_REMAP_H

/*
 * KV Remap — Adaptive skeleton + delta KV cache management
 * ══════════════════════════════════════════════════════════
 *
 * Three-tier adaptive system:
 *   0-15% change:  XOR delta, RLE compressed (small, precise)
 *   15-85% change: byte-offset ranges (fast, topology)
 *   85%+ change:   Rebuild skeleton (new baseline)
 *
 * Self-contained — no zstd/diamond dependencies.
 * For production: swap compress/decompress with Binary Shell.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Thresholds ─────────────────────────────────────────── */
#define KV_REMAP_THRESH_LOW     15
#define KV_REMAP_THRESH_HIGH    85

/* ── Delta types ────────────────────────────────────────── */
#define DELTA_NONE      0
#define DELTA_ENTROPY   1   /* XOR diff, RLE compressed */
#define DELTA_GEO       2   /* byte-offset ranges */
#define DELTA_REBUILD   3

/* ── Storage limits ─────────────────────────────────────── */
#define KV_REMAP_MAX_LAYERS      64
#define KV_REMAP_MAX_GEO_RANGES  4096

/* ── Geo coordinate range ───────────────────────────────── */
typedef struct {
    uint32_t start;
    uint32_t length;
    uint16_t layer;
    uint8_t  direction;
} GeoRange;

/* ── RLE header ─────────────────────────────────────────── */
#define RLE_MAGIC  0x524C4531  /* "RLE1" */

typedef struct {
    uint32_t magic;
    uint32_t orig_size;
    uint32_t comp_size;
    uint32_t n_runs;
} RLEHeader;

typedef struct {
    uint16_t zero_run;
    uint8_t  data_len;
    /* followed by data_len bytes of literal data */
} RLERun;

/* ── Delta storage ──────────────────────────────────────── */
typedef struct {
    uint8_t  type;
    uint16_t change_pct;

    void    *entropy_data;
    size_t   entropy_size;

    GeoRange ranges[KV_REMAP_MAX_GEO_RANGES];
    uint32_t n_ranges;
    uint32_t total_changed_positions;

    void    *geo_data;      /* actual byte values for changed ranges */
    size_t   geo_data_size;

    size_t   delta_size;
} KVRemapDelta;

/* ── Layer info ─────────────────────────────────────────── */
typedef struct {
    void    *k_data;
    void    *v_data;
    size_t   k_nb1;
    size_t   v_nb1;
    size_t   k_size;
    size_t   v_size;
    int      n_embd;
    int      layer_id;
} KVRemapLayer;

/* ── Main context ───────────────────────────────────────── */
typedef struct {
    int      n_ctx;
    int      n_embd;
    int      n_layers;
    size_t   layer_kv_bytes;
    size_t   total_kv_bytes;

    KVRemapLayer layers[KV_REMAP_MAX_LAYERS];

    uint8_t *skeleton_data;
    size_t   skeleton_orig;
    size_t   skeleton_comp;
    int      skeleton_valid;

    KVRemapDelta delta;

    uint8_t *ref_skeleton;
    size_t   ref_size;

    uint32_t n_rebuilds;
    uint32_t n_delta_stores;
    uint32_t n_restores;
    double   total_remap_time_ms;

    int      enabled;
} KVRemapCtx;


/* =============================================================
 * RLE compress: XOR diff → [zero_run][data_len][data...]
 * ============================================================= */

static inline int kv_remap_compress(const void *data, size_t size,
                                    void **out, size_t *out_size)
{
    const uint8_t *src = (const uint8_t *)data;
    /* Worst case: every byte is non-zero → 1 literal per byte + overhead */
    size_t max_out = sizeof(RLEHeader) + size * 3 + 256;
    uint8_t *buf = (uint8_t *)malloc(max_out);
    if (!buf) return -1;

    RLEHeader *hdr = (RLEHeader *)buf;
    uint8_t *dst = buf + sizeof(RLEHeader);

    uint32_t n_runs = 0;
    size_t i = 0;

    while (i < size) {
        /* Count zero run */
        uint16_t zero_run = 0;
        while (i < size && src[i] == 0 && zero_run < 65535) {
            zero_run++;
            i++;
        }

        /* Collect non-zero literal run */
        size_t data_start = i;
        uint8_t data_len = 0;
        while (i < size && src[i] != 0 && data_len < 255) {
            data_len++;
            i++;
        }

        /* Write run */
        RLERun *run = (RLERun *)dst;
        run->zero_run = zero_run;
        run->data_len = data_len;
        dst += sizeof(RLERun);
        if (data_len > 0) {
            memcpy(dst, src + data_start, data_len);
            dst += data_len;
        }
        n_runs++;
    }

    size_t comp = (size_t)(dst - buf);
    hdr->magic = RLE_MAGIC;
    hdr->orig_size = (uint32_t)size;
    hdr->comp_size = (uint32_t)comp;
    hdr->n_runs = n_runs;

    /* If compressed is larger, store raw */
    double ratio = (double)size / (double)(comp > sizeof(RLEHeader) ?
        comp - sizeof(RLEHeader) : 1);
    if (comp >= size || ratio < 1.05) {
        free(buf);
        size_t raw_sz = sizeof(RLEHeader) + size;
        uint8_t *raw = (uint8_t *)malloc(raw_sz);
        if (!raw) return -1;
        RLEHeader *rh = (RLEHeader *)raw;
        rh->magic = RLE_MAGIC;
        rh->orig_size = (uint32_t)size;
        rh->comp_size = (uint32_t)raw_sz;
        rh->n_runs = 0; /* n_runs=0 means raw data */
        memcpy(raw + sizeof(RLEHeader), data, size);
        *out = raw;
        *out_size = raw_sz;
        return 1;
    }

    *out = buf;
    *out_size = comp;
    return 0;
}


/* =============================================================
 * RLE decompress
 * ============================================================= */

static inline void *kv_remap_decompress(const void *compressed, size_t comp_size,
                                        size_t *out_size)
{
    if (comp_size < sizeof(RLEHeader)) return NULL;
    const RLEHeader *hdr = (const RLEHeader *)compressed;

    if (hdr->magic != RLE_MAGIC) return NULL;

    size_t orig = hdr->orig_size;
    if (out_size) *out_size = orig;

    if (hdr->n_runs == 0) {
        /* Raw data */
        uint8_t *out = (uint8_t *)malloc(orig > 0 ? orig : 1);
        if (!out) return NULL;
        memcpy(out, (const uint8_t *)compressed + sizeof(RLEHeader), orig);
        return out;
    }

    uint8_t *out = (uint8_t *)malloc(orig > 0 ? orig : 1);
    if (!out) return NULL;

    const uint8_t *src = (const uint8_t *)compressed + sizeof(RLEHeader);
    size_t dst_pos = 0;

    for (uint32_t r = 0; r < hdr->n_runs; r++) {
        const RLERun *run = (const RLERun *)src;
        uint16_t zeros = run->zero_run;
        uint8_t  dlen  = run->data_len;
        src += sizeof(RLERun);

        /* Write zeros */
        for (uint16_t z = 0; z < zeros && dst_pos < orig; z++)
            out[dst_pos++] = 0;

        /* Write literal data */
        for (uint8_t d = 0; d < dlen && dst_pos < orig; d++)
            out[dst_pos++] = src[d];
        src += dlen;
    }

    return out;
}


/* =============================================================
 * Init
 * ============================================================= */

static inline void kv_remap_init(KVRemapCtx *ctx, int n_ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->n_ctx = n_ctx;
    ctx->delta.type = DELTA_NONE;
    fprintf(stderr, "[kv-remap] init: n_ctx=%d, thresholds=%d/%d%%\n",
        n_ctx, KV_REMAP_THRESH_LOW, KV_REMAP_THRESH_HIGH);
}


/* =============================================================
 * Register layers
 * ============================================================= */

static inline void kv_remap_register(KVRemapCtx *ctx,
    void **k_data, void **v_data,
    size_t *k_nb1, size_t *v_nb1,
    size_t *k_size, size_t *v_size,
    int *n_embd_k, int *layer_id,
    int n_layers)
{
    if (n_layers > KV_REMAP_MAX_LAYERS) n_layers = KV_REMAP_MAX_LAYERS;
    ctx->n_layers = n_layers;
    ctx->total_kv_bytes = 0;

    for (int i = 0; i < n_layers; i++) {
        ctx->layers[i].k_data   = k_data[i];
        ctx->layers[i].v_data   = v_data[i];
        ctx->layers[i].k_nb1    = k_nb1[i];
        ctx->layers[i].v_nb1    = v_nb1[i];
        ctx->layers[i].k_size   = k_size[i];
        ctx->layers[i].v_size   = v_size[i];
        ctx->layers[i].n_embd   = n_embd_k[i];
        ctx->layers[i].layer_id = layer_id[i];
        ctx->total_kv_bytes += k_size[i] + v_size[i];
    }

    ctx->n_embd = ctx->layers[0].n_embd;
    ctx->layer_kv_bytes = ctx->total_kv_bytes / (size_t)n_layers;
    ctx->enabled = 1;
}


/* =============================================================
 * Set skeleton
 * ============================================================= */

static inline int kv_remap_set_skeleton(KVRemapCtx *ctx) {
    if (!ctx->enabled) return -1;

    free(ctx->skeleton_data);
    ctx->skeleton_data = NULL;
    ctx->skeleton_comp = 0;

    ctx->skeleton_orig = ctx->total_kv_bytes;
    fprintf(stderr, "[kv-remap] set_skeleton: n_layers=%d total=%zu bytes\n",
        ctx->n_layers, ctx->skeleton_orig);

    if (ctx->skeleton_orig == 0) return -1;

    uint8_t *buf = (uint8_t *)malloc(ctx->skeleton_orig);
    if (!buf) {
        fprintf(stderr, "[kv-remap] OOM: cannot alloc %zu bytes for skeleton\n", ctx->skeleton_orig);
        return -1;
    }

    size_t off = 0;
    for (int l = 0; l < ctx->n_layers; l++) {
        fprintf(stderr, "[kv-remap] layer %d: k_data=%p k_size=%zu v_data=%p v_size=%zu\n",
            l, ctx->layers[l].k_data, ctx->layers[l].k_size,
            ctx->layers[l].v_data, ctx->layers[l].v_size);
        if (!ctx->layers[l].k_data || !ctx->layers[l].v_data) {
            fprintf(stderr, "[kv-remap] SKIP layer %d: NULL data pointer\n", l);
            continue;
        }
        if (ctx->layers[l].k_size > 0) {
            memcpy(buf + off, ctx->layers[l].k_data, ctx->layers[l].k_size);
            off += ctx->layers[l].k_size;
        }
        if (ctx->layers[l].v_size > 0) {
            memcpy(buf + off, ctx->layers[l].v_data, ctx->layers[l].v_size);
            off += ctx->layers[l].v_size;
        }
    }
    fprintf(stderr, "[kv-remap] copied %zu bytes into skeleton buffer\n", off);

    free(ctx->ref_skeleton);
    ctx->ref_skeleton = (uint8_t *)malloc(ctx->skeleton_orig);
    if (ctx->ref_skeleton)
        memcpy(ctx->ref_skeleton, buf, ctx->skeleton_orig);
    ctx->ref_size = ctx->skeleton_orig;

    void *comp = NULL;
    size_t comp_size = 0;
    int r = kv_remap_compress(buf, ctx->skeleton_orig, &comp, &comp_size);
    free(buf);
    if (r < 0 || !comp) return -1;

    ctx->skeleton_data = (uint8_t *)comp;
    ctx->skeleton_comp = comp_size;
    ctx->skeleton_valid = 1;

    double ratio = (double)ctx->skeleton_orig / (double)(comp_size > 0 ? comp_size : 1);
    fprintf(stderr, "[kv-remap] skeleton: orig=%zu comp=%zu ratio=%.2fx\n",
        ctx->skeleton_orig, comp_size, ratio);
    return 0;
}


/* =============================================================
 * Classify
 * ============================================================= */

static inline int kv_remap_classify(KVRemapCtx *ctx) {
    if (!ctx->enabled || !ctx->skeleton_valid || !ctx->ref_skeleton)
        return -1;

    size_t total = ctx->total_kv_bytes;
    uint64_t diff_bytes = 0;

    /* memcmp on 4096-byte strides — ~100x faster than byte-by-byte */
    #define CLASSIFY_STRIDE 4096
    size_t off = 0;
    for (int l = 0; l < ctx->n_layers; l++) {
        const uint8_t *sk_k = ctx->ref_skeleton + off;
        const uint8_t *live_k = (const uint8_t *)ctx->layers[l].k_data;
        size_t k_rem = ctx->layers[l].k_size;
        size_t k_off = 0;
        while (k_rem > 0) {
            size_t chunk = k_rem < CLASSIFY_STRIDE ? k_rem : CLASSIFY_STRIDE;
            if (memcmp(sk_k + k_off, live_k + k_off, chunk) != 0)
                diff_bytes += chunk;
            k_off += chunk;
            k_rem -= chunk;
        }
        off += ctx->layers[l].k_size;

        const uint8_t *sk_v = ctx->ref_skeleton + off;
        const uint8_t *live_v = (const uint8_t *)ctx->layers[l].v_data;
        size_t v_rem = ctx->layers[l].v_size;
        size_t v_off = 0;
        while (v_rem > 0) {
            size_t chunk = v_rem < CLASSIFY_STRIDE ? v_rem : CLASSIFY_STRIDE;
            if (memcmp(sk_v + v_off, live_v + v_off, chunk) != 0)
                diff_bytes += chunk;
            v_off += chunk;
            v_rem -= chunk;
        }
        off += ctx->layers[l].v_size;
    }
    #undef CLASSIFY_STRIDE

    return (int)(diff_bytes * 100 / total);
}


/* =============================================================
 * Build geo ranges (byte-offset, no Hilbert dependency)
 * ============================================================= */

static inline uint32_t kv_remap_build_geo_ranges(
    const uint8_t *diff, size_t diff_size,
    GeoRange *ranges, uint32_t max_ranges)
{
    uint32_t n_ranges = 0;
    uint32_t range_start = 0;
    int in_range = 0;

    for (size_t i = 0; i < diff_size; i++) {
        if (diff[i] != 0) {
            if (!in_range) {
                range_start = (uint32_t)i;
                in_range = 1;
            }
        } else {
            if (in_range) {
                if (n_ranges < max_ranges) {
                    ranges[n_ranges].start = range_start;
                    ranges[n_ranges].length = (uint32_t)i - range_start;
                    ranges[n_ranges].layer = 0;
                    ranges[n_ranges].direction = 0;
                    n_ranges++;
                }
                in_range = 0;
            }
        }
    }

    if (in_range && n_ranges < max_ranges) {
        ranges[n_ranges].start = range_start;
        ranges[n_ranges].length = (uint32_t)diff_size - range_start;
        ranges[n_ranges].layer = 0;
        ranges[n_ranges].direction = 0;
        n_ranges++;
    }

    return n_ranges;
}


/* =============================================================
 * Store delta
 * ============================================================= */

static inline int kv_remap_store_delta(KVRemapCtx *ctx, int change_pct) {
    if (!ctx->enabled) return -1;

    free(ctx->delta.entropy_data);
    ctx->delta.entropy_data = NULL;
    ctx->delta.entropy_size = 0;
    free(ctx->delta.geo_data);
    ctx->delta.geo_data = NULL;
    ctx->delta.geo_data_size = 0;
    ctx->delta.n_ranges = 0;
    ctx->delta.change_pct = (uint16_t)change_pct;

    if (change_pct == 0) {
        ctx->delta.type = DELTA_NONE;
        ctx->delta.delta_size = 0;
        return 0;
    }

    size_t total = ctx->total_kv_bytes;
    uint8_t *diff = (uint8_t *)malloc(total);
    if (!diff) return -1;

    size_t off = 0;
    for (int l = 0; l < ctx->n_layers; l++) {
        const uint8_t *sk_k = ctx->ref_skeleton + off;
        const uint8_t *live_k = (const uint8_t *)ctx->layers[l].k_data;
        for (size_t i = 0; i < ctx->layers[l].k_size; i++)
            diff[off + i] = sk_k[i] ^ live_k[i];
        off += ctx->layers[l].k_size;

        const uint8_t *sk_v = ctx->ref_skeleton + off;
        const uint8_t *live_v = (const uint8_t *)ctx->layers[l].v_data;
        for (size_t i = 0; i < ctx->layers[l].v_size; i++)
            diff[off + i] = sk_v[i] ^ live_v[i];
        off += ctx->layers[l].v_size;
    }

    if (change_pct <= KV_REMAP_THRESH_LOW) {
        ctx->delta.type = DELTA_ENTROPY;
        void *comp = NULL;
        size_t comp_size = 0;
        int r = kv_remap_compress(diff, total, &comp, &comp_size);
        free(diff);
        if (r < 0 || !comp) return -1;

        ctx->delta.entropy_data = comp;
        ctx->delta.entropy_size = comp_size;
        ctx->delta.delta_size = comp_size;

        double ratio = (double)total / (double)(comp_size > sizeof(RLEHeader) ?
            comp_size - sizeof(RLEHeader) : 1);
        fprintf(stderr, "[kv-remap] store: DELTA_ENTROPY (%d%%), raw_xor=%zu, compressed=%zu, ratio=%.2fx\n",
            change_pct, total, comp_size > sizeof(RLEHeader) ? comp_size - sizeof(RLEHeader) : 0, ratio);

    } else {
        ctx->delta.type = DELTA_GEO;
        ctx->delta.n_ranges = kv_remap_build_geo_ranges(
            diff, total, ctx->delta.ranges, KV_REMAP_MAX_GEO_RANGES);

        /* If ranges overflowed, fall back to ENTROPY (full XOR + RLE) */
        /* Count how many ranges there actually are to detect overflow */
        {
            GeoRange tmp_ranges[4096];
            uint32_t actual = kv_remap_build_geo_ranges(
                diff, total, tmp_ranges, 4096);
            if (actual > ctx->delta.n_ranges || actual >= KV_REMAP_MAX_GEO_RANGES) {
                /* Overflow: use ENTROPY instead */
                ctx->delta.type = DELTA_ENTROPY;
                ctx->delta.n_ranges = 0;
                void *comp = NULL;
                size_t comp_size = 0;
                int r = kv_remap_compress(diff, total, &comp, &comp_size);
                free(diff);
                if (r < 0 || !comp) return -1;
                ctx->delta.entropy_data = comp;
                ctx->delta.entropy_size = comp_size;
                ctx->delta.delta_size = comp_size;
                double ratio = (double)total / (double)(comp_size > sizeof(RLEHeader) ?
                    comp_size - sizeof(RLEHeader) : 1);
                fprintf(stderr, "[kv-remap] store: DELTA_GEO→ENTROPY (%d%%, ranges overflow), compressed=%zu, ratio=%.2fx\n",
                    change_pct, comp_size > sizeof(RLEHeader) ? comp_size - sizeof(RLEHeader) : 0, ratio);
                return 0;
            }
        }

        /* Store actual byte values for each changed range */
        uint8_t *gd = NULL;
        size_t gd_off = 0, gd_cap = 0;
        for (uint32_t ri = 0; ri < ctx->delta.n_ranges; ri++) {
            uint32_t start = ctx->delta.ranges[ri].start;
            uint32_t len   = ctx->delta.ranges[ri].length;
            size_t need = gd_off + len;
            if (need > gd_cap) {
                gd_cap = need + 65536;
                uint8_t *ngd = (uint8_t *)realloc(gd, gd_cap);
                if (!ngd) { free(gd); free(diff); return -1; }
                gd = ngd;
            }
            /* Store XOR diff values for this range */
            memcpy(gd + gd_off, diff + start, len);
            gd_off += len;
        }
        free(diff);

        free(ctx->delta.geo_data);
        ctx->delta.geo_data = gd;
        ctx->delta.geo_data_size = gd_off;
        ctx->delta.delta_size = ctx->delta.n_ranges * sizeof(GeoRange) + gd_off;
        ctx->delta.total_changed_positions = (uint32_t)(change_pct * ctx->n_ctx / 100);

        fprintf(stderr, "[kv-remap] store: DELTA_GEO (%d%%), %u ranges, data=%zu, total=%zu bytes\n",
            change_pct, ctx->delta.n_ranges, gd_off, ctx->delta.delta_size);
    }

    ctx->n_delta_stores++;
    return 0;
}


/* =============================================================
 * Restore
 * ============================================================= */

static inline int kv_remap_restore(KVRemapCtx *ctx) {
    if (!ctx->enabled || !ctx->skeleton_valid || !ctx->ref_skeleton) return -1;

    if (ctx->delta.type == DELTA_REBUILD)
        return -1;

    /* ── Step 1: copy skeleton ref to live KV ── */
    size_t off = 0;
    for (int l = 0; l < ctx->n_layers; l++) {
        memcpy(ctx->layers[l].k_data, ctx->ref_skeleton + off, ctx->layers[l].k_size);
        off += ctx->layers[l].k_size;
        memcpy(ctx->layers[l].v_data, ctx->ref_skeleton + off, ctx->layers[l].v_size);
        off += ctx->layers[l].v_size;
    }

    /* ── Step 2: apply delta ── */
    if (ctx->delta.type == DELTA_ENTROPY && ctx->delta.entropy_data) {
        size_t delta_dec_size = 0;
        void *delta_dec = kv_remap_decompress(
            ctx->delta.entropy_data, ctx->delta.entropy_size, &delta_dec_size);
        if (delta_dec) {
            size_t apply = delta_dec_size < ctx->total_kv_bytes ?
                delta_dec_size : ctx->total_kv_bytes;
            /* XOR delta into live KV (already has skeleton) */
            off = 0;
            for (int l = 0; l < ctx->n_layers; l++) {
                for (size_t i = 0; i < ctx->layers[l].k_size && off + i < apply; i++)
                    ((uint8_t *)ctx->layers[l].k_data)[i] ^= ((uint8_t *)delta_dec)[off + i];
                off += ctx->layers[l].k_size;
                for (size_t i = 0; i < ctx->layers[l].v_size && off + i < apply; i++)
                    ((uint8_t *)ctx->layers[l].v_data)[i] ^= ((uint8_t *)delta_dec)[off + i];
                off += ctx->layers[l].v_size;
            }
            free(delta_dec);
        }
    }
    else if (ctx->delta.type == DELTA_GEO && ctx->delta.geo_data && ctx->delta.n_ranges > 0) {
        /* GEO restore: reconstruct flat buffer from skeleton + XOR geo values */
        uint8_t *recon = (uint8_t *)malloc(ctx->total_kv_bytes);
        if (recon) {
            memcpy(recon, ctx->ref_skeleton, ctx->total_kv_bytes);
            const uint8_t *gd = (const uint8_t *)ctx->delta.geo_data;
            size_t gd_off = 0;
            for (uint32_t r = 0; r < ctx->delta.n_ranges; r++) {
                GeoRange *gr = &ctx->delta.ranges[r];
                size_t end = (size_t)gr->start + gr->length;
                if (end > ctx->total_kv_bytes) end = ctx->total_kv_bytes;
                for (size_t i = gr->start; i < end; i++)
                    recon[i] ^= gd[gd_off + (i - gr->start)];
                gd_off += end - gr->start;
            }
            off = 0;
            for (int l = 0; l < ctx->n_layers; l++) {
                memcpy(ctx->layers[l].k_data, recon + off, ctx->layers[l].k_size);
                off += ctx->layers[l].k_size;
                memcpy(ctx->layers[l].v_data, recon + off, ctx->layers[l].v_size);
                off += ctx->layers[l].v_size;
            }
            free(recon);
        }
    }

    ctx->n_restores++;
    fprintf(stderr, "[kv-remap] restore: type=%s pct=%u\n",
        ctx->delta.type == DELTA_ENTROPY ? "ENTROPY" :
        ctx->delta.type == DELTA_GEO ? "GEO" : "NONE",
        ctx->delta.change_pct);
    return 0;
}


/* =============================================================
 * Rebuild
 * ============================================================= */

static inline int kv_remap_rebuild(KVRemapCtx *ctx) {
    if (!ctx->enabled) return -1;

    fprintf(stderr, "[kv-remap] REBUILD: flush + new skeleton\n");

    free(ctx->skeleton_data);
    ctx->skeleton_data = NULL;
    ctx->skeleton_comp = 0;
    ctx->skeleton_valid = 0;

    free(ctx->delta.entropy_data);
    ctx->delta.entropy_data = NULL;
    ctx->delta.entropy_size = 0;
    free(ctx->delta.geo_data);
    ctx->delta.geo_data = NULL;
    ctx->delta.geo_data_size = 0;
    ctx->delta.type = DELTA_REBUILD;
    ctx->delta.n_ranges = 0;

    ctx->n_rebuilds++;
    return kv_remap_set_skeleton(ctx);
}


/* =============================================================
 * Full adaptive cycle
 * ============================================================= */

static inline int kv_remap_cycle(KVRemapCtx *ctx) {
    if (!ctx->enabled) return -1;

    int pct = kv_remap_classify(ctx);
    if (pct < 0) return -1;

    if (pct >= KV_REMAP_THRESH_HIGH) {
        return kv_remap_rebuild(ctx);
    } else if (pct > 0) {
        return kv_remap_store_delta(ctx, pct);
    } else {
        ctx->delta.type = DELTA_NONE;
        ctx->delta.change_pct = 0;
        ctx->delta.delta_size = 0;
        fprintf(stderr, "[kv-remap] cycle: no change, skeleton valid\n");
        return 0;
    }
}


/* =============================================================
 * Status / Destroy
 * ============================================================= */

static inline void kv_remap_print_status(const KVRemapCtx *ctx) {
    static const char *names[] = {"NONE", "ENTROPY", "GEO", "REBUILD"};
    fprintf(stderr, "[kv-remap] skeleton: %s (orig=%zu comp=%zu)\n",
        ctx->skeleton_valid ? "valid" : "invalid",
        ctx->skeleton_orig, ctx->skeleton_comp);
    fprintf(stderr, "[kv-remap] delta: %s pct=%u size=%zu\n",
        names[ctx->delta.type & 3], ctx->delta.change_pct, ctx->delta.delta_size);
    if (ctx->delta.type == DELTA_GEO)
        fprintf(stderr, "[kv-remap] geo ranges: %u\n", ctx->delta.n_ranges);
    fprintf(stderr, "[kv-remap] stats: rebuilds=%u stores=%u restores=%u\n",
        ctx->n_rebuilds, ctx->n_delta_stores, ctx->n_restores);
}

static inline void kv_remap_destroy(KVRemapCtx *ctx) {
    free(ctx->skeleton_data);
    free(ctx->ref_skeleton);
    free(ctx->delta.entropy_data);
    free(ctx->delta.geo_data);
    fprintf(stderr, "[kv-remap] destroyed (rebuilds=%u, stores=%u, restores=%u)\n",
        ctx->n_rebuilds, ctx->n_delta_stores, ctx->n_restores);
    memset(ctx, 0, sizeof(*ctx));
}

#endif /* KV_REMAP_H */
