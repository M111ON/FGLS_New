#ifndef FGLS_TWIN_STORE_H
#define FGLS_TWIN_STORE_H

#include <stdint.h>
#include <string.h>

typedef struct {
    uint64_t merkle_root;
    uint64_t sha256_hi;
    uint64_t sha256_lo;
    uint64_t offset;
    uint64_t hop_count;
    uint64_t segment;
} DodecaEntry;

typedef struct {
    uint32_t writes;
} FtsTwinStore;

typedef struct {
    uint32_t writes;
    uint32_t deletes;
    uint32_t overflows;
    uint32_t active_cosets;
    uint32_t reserved_mask;
} FtsTwinStats;

static inline void fts_init(FtsTwinStore *s, uint64_t root_seed)
{
    (void)root_seed;
    memset(s, 0, sizeof(*s));
}

static inline int fts_write(FtsTwinStore *s,
                            uint64_t addr,
                            uint64_t value,
                            const DodecaEntry *e)
{
    (void)addr;
    (void)value;
    (void)e;
    s->writes++;
    return 0;
}

static inline FtsTwinStats fts_stats(const FtsTwinStore *s)
{
    FtsTwinStats st;
    memset(&st, 0, sizeof(st));
    st.writes = s->writes;
    return st;
}

#endif
