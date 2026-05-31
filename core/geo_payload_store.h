#ifndef GEO_PAYLOAD_STORE_H
#define GEO_PAYLOAD_STORE_H

#include <stdint.h>
#include <string.h>

#define PL_EMPTY 0xFFFFFFFFFFFFFFFFULL
#define PL_CAP 2048u

typedef struct {
    uint64_t addr[PL_CAP];
    uint64_t val[PL_CAP];
    uint8_t  used[PL_CAP];
    uint32_t count;
} PayloadStore;

typedef struct {
    uint64_t  value;
    uint8_t   found;
    struct {
        uint8_t lane;
        uint8_t slot;
        uint16_t flat;
    } id;
} PayloadResult;

static inline void pl_init(PayloadStore *ps)
{
    memset(ps, 0, sizeof(*ps));
}

static inline void pl_write(PayloadStore *ps, uint64_t addr, uint64_t value)
{
    for (uint32_t i = 0; i < ps->count; i++) {
        if (ps->used[i] && ps->addr[i] == addr) {
            ps->val[i] = value;
            return;
        }
    }
    if (ps->count < PL_CAP) {
        uint32_t i = ps->count++;
        ps->used[i] = 1u;
        ps->addr[i] = addr;
        ps->val[i] = value;
    }
}

static inline PayloadResult pl_read(PayloadStore *ps, uint64_t addr)
{
    for (uint32_t i = 0; i < ps->count; i++) {
        if (ps->used[i] && ps->addr[i] == addr) {
            PayloadResult r = { .value = ps->val[i], .found = 1u };
            return r;
        }
    }
    PayloadResult miss = { .value = 0u, .found = 0u };
    return miss;
}

#endif
