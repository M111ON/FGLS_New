#ifndef SID_CACHE_H
#define SID_CACHE_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define SID_CACHE_MAX_ENTRIES  256
#define SID_CACHE_NAME_MAX     128
#define SID_TRING_SLOTS        1440

/* Lightweight TRing rewind store — O(1) lookup by TRing position.
 * Two-level: TWFaceRewindSid uses packed_key w/ bit-49 valid marker. */
#define SID_FACE_BITS      4
#define SID_ZONE_BITS      4
#define SID_SLOT_BITS      4
#define SID_DRAIN_BITS     1
#define SID_TRI_BITS       1
#define SID_VALID_BIT      49u

#define SID_PACK_KEY(face,zone,slot,is_tri,resid_x,resid_y) \
    ((uint64_t)1 << SID_VALID_BIT) | \
    ((uint64_t)((face)&0xF) << 60) | \
    ((uint64_t)((zone)&0xF) << 56) | \
    ((uint64_t)((slot)&0xF) << 52) | \
    ((uint64_t)((is_tri)&1) << 51) | \
    ((uint64_t)((resid_x)&0xFFFF) << 32) | \
    ((uint64_t)((resid_y)&0xFFFF) << 16)

typedef struct {
    uint64_t keys[SID_TRING_SLOTS];  /* packed keys, 0 = empty */
    uint32_t stored;
} TWFaceRewindSid;

static inline void tw_rewind_sid_init(TWFaceRewindSid *rb) {
    memset(rb, 0, sizeof(*rb));
}

static inline void tw_rewind_sid_store(TWFaceRewindSid *rb, uint64_t key, uint16_t tring) {
    rb->keys[tring % SID_TRING_SLOTS] = key;
    rb->stored++;
}

static inline uint64_t tw_rewind_sid_find(const TWFaceRewindSid *rb, uint16_t tring) {
    return rb->keys[tring % SID_TRING_SLOTS];
}

static inline int tw_rewind_sid_has(const TWFaceRewindSid *rb, uint16_t tring) {
    return (rb->keys[tring % SID_TRING_SLOTS] >> SID_VALID_BIT) & 1;
}

static inline void tw_rewind_sid_evict(TWFaceRewindSid *rb, uint16_t tring) {
    rb->keys[tring % SID_TRING_SLOTS] = 0;
}

typedef struct {
    char     name[SID_CACHE_NAME_MAX];
    uint16_t tring_pos;
    uint8_t *data;
    size_t   size;
    uint32_t hits;
} SIDCacheEntry;

typedef struct {
    SIDCacheEntry    entries[SID_CACHE_MAX_ENTRIES];
    TWFaceRewindSid  rewind;            /* TRing O(1) index */
    uint32_t         n_entries;
    uint64_t         pool_size;
    uint64_t         pool_used;
    uint64_t         hits;
    uint64_t         misses;
    uint64_t         evictions;
    uint32_t         next_evict;
} SIDCache;

static void sid_cache_init(SIDCache *c, uint64_t pool_bytes) {
    memset(c,0,sizeof(*c)); c->pool_size=pool_bytes;
    for (int i=0;i<SID_CACHE_MAX_ENTRIES;i++) c->entries[i].tring_pos=0xFFFF;
    tw_rewind_sid_init(&c->rewind);
}

static void sid_cache_clear(SIDCache *c) {
    for (uint32_t i=0;i<SID_CACHE_MAX_ENTRIES;i++) {
        free(c->entries[i].data); c->entries[i].data=NULL;
        c->entries[i].tring_pos=0xFFFF; c->entries[i].size=0;
    }
    c->n_entries=0; c->pool_used=0; tw_rewind_sid_init(&c->rewind);
}

static int sid_cache_get(SIDCache *c, const char *name, uint8_t **data, size_t *size) {
    for (uint32_t i=0;i<SID_CACHE_MAX_ENTRIES;i++) {
        if (c->entries[i].tring_pos!=0xFFFF&&strcmp(c->entries[i].name,name)==0) {
            *data=c->entries[i].data; *size=c->entries[i].size;
            c->entries[i].hits++; c->hits++; return 0;
        }
    }
    c->misses++; return -1;
}

