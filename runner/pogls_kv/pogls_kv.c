#include "pogls_kv.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    uint32_t magic;
    uint32_t orig_size;
    uint32_t comp_size;
    uint32_t n_runs;
} RLEHeader;

typedef struct {
    uint16_t zero_run;
    uint8_t  data_len;
} RLERun;

static int rle_compress(const uint8_t *src, size_t size,
                        void **out, size_t *out_size)
{
    size_t max_out = sizeof(RLEHeader) + size * 3 + 256;
    uint8_t *buf = (uint8_t *)malloc(max_out);
    if (!buf) return -1;

    RLEHeader *hdr = (RLEHeader *)buf;
    uint8_t *dst = buf + sizeof(RLEHeader);
    uint32_t n_runs = 0;
    size_t i = 0;

    while (i < size) {
        uint16_t zero_run = 0;
        while (i < size && src[i] == 0 && zero_run < 65535) {
            zero_run++;
            i++;
        }
        size_t data_start = i;
        uint8_t data_len = 0;
        while (i < size && src[i] != 0 && data_len < 255) {
            data_len++;
            i++;
        }

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
    hdr->magic = POGLS_KV_RLE_MAGIC;
    hdr->orig_size = (uint32_t)size;
    hdr->comp_size = (uint32_t)comp;
    hdr->n_runs = n_runs;

    double ratio = (double)size / (double)(comp > sizeof(RLEHeader) ?
        comp - sizeof(RLEHeader) : 1);
    if (comp >= size || ratio < 1.05) {
        free(buf);
        size_t raw_sz = sizeof(RLEHeader) + size;
        uint8_t *raw = (uint8_t *)malloc(raw_sz);
        if (!raw) return -1;
        RLEHeader *rh = (RLEHeader *)raw;
        rh->magic = POGLS_KV_RLE_MAGIC;
        rh->orig_size = (uint32_t)size;
        rh->comp_size = (uint32_t)raw_sz;
        rh->n_runs = 0;
        memcpy(raw + sizeof(RLEHeader), src, size);
        *out = raw;
        *out_size = raw_sz;
        return 1;
    }

    *out = buf;
    *out_size = comp;
    return 0;
}

static uint8_t *rle_decompress(const void *compressed, size_t comp_size,
                               size_t *out_size)
{
    if (comp_size < sizeof(RLEHeader)) return NULL;
    const RLEHeader *hdr = (const RLEHeader *)compressed;
    if (hdr->magic != POGLS_KV_RLE_MAGIC) return NULL;

    size_t orig = hdr->orig_size;
    if (out_size) *out_size = orig;

    if (hdr->n_runs == 0) {
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
        for (uint16_t z = 0; z < zeros && dst_pos < orig; z++)
            out[dst_pos++] = 0;
        for (uint8_t d = 0; d < dlen && dst_pos < orig; d++)
            out[dst_pos++] = src[d];
        src += dlen;
    }
    return out;
}

static uint32_t build_geo_ranges(const uint8_t *diff, size_t diff_size,
                                 PoglsKvGeoRange *ranges, uint32_t max_ranges)
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
                    n_ranges++;
                }
                in_range = 0;
            }
        }
    }

    if (in_range && n_ranges < max_ranges) {
        ranges[n_ranges].start = range_start;
        ranges[n_ranges].length = (uint32_t)diff_size - range_start;
        n_ranges++;
    }

    return n_ranges;
}

int pogls_kv_skeleton_init(PoglsKvSkeleton *sk, const uint8_t *baseline, size_t n_bytes)
{
    memset(sk, 0, sizeof(*sk));

    sk->data = (uint8_t *)malloc(n_bytes);
    if (!sk->data) return -1;
    memcpy(sk->data, baseline, n_bytes);
    sk->orig_size = n_bytes;

    void *comp = NULL;
    size_t comp_size = 0;
    int r = rle_compress(sk->data, n_bytes, &comp, &comp_size);
    if (r < 0 || !comp) {
        free(sk->data);
        sk->data = NULL;
        return -1;
    }

    sk->compressed = (uint8_t *)comp;
    sk->comp_size = comp_size;
    sk->valid = 1;
    return 0;
}

