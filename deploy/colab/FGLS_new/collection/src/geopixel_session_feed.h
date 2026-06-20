/*
 * geopixel_session_feed.h
 * File -> Scan session -> GPX4 META container bridge
 */
#ifndef GEOPIXEL_SESSION_FEED_H
#define GEOPIXEL_SESSION_FEED_H

#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>

#ifndef GEOPIXEL_SESSION_ENABLE_ANIM_CODEC
#define GEOPIXEL_SESSION_ENABLE_ANIM_CODEC 0
#endif

#if __has_include("../geopixel/geopixel/geo_gpx_anim_o23.h")
#include "../geopixel/geopixel/geo_gpx_anim_o23.h"
#elif __has_include("../geopixel/geopixel/gpx4_container_o22.h")
#include "../geopixel/geopixel/gpx4_container_o22.h"
#else
#error "geopixel_session_feed.h: missing GeoPixel container headers"
#endif

typedef struct {
    uint64_t file_bytes;
    uint32_t total_chunks;
    uint32_t emitted_chunks;
    uint32_t passthru_chunks;
    uint32_t tail_chunks;
    uint64_t seed_xor;
} GeopixelSessionMetrics;

int geopixel_session_scan_buf(const uint8_t *buf,
                              size_t len,
                              GeopixelSessionMetrics *out_metrics);

int geopixel_session_scan_file(const char *path,
                               GeopixelSessionMetrics *out_metrics);

int geopixel_session_write_meta_gpx4(const char *out_path,
                                     const GeopixelSessionMetrics *metrics,
                                     uint16_t tw,
                                     uint16_t th);

int geopixel_timeline_write_sequence_gpx4(const char *out_path,
                                          const char *const *frame_paths,
                                          uint16_t n_frames,
                                          uint16_t width_px,
                                          uint16_t height_px,
                                          uint16_t fps_num,
                                          uint16_t fps_den,
                                          uint16_t keyframe_interval);

typedef enum {
    GPX_TIMELINE_AUTO = 0,
    GPX_TIMELINE_META = 1,
    GPX_TIMELINE_ANIM_CODEC = 2
} GpxTimelineMode;

int geopixel_timeline_write_sequence_gpx4_ex(const char *out_path,
                                             const char *const *frame_paths,
                                             uint16_t n_frames,
                                             uint16_t width_px,
                                             uint16_t height_px,
                                             uint16_t fps_num,
                                             uint16_t fps_den,
                                             uint16_t keyframe_interval,
                                             GpxTimelineMode mode);

#ifdef GEOPIXEL_SESSION_FEED_IMPL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* same pre-compressed magics used by the scanner path */
#define _GPXSF_MAGIC_ZIP   0x04034B50u
#define _GPXSF_MAGIC_GZIP  0x00088B1Fu
#define _GPXSF_MAGIC_ZSTD  0xFD2FB528u
#define _GPXSF_MAGIC_PNG   0x474E5089u
#define _GPXSF_MAGIC_JPEG  0xFFD8FFE0u
#define _GPXSF_MAGIC_JFIF  0xFFD8FFE1u
#define _GPXSF_MAGIC_MP4A  0x70797466u
#define _GPXSF_MAGIC_WEBP  0x46464952u

static int _gpxsf_is_passthru_magic(const uint8_t *chunk64)
{
    uint32_t magic = 0u;
    memcpy(&magic, chunk64, 4);
    return (magic == _GPXSF_MAGIC_ZIP  ||
            magic == _GPXSF_MAGIC_GZIP ||
            magic == _GPXSF_MAGIC_ZSTD ||
            magic == _GPXSF_MAGIC_PNG  ||
            magic == _GPXSF_MAGIC_JPEG ||
            magic == _GPXSF_MAGIC_JFIF ||
            magic == _GPXSF_MAGIC_MP4A ||
            magic == _GPXSF_MAGIC_WEBP);
}

static uint64_t _gpxsf_chunk_seed(const uint8_t *chunk64)
{
    uint64_t w[8];
    uint64_t s;
    memcpy(w, chunk64, 64);
    s = w[0] ^ w[1] ^ w[2] ^ w[3] ^ w[4] ^ w[5] ^ w[6] ^ w[7];
    s ^= s >> 33;
    s *= UINT64_C(0xff51afd7ed558ccd);
    s ^= s >> 33;
    s *= UINT64_C(0xc4ceb9fe1a85ec53);
    s ^= s >> 33;
    return s;
}

