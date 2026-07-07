#pragma once
#include <stdint.h>
#include <string.h>
#include "geo_temporal_ring.h"
#include "geo_pyramid.h"

#define TSTREAM_DATA_BYTES   4096u
#define TSTREAM_PKT_BYTES    4104u
#define TSTREAM_MAX_PKTS     TEMPORAL_WALK_LEN

typedef struct {
    uint32_t enc;
    uint16_t size;
    uint16_t crc16;
    uint8_t  data[TSTREAM_DATA_BYTES];
} TStreamPkt;

typedef struct {
    uint8_t  data[TSTREAM_DATA_BYTES];
    uint16_t size;
    uint8_t  _pad[6];
} TStreamChunk;

static inline uint16_t _tstream_crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFFu;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)buf[i] << 8);
        for (uint8_t b = 0; b < 8u; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
    }
    return crc;
}

static inline uint16_t tstream_slice_file(TStreamPkt       out_pkts[TSTREAM_MAX_PKTS],
                                           const uint8_t   *file,
                                           uint32_t         fsize)
{
    if (fsize == 0u) return 0u;
    uint16_t n = (uint16_t)((fsize + TSTREAM_DATA_BYTES - 1u) / TSTREAM_DATA_BYTES);
    if (n > TSTREAM_MAX_PKTS) return 0u;
    for (uint16_t i = 0u; i < n; i++) {
        TStreamPkt *p = &out_pkts[i];
        uint32_t off  = (uint32_t)i * TSTREAM_DATA_BYTES;
        uint16_t sz   = (uint16_t)((off + TSTREAM_DATA_BYTES <= fsize)
                                    ? TSTREAM_DATA_BYTES
                                    : fsize - off);
        p->enc  = GEO_WALK[i];
        p->size = sz;
        memset(p->data, 0, TSTREAM_DATA_BYTES);
        memcpy(p->data, file + off, sz);
        p->crc16 = _tstream_crc16(p->data, sz);
    }
    return n;
}

static inline int tstream_recv_pkt(TRingCtx        *r,
                                    TStreamChunk    store[TSTREAM_MAX_PKTS],
                                    const TStreamPkt *pkt)
{
    uint16_t pos = tring_pos(pkt->enc);
    if (pos == 0xFFFFu) return -1;
    uint16_t crc = _tstream_crc16(pkt->data, pkt->size);
    if (crc != pkt->crc16) return -2;
    memcpy(store[pos].data, pkt->data, pkt->size);
    store[pos].size = pkt->size;
    return tring_snap(r, pkt->enc);
}

static inline uint32_t tstream_reconstruct(const TRingCtx      *r,
                                            const TStreamChunk   store[TSTREAM_MAX_PKTS],
                                            uint16_t             n_pkts,
                                            uint8_t             *out)
{
    uint32_t written = 0u;
    for (uint16_t i = 0u; i < n_pkts; i++) {
        if (r->slots[i].present) {
            uint16_t sz = store[i].size;
            memcpy(out + written, store[i].data, sz);
            written += sz;
        } else {
            memset(out + written, 0, TSTREAM_DATA_BYTES);
            written += TSTREAM_DATA_BYTES;
        }
    }
    return written;
}

static inline uint32_t tstream_reconstruct_exact(const TRingCtx      *r,
                                                   const TStreamChunk   store[TSTREAM_MAX_PKTS],
                                                   uint16_t             n_pkts,
                                                   uint8_t             *out)
{
    uint32_t written = 0u;
    for (uint16_t i = 0u; i < n_pkts; i++) {
        if (!r->slots[i].present) continue;
        uint16_t sz = store[i].size;
        memcpy(out + written, store[i].data, sz);
        written += sz;
    }
    return written;
}

static inline int tstream_phase_ready(const TRingCtx *r, uint8_t level)
{
    uint16_t start = pyr_phase_start(level);
    uint16_t end   = start + GEO_PYR_PHASE_LEN;
    for (uint16_t i = start; i < end; i++)
        if (!r->slots[i].present) return 0;
    return 1;
}

static inline uint16_t tstream_gap_report(const TRingCtx *r, uint16_t n_pkts)
{
    uint16_t gaps = 0u;
    for (uint16_t i = 0u; i < n_pkts; i++)
        if (!r->slots[i].present) gaps++;
    return gaps;
}
