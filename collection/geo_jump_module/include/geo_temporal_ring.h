#pragma once
#include <stdint.h>
#include <string.h>
#include "geo_temporal_lut.h"

typedef struct {
    uint32_t enc;
    uint32_t chunk_id;
    uint8_t  present;
    uint8_t  _pad[3];
} TRingSlot;

typedef struct {
    TRingSlot slots[TEMPORAL_WALK_LEN];
    uint16_t  head;
    uint16_t  missing;
    uint32_t  chunk_count;
} TRingCtx;

static inline void tring_init(TRingCtx *r){
    memset(r,0,sizeof(TRingCtx));
    for(int i=0;i<TEMPORAL_WALK_LEN;i++)
        r->slots[i].enc=GEO_WALK[i];
}

static inline void tring_assign(TRingCtx *r, uint16_t pos, uint32_t chunk_id){
    r->slots[pos].chunk_id=chunk_id;
    r->slots[pos].present=1;
    r->chunk_count++;
}

static inline uint16_t tring_tick(TRingCtx *r){
    r->head=(uint16_t)((r->head+1)%TEMPORAL_WALK_LEN);
    return r->head;
}

static inline uint16_t tring_pos(uint32_t enc){
    return GEO_WALK_IDX[enc & 0x7FFu];
}

static inline uint16_t tring_pair_pos(uint32_t enc){
    return GEO_WALK_IDX[TRING_CPAIR(enc) & 0x7FFu];
}

static inline uint16_t tring_first_gap(const TRingCtx *r){
    for(uint16_t i=0;i<TEMPORAL_WALK_LEN;i++){
        uint16_t pos=(uint16_t)((r->head+i)%TEMPORAL_WALK_LEN);
        if(!r->slots[pos].present) return pos;
    }
    return 0xFFFF;
}

static inline int tring_snap(TRingCtx *r, uint32_t enc){
    uint16_t pos=tring_pos(enc);
    if(pos==0xFFFF) return -1;
    r->slots[pos].present=1;
    int gap=(int)pos-(int)r->head;
    if(gap<0) gap+=TEMPORAL_WALK_LEN;
    r->head=pos;
    r->missing+=(uint16_t)(gap>0?gap:0);
    return gap;
}

static inline uint16_t tring_gap_count(const TRingCtx *r, uint16_t to_pos){
    uint16_t cnt=0;
    for(uint16_t i=0;i<to_pos;i++)
        if(!r->slots[i].present) cnt++;
    return cnt;
}

static inline int tring_is_valid_next(const TRingCtx *r, uint32_t enc){
    uint16_t expected=(uint16_t)((r->head+1)%TEMPORAL_WALK_LEN);
    uint16_t pos=tring_pos(enc);
    return (pos!=0xFFFFu) & (pos==expected);
}

static inline int tring_verify_next(TRingCtx *r, uint32_t enc){
    uint16_t pos=tring_pos(enc);
    if(pos==0xFFFFu) return -1;
    uint16_t expected=(uint16_t)((r->head+1)%TEMPORAL_WALK_LEN);
    if(pos!=expected) return 0;
    r->slots[pos].present=1;
    r->head=pos;
    return 1;
}
