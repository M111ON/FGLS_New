/*
 * tensor_track_v1.h — V1: Old approach (enc + enclosure + manual routing)
 * ═══════════════════════════════════════════════════════════════════════
 * V1 แยก index (frame_at) กับ container (enc_find_home + enc_chunk_idx)
 * ผลลัพธ์: ซ้ำซ้อน — enc อยู่แล้ว แต่เดิน RDH ซ้ำ
 * ═══════════════════════════════════════════════════════════════════════
 */

#ifndef TENSOR_TRACK_V1_H
#define TENSOR_TRACK_V1_H

#include <stdint.h>
#include <string.h>
#include <stddef.h>

#include "geo_frame_seek.h"
#include "rdh_capture.h"
#include "fibo_tick.h"
#include "gls_enclosure.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TTV1_RING_SIZE 256u

/* V1 record: เก็บทุกอย่าง (home_x, home_y, chunk_idx, face, slot...) */
typedef struct {
    uint32_t  chunk_id;
    uint32_t  data_len;
    int64_t   flat_key;
    uint16_t  enc;
    uint8_t   face;
    uint8_t   slot;
    uint8_t   ico_idx;
    uint8_t   phase;
    uint16_t  home_x;        /* ← ซ้ำ — enc ให้ field position อยู่แล้ว */
    uint16_t  home_y;        /* ← ซ้ำ */
    uint32_t  chunk_idx;     /* ← ซ้ำ — enc_chunk_idx = f(enc) */
    uint8_t   entropy_score;
    uint8_t   entropy_class;
    uint8_t   tick;
    uint8_t   store_action;  /* ← ซ้ำ — ft_store_action(enc) อยู่แล้ว */
} TTV1ChunkRecord;

/* 32 bytes per record (vs V2: 10 bytes) */

typedef struct {
    uint32_t  total_chunks;
    uint32_t  total_bytes;
    uint32_t  entropy_count[4];
    uint8_t   entropy_avg;
    uint32_t  action_count[4];
    uint32_t  enc_coverage;
} TTV1Stats;

typedef struct {
    TTV1ChunkRecord ring[TTV1_RING_SIZE];
    uint32_t  ring_head;
    uint32_t  ring_count;
    TTV1Stats stats;
    uint16_t  enc_seen[(FRAME_CYCLE + 15) / 16];
} TTV1Context;

static inline void ttv1_init(TTV1Context *ctx) {
    memset(ctx, 0, sizeof(TTV1Context));
}

static inline int ttv1_ingest(TTV1Context *ctx,
                               const uint8_t *data, uint32_t data_len,
                               TTV1ChunkRecord *rec_out)
{
    if (!ctx || !data || data_len == 0 || !rec_out) return -1;

    uint32_t cid = ctx->ring_count;

    /* 1. rdh_capture → flat_key → enc */
    int64_t flat_key = rdh_capture(data, (size_t)data_len, &RDH_CAPTURE_144);
    uint16_t enc = (uint16_t)((uint64_t)flat_key % FRAME_CYCLE);

    /* 2. frame_at(enc) → face, slot, ico_idx, phase */
    DualFrame frame = frame_at(enc);

    /* 3. enc_find_home — เดิน RDH ซ้ำ (REDUNDANT — enc มาจาก RDH อยู่แล้ว) */
    uint32_t home_x = 0, home_y = 0;
    enc_find_home(data, data_len, 144, &home_x, &home_y);

    /* 4. enc_chunk_idx — คำนวณใหม่ (REDUNDANT — ft_enc_to_field ให้ field pos) */
    EncConfig cfg = enc_config(4);
    uint32_t cidx = enc_chunk_idx(home_x, home_y, cfg.field_dim, cfg.scale);

    /* 5. ft_store_action — เรียกซ้ำ (REDUNDANT) */
    uint8_t action = ft_store_action(enc);
    uint8_t tick   = ft_enc_to_tick(enc);

    /* 6. entropy — OK (สิ่งเดียวที่ enc ไม่รู้) */
    uint8_t ent = tt_entropy_score(data, data_len);
    uint8_t cls = tt_entropy_class(ent);

    /* Fill record: 32 bytes (เก็บทุกอย่าง) */
    rec_out->chunk_id      = cid;
    rec_out->data_len      = data_len;
    rec_out->flat_key      = flat_key;
    rec_out->enc           = enc;
    rec_out->face          = frame.face;
    rec_out->slot          = frame.slot;
    rec_out->ico_idx       = frame.ico_idx;
    rec_out->phase         = frame.phase;
    rec_out->home_x        = (uint16_t)home_x;
    rec_out->home_y        = (uint16_t)home_y;
    rec_out->chunk_idx     = cidx;
    rec_out->entropy_score = ent;
    rec_out->entropy_class = cls;
    rec_out->tick          = tick;
    rec_out->store_action  = action;

    /* ring buffer */
    ctx->ring[ctx->ring_head] = *rec_out;
    ctx->ring_head = (ctx->ring_head + 1) % TTV1_RING_SIZE;
    ctx->ring_count = cid + 1;

    /* stats */
    TTV1Stats *s = &ctx->stats;
    s->total_chunks++;
    s->total_bytes += data_len;
    s->entropy_count[cls]++;
    s->action_count[action]++;
    s->entropy_avg = (uint8_t)(
        ((uint32_t)s->entropy_avg * (s->total_chunks - 1) + ent)
        / s->total_chunks
    );

    uint32_t w = enc / 16, b = enc % 16;
    if (!(ctx->enc_seen[w] & (1u << b))) {
        ctx->enc_seen[w] |= (uint16_t)(1u << b);
        s->enc_coverage++;
    }

    return (int)cid;
}

#ifdef __cplusplus
}
#endif

#endif /* TENSOR_TRACK_V1_H */