static int sid_cache_get_by_tring(SIDCache *c, uint16_t tring, uint8_t **data, size_t *size) {
    for (uint32_t i=0;i<SID_CACHE_MAX_ENTRIES;i++) {
        if (c->entries[i].tring_pos==tring) {
            *data=c->entries[i].data; *size=c->entries[i].size;
            c->entries[i].hits++; c->hits++; return 0;
        }
    }
    c->misses++; return -1;
}

static int sid_cache_put(SIDCache *c, const char *name, uint16_t tring, const uint8_t *data, size_t sz) {
    if (sz>c->pool_size) return -1;
    for (uint32_t i=0;i<SID_CACHE_MAX_ENTRIES;i++) {
        if (c->entries[i].tring_pos!=0xFFFF&&strcmp(c->entries[i].name,name)==0) {
            if (c->entries[i].size!=sz) {
                uint8_t *nd=(uint8_t*)realloc(c->entries[i].data,sz);
                if(!nd) return -1; c->entries[i].data=nd;
                c->pool_used-=c->entries[i].size; c->pool_used+=sz;
            }
            memcpy(c->entries[i].data,data,sz);
            c->entries[i].size=sz; c->entries[i].tring_pos=tring; return 1;
        }
    }
    if (c->pool_used+sz>c->pool_size) {
        for (uint32_t t=0;t<SID_CACHE_MAX_ENTRIES;t++) {
            uint32_t ei=(c->next_evict+t)%SID_CACHE_MAX_ENTRIES;
            if (c->entries[ei].tring_pos!=0xFFFF) {
                tw_rewind_sid_evict(&c->rewind, c->entries[ei].tring_pos);
                c->pool_used-=c->entries[ei].size; free(c->entries[ei].data);
                c->entries[ei].data=NULL; c->entries[ei].tring_pos=0xFFFF; c->entries[ei].size=0;
                c->evictions++; break;
            }
        }
    }
    uint32_t slot=SID_CACHE_MAX_ENTRIES;
    for (uint32_t i=0;i<SID_CACHE_MAX_ENTRIES;i++)
        { if(c->entries[i].tring_pos==0xFFFF){slot=i;break;} }
    if(slot>=SID_CACHE_MAX_ENTRIES) return -1;
    c->entries[slot].data=(uint8_t*)malloc(sz);
    if(!c->entries[slot].data) return -1;
    memcpy(c->entries[slot].data,data,sz);
    strncpy(c->entries[slot].name,name,SID_CACHE_NAME_MAX-1);
    c->entries[slot].tring_pos=tring; c->entries[slot].size=sz; c->entries[slot].hits=0;
    c->pool_used+=sz; c->n_entries++; c->next_evict=(slot+1)%SID_CACHE_MAX_ENTRIES;
    uint64_t pk = SID_PACK_KEY(tring/120, (tring%60)/6, (tring%60)%6, (tring/60)&1, 0, 0);
    tw_rewind_sid_store(&c->rewind, pk, tring);
    return 0;
}

static inline int sid_cache_has_tring(const SIDCache *c, uint16_t tring) {
    return tw_rewind_sid_has((TWFaceRewindSid*)&c->rewind, tring);
}

static int sid_cache_evict(SIDCache *c, uint16_t tring) {
    tw_rewind_sid_evict(&c->rewind, tring);
    for (uint32_t i=0;i<SID_CACHE_MAX_ENTRIES;i++) {
        if(c->entries[i].tring_pos==tring) {
            c->pool_used-=c->entries[i].size; free(c->entries[i].data);
            c->entries[i].data=NULL; c->entries[i].tring_pos=0xFFFF; c->entries[i].size=0;
            c->evictions++; return 0;
        }
    }
    return -1;
}

#endif