void pogls_kv_skeleton_destroy(PoglsKvSkeleton *sk)
{
    free(sk->data);
    free(sk->compressed);
    memset(sk, 0, sizeof(*sk));
}

int pogls_kv_classify(const uint8_t *cur, const uint8_t *base, size_t n_bytes)
{
    if (n_bytes == 0) return 0;

    uint64_t diff_bytes = 0;
    size_t off = 0;

    while (off < n_bytes) {
        size_t chunk = n_bytes - off;
        if (chunk > 4096) chunk = 4096;
        if (memcmp(base + off, cur + off, chunk) != 0)
            diff_bytes += chunk;
        off += chunk;
    }

    return (int)(diff_bytes * 100 / n_bytes);
}

int pogls_kv_encode(PoglsKvDelta *delta, const uint8_t *cur,
                    const uint8_t *base, size_t n_bytes)
{
    memset(delta, 0, sizeof(*delta));

    int pct = pogls_kv_classify(cur, base, n_bytes);
    delta->change_pct = (uint16_t)pct;

    if (pct == 0) {
        delta->type = POGLS_KV_REMAP_ENTROPY;
        delta->delta_size = 0;
        return 0;
    }

    if (pct >= POGLS_KV_THRESH_HIGH) {
        delta->type = POGLS_KV_REMAP_REBUILD;
        return -1;
    }

    uint8_t *diff = (uint8_t *)malloc(n_bytes);
    if (!diff) return -1;
    for (size_t i = 0; i < n_bytes; i++)
        diff[i] = cur[i] ^ base[i];

    if (pct <= POGLS_KV_THRESH_LOW) {
        delta->type = POGLS_KV_REMAP_ENTROPY;
        void *comp = NULL;
        size_t comp_size = 0;
        int r = rle_compress(diff, n_bytes, &comp, &comp_size);
        free(diff);
        if (r < 0 || !comp) return -1;
        delta->entropy_data = comp;
        delta->entropy_size = comp_size;
        delta->delta_size = comp_size;
    } else {
        delta->type = POGLS_KV_REMAP_GEO;
        delta->n_ranges = build_geo_ranges(diff, n_bytes, delta->ranges,
                                            POGLS_KV_MAX_GEO_RANGES);

        size_t gd_off = 0, gd_cap = 0;
        uint8_t *gd = NULL;
        for (uint32_t i = 0; i < delta->n_ranges; i++) {
            uint32_t start = delta->ranges[i].start;
            uint32_t len   = delta->ranges[i].length;
            size_t need = gd_off + len;
            if (need > gd_cap) {
                gd_cap = need + 65536;
                uint8_t *ngd = (uint8_t *)realloc(gd, gd_cap);
                if (!ngd) { free(gd); free(diff); return -1; }
                gd = ngd;
            }
            memcpy(gd + gd_off, diff + start, len);
            gd_off += len;
        }
        free(diff);

        delta->geo_data = gd;
        delta->geo_data_size = gd_off;
        delta->delta_size = delta->n_ranges * sizeof(PoglsKvGeoRange) + gd_off;
    }

    return 0;
}

