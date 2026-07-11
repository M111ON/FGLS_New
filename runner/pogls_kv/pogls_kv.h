#ifndef POGLS_KV_H
#define POGLS_KV_H

#include <stdint.h>
#include <stddef.h>

#define POGLS_KV_REMAP_ENTROPY  0
#define POGLS_KV_REMAP_GEO      1
#define POGLS_KV_REMAP_REBUILD  2

#define POGLS_KV_THRESH_LOW     15
#define POGLS_KV_THRESH_HIGH    85
#define POGLS_KV_MAX_GEO_RANGES 4096
#define POGLS_KV_RLE_MAGIC      0x524C4531

typedef struct {
    uint32_t start;
    uint32_t length;
} PoglsKvGeoRange;

typedef struct {
    uint8_t  *data;
    uint8_t  *compressed;
    size_t    comp_size;
    size_t    orig_size;
    int       valid;
} PoglsKvSkeleton;

typedef struct {
    uint8_t  type;
    uint16_t change_pct;
    void    *entropy_data;
    size_t   entropy_size;
    PoglsKvGeoRange ranges[POGLS_KV_MAX_GEO_RANGES];
    uint32_t n_ranges;
    void    *geo_data;
    size_t   geo_data_size;
    size_t   delta_size;
} PoglsKvDelta;

typedef struct {
    PoglsKvSkeleton *skeleton;
    const uint8_t   *cur;
    size_t           total_bytes;
    int              state;
    size_t           lane_size;
    size_t           off[3];
    uint64_t         diff_count[3];
    uint64_t         checked[3];
    int              complete[3];
    int              freeze_state;
    size_t           freeze_off[3];
    int              change_pct;
    int              enabled;
} PoglsKvRail;

int  pogls_kv_skeleton_init(PoglsKvSkeleton *sk, const uint8_t *baseline, size_t n_bytes);
void pogls_kv_skeleton_destroy(PoglsKvSkeleton *sk);
int  pogls_kv_classify(const uint8_t *cur, const uint8_t *base, size_t n_bytes);
int  pogls_kv_encode(PoglsKvDelta *delta, const uint8_t *cur,
                     const uint8_t *base, size_t n_bytes);
int  pogls_kv_decode(uint8_t *out, const PoglsKvDelta *delta,
                     const uint8_t *base, size_t n_bytes);
int  pogls_kv_rail_init(PoglsKvRail *rail, PoglsKvSkeleton *sk,
                        size_t total_bytes, uint8_t (*get_layer)(size_t off));
int  pogls_kv_rail_step(PoglsKvRail *rail);
void pogls_kv_rail_free(PoglsKvRail *rail);
void pogls_kv_rail_freeze(PoglsKvRail *rail);
void pogls_kv_rail_resume(PoglsKvRail *rail);

#endif