int geopixel_session_scan_buf(const uint8_t *buf,
                              size_t len,
                              GeopixelSessionMetrics *out_metrics)
{
    uint8_t first64[64];
    int file_passthru;
    uint32_t i;

    if (!buf || !out_metrics) return -1;
    memset(out_metrics, 0, sizeof(*out_metrics));
    out_metrics->file_bytes = (uint64_t)len;
    out_metrics->total_chunks = (uint32_t)((len + 63u) / 64u);

    if (len == 0u) return 0;

    memset(first64, 0, sizeof(first64));
    memcpy(first64, buf, len < 64u ? len : 64u);
    file_passthru = _gpxsf_is_passthru_magic(first64);

    for (i = 0u; i < out_metrics->total_chunks; i++) {
        uint8_t chunk64[64];
        size_t off = (size_t)i * 64u;
        size_t remain = len - off;
        size_t copy_n = remain < 64u ? remain : 64u;
        memset(chunk64, 0, sizeof(chunk64));
        memcpy(chunk64, buf + off, copy_n);
        out_metrics->emitted_chunks++;
        out_metrics->seed_xor ^= _gpxsf_chunk_seed(chunk64);
        if (file_passthru) out_metrics->passthru_chunks++;
        if (copy_n < 64u) out_metrics->tail_chunks++;
    }
    return 0;
}

int geopixel_session_scan_file(const char *path,
                               GeopixelSessionMetrics *out_metrics)
{
    FILE *f;
    long sz;
    uint8_t *buf;
    size_t got;
    int r;

    if (!path || !out_metrics) return -1;
    f = fopen(path, "rb");
    if (!f) return -2;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -3; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return -4; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -5; }

    buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return -6; }
    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return -7; }

    r = geopixel_session_scan_buf(buf, got, out_metrics);
    free(buf);
    return r;
}

int geopixel_session_write_meta_gpx4(const char *out_path,
                                     const GeopixelSessionMetrics *metrics,
                                     uint16_t tw,
                                     uint16_t th)
{
    char payload[256];
    int n;
    Gpx4LayerDef layer;

    if (!out_path || !metrics || tw == 0 || th == 0) return -1;

    n = snprintf(payload, sizeof(payload),
        "session_bytes=%" PRIu64 "\nchunks_total=%u\nchunks_emitted=%u\npassthru=%u\ntail=%u\nseed_xor=0x%016" PRIx64 "\n",
        metrics->file_bytes,
        metrics->total_chunks,
        metrics->emitted_chunks,
        metrics->passthru_chunks,
        metrics->tail_chunks,
        metrics->seed_xor);
    if (n <= 0) return -2;
    if ((size_t)n >= sizeof(payload)) return -3;

    memset(&layer, 0, sizeof(layer));
    layer.type = GPX4_LAYER_META;
    layer.lflags = 0u;
    layer.name[0] = 'S';
    layer.name[1] = 'E';
    layer.name[2] = 'S';
    layer.name[3] = 'S';
    layer.data = (uint8_t *)payload;
    layer.size = (uint32_t)n;
    layer.tiles = NULL;

    return gpx4_write(out_path, tw, th, &layer, 1);
}

static void _gpxsf_frame_name(char out4[4], uint16_t idx)
{
    out4[0] = 'F';
    out4[1] = (char)('0' + ((idx / 100u) % 10u));
    out4[2] = (char)('0' + ((idx / 10u) % 10u));
    out4[3] = (char)('0' + (idx % 10u));
}

int geopixel_timeline_write_sequence_gpx4(const char *out_path,
                                          const char *const *frame_paths,
                                          uint16_t n_frames,
                                          uint16_t width_px,
                                          uint16_t height_px,
                                          uint16_t fps_num,
                                          uint16_t fps_den,
                                          uint16_t keyframe_interval)
{
    return geopixel_timeline_write_sequence_gpx4_ex(
        out_path, frame_paths, n_frames, width_px, height_px,
        fps_num, fps_den, keyframe_interval, GPX_TIMELINE_META
    );
}