int pogls_kv_decode(uint8_t *out, const PoglsKvDelta *delta,
                    const uint8_t *base, size_t n_bytes)
{
    if (delta->type == POGLS_KV_REMAP_REBUILD)
        return -1;

    memcpy(out, base, n_bytes);

    if (delta->type == POGLS_KV_REMAP_ENTROPY && delta->entropy_data) {
        size_t dec_size = 0;
        uint8_t *dec = rle_decompress(delta->entropy_data, delta->entropy_size, &dec_size);
        if (!dec) return -1;
        size_t apply = dec_size < n_bytes ? dec_size : n_bytes;
        for (size_t i = 0; i < apply; i++)
            out[i] ^= dec[i];
        free(dec);
    } else if (delta->type == POGLS_KV_REMAP_GEO && delta->geo_data && delta->n_ranges > 0) {
        const uint8_t *gd = (const uint8_t *)delta->geo_data;
        size_t gd_off = 0;
        for (uint32_t r = 0; r < delta->n_ranges; r++) {
            uint32_t start = delta->ranges[r].start;
            uint32_t len   = delta->ranges[r].length;
            size_t end = (size_t)start + len;
            if (end > n_bytes) end = n_bytes;
            for (size_t i = start; i < end; i++)
                out[i] ^= gd[gd_off + (i - start)];
            gd_off += end - start;
        }
    }

    return 0;
}

int pogls_kv_rail_init(PoglsKvRail *rail, PoglsKvSkeleton *sk,
                       size_t total_bytes, uint8_t (*get_layer)(size_t off))
{
    (void)get_layer;
    memset(rail, 0, sizeof(*rail));
    rail->skeleton = sk;
    rail->total_bytes = total_bytes;

    if (total_bytes > 0) {
        rail->lane_size = total_bytes / 3;
        if (rail->lane_size == 0) rail->lane_size = 1;
    }

    rail->enabled = 1;
    return 0;
}

int pogls_kv_rail_step(PoglsKvRail *rail)
{
    if (!rail->enabled) return -1;
    if (rail->state == 2) return 0;
    if (rail->state != 1 && !rail->cur) return -1;

    if (rail->state == 0) {
        rail->state = 1;
        rail->change_pct = -1;
        for (int i = 0; i < 3; i++) {
            rail->off[i] = 0;
            rail->diff_count[i] = 0;
            rail->checked[i] = 0;
            rail->complete[i] = 0;
        }
    }

    int all_done = 1;
    int any_work = 0;
    const uint8_t *base = rail->skeleton->data;

    for (int i = 0; i < 3; i++) {
        if (rail->complete[i]) continue;
        all_done = 0;

        size_t start = (size_t)i * rail->lane_size;
        size_t end = (i == 2) ? rail->total_bytes : start + rail->lane_size;
        if (end > rail->total_bytes) end = rail->total_bytes;
        if (end <= start) { rail->complete[i] = 1; continue; }

        size_t lane_bytes = end - start;
        if (rail->off[i] >= lane_bytes) {
            rail->complete[i] = 1;
            continue;
        }

        size_t chunk = lane_bytes - rail->off[i];
        if (chunk > 4096) chunk = 4096;

        if (memcmp(base + start + rail->off[i],
                   rail->cur + start + rail->off[i], chunk) != 0)
            rail->diff_count[i] += chunk;
        rail->checked[i] += chunk;
        rail->off[i] += chunk;

        if (rail->off[i] >= lane_bytes)
            rail->complete[i] = 1;
        any_work = 1;
    }

    if (all_done) {
        uint64_t total_checked = 0, total_diff = 0;
        for (int i = 0; i < 3; i++) {
            total_checked += rail->checked[i];
            total_diff += rail->diff_count[i];
        }
        int pct = total_checked > 0 ? (int)(total_diff * 100 / total_checked) : 0;
        rail->change_pct = pct;
        rail->state = 0;
        return 1;
    }

    return any_work ? 0 : -1;
}

void pogls_kv_rail_freeze(PoglsKvRail *rail)
{
    if (rail->state != 1) return;
    rail->freeze_state = rail->state;
    for (int i = 0; i < 3; i++)
        rail->freeze_off[i] = rail->off[i];
    rail->state = 2;
}

void pogls_kv_rail_resume(PoglsKvRail *rail)
{
    if (rail->state != 2) return;
    rail->state = rail->freeze_state;
    for (int i = 0; i < 3; i++)
        rail->off[i] = rail->freeze_off[i];
}

void pogls_kv_rail_free(PoglsKvRail *rail)
{
    memset(rail, 0, sizeof(*rail));
}
