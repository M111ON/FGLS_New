#ifndef SID_CACHE_H
#define SID_CACHE_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define SID_CACHE_MAX_ENTRIES  256
#define SID_CACHE_NAME_MAX     128

typedef struct {
    char     name[SID_CACHE_NAME_MAX];
    uint16_t tring_pos;
    uint8_t *data;
    size_t   size;
    uint32_t hits;
} SIDCacheEntry;

typedef struct {
    SIDCacheEntry entries[SID_CACHE_MAX_ENTRIES];
    uint32_t      n_entries;
    uint64_t      pool_size;
    uint64_t      pool_used;
    uint64_t      hits;
    uint64_t      misses;
    uint64_t      evictions;
    uint32_t      next_evict;
} SIDCache;

static void sid_cache_init(SIDCache *c, uint64_t pool_bytes) {
    memset(c,0,sizeof(*c)); c->pool_size=pool_bytes;
    for (int i=0;i<SID_CACHE_MAX_ENTRIES;i++) c->entries[i].tring_pos=0xFFFF;
}

static void sid_cache_clear(SIDCache *c) {
    for (uint32_t i=0;i<SID_CACHE_MAX_ENTRIES;i++) {
        free(c->entries[i].data); c->entries[i].data=NULL;
        c->entries[i].tring_pos=0xFFFF; c->entries[i].size=0;
    }
    c->n_entries=0; c->pool_used=0;
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
    return 0;
}

static int sid_cache_evict(SIDCache *c, uint16_t tring) {
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