int geopixel_timeline_write_sequence_gpx4_ex(const char *out_path,
                                             const char *const *frame_paths,
                                             uint16_t n_frames,
                                             uint16_t width_px,
                                             uint16_t height_px,
                                             uint16_t fps_num,
                                             uint16_t fps_den,
                                             uint16_t keyframe_interval,
                                             GpxTimelineMode mode)
{
    Gpx4LayerDef *layers = NULL;
    uint8_t **frame_bufs = NULL;
    uint32_t *frame_sizes = NULL;
    uint32_t i;
    int rc = -1;
    size_t expected_frame_bytes = (size_t)width_px * (size_t)height_px * 3u;
    int use_anim_codec = 0;

    if (!out_path || !frame_paths || n_frames == 0u) return -1;
    if (fps_num == 0u || fps_den == 0u) return -2;

    if (mode == GPX_TIMELINE_ANIM_CODEC) use_anim_codec = 1;

    layers = (Gpx4LayerDef *)calloc((size_t)n_frames + 1u, sizeof(Gpx4LayerDef));
    frame_bufs = (uint8_t **)calloc(n_frames, sizeof(uint8_t *));
    frame_sizes = (uint32_t *)calloc(n_frames, sizeof(uint32_t));
    if (!layers || !frame_bufs || !frame_sizes) goto cleanup;

    for (i = 0u; i < n_frames; i++) {
        FILE *f = fopen(frame_paths[i], "rb");
        long sz;
        uint8_t *buf;
        size_t got;
        if (!f) goto cleanup;
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); goto cleanup; }
        sz = ftell(f);
        if (sz < 0) { fclose(f); goto cleanup; }
        if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); goto cleanup; }
        buf = (uint8_t *)malloc((size_t)sz);
        if (!buf) { fclose(f); goto cleanup; }
        got = fread(buf, 1, (size_t)sz, f);
        fclose(f);
        if (got != (size_t)sz) { free(buf); goto cleanup; }

        frame_bufs[i] = buf;
        frame_sizes[i] = (uint32_t)sz;
        if (mode == GPX_TIMELINE_AUTO && (size_t)sz != expected_frame_bytes)
            use_anim_codec = 0;
        if (mode == GPX_TIMELINE_AUTO && i == 0u && (size_t)sz == expected_frame_bytes)
            use_anim_codec = 1;

        if (mode == GPX_TIMELINE_ANIM_CODEC && (size_t)sz != expected_frame_bytes) {
            rc = -8;
            goto cleanup;
        }
    }

    if (use_anim_codec) {
#if GEOPIXEL_SESSION_ENABLE_ANIM_CODEC
        GpxAnimEncCfg cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.fps_num = fps_num;
        cfg.fps_den = fps_den;
        cfg.keyframe_interval = keyframe_interval;
        cfg.zstd_level = 9;
        cfg.tile_sz = 32;
        rc = gpx_anim_encode(frame_bufs, n_frames, width_px, height_px, &cfg, out_path);
        goto cleanup;
#else
        if (mode == GPX_TIMELINE_ANIM_CODEC) {
            rc = -9;
            goto cleanup;
        }
        use_anim_codec = 0;
#endif /* GEOPIXEL_SESSION_ENABLE_ANIM_CODEC */
    }

    /* fallback / explicit META timeline path */
    {
        Gpx4AnimHdr *ah = (Gpx4AnimHdr *)malloc(sizeof(Gpx4AnimHdr));
        if (!ah) goto cleanup;
        memset(ah, 0, sizeof(*ah));
        ah->n_frames = n_frames;
        ah->fps_num = fps_num;
        ah->fps_den = fps_den;
        ah->keyframe_interval = keyframe_interval;
        ah->width_px = width_px;
        ah->height_px = height_px;
        layers[0].type = GPX4_LAYER_ANIM_HDR;
        layers[0].lflags = 0u;
        layers[0].name[0] = 'A';
        layers[0].name[1] = 'H';
        layers[0].name[2] = 'D';
        layers[0].name[3] = 'R';
        layers[0].size = GPX4_ANIM_HDR_SZ;
        layers[0].data = (uint8_t *)ah;
        layers[0].tiles = NULL;
    }

    for (i = 0u; i < n_frames; i++) {
        layers[i + 1u].type = GPX4_LAYER_META;
        layers[i + 1u].lflags = 0u;
        _gpxsf_frame_name(layers[i + 1u].name, (uint16_t)i);
        layers[i + 1u].data = frame_bufs[i];
        layers[i + 1u].size = frame_sizes[i];
        layers[i + 1u].tiles = NULL;
    }
    rc = gpx4_write(out_path, 1u, 1u, layers, (int)n_frames + 1);

cleanup:
    if (layers) {
        free(layers[0].data); /* AHDR */
    }
    for (i = 0u; i < n_frames; i++) free(frame_bufs ? frame_bufs[i] : NULL);
    free(frame_bufs);
    free(frame_sizes);
    free(layers);
    return rc;
}
#endif /* GEOPIXEL_SESSION_FEED_IMPL */

#endif /* GEOPIXEL_SESSION_FEED_H */
