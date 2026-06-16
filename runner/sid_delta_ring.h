/*
 * sid_delta_ring.h — SID Delta Ring Journal
 * ═══════════════════════════════════════════
 *
 * Circular buffer ที่บันทึก tensor pointer swap ทุกครั้ง ก่อน sid_swap_apply()
 * เพื่อให้สามารถ rewind (ย้อนกลับ) หรือ fast-forward (เล่นซ้ำ) ได้
 * โดยไม่ต้อง copy tensor data — แค่ log pointer address
 *
 * How it works:
 *   ก่อน swap ทุกครั้ง → sid_delta_push() เก็บ (orig_data, sid_data, tensor_ptr)
 *   rewind             → วนถอยหลังจาก tail → checkpoint, restore orig_data pointer
 *   fast-forward       → วนจาก checkpoint → head, re-apply sid_data pointer
 *   checkpoint         → set bookmark (name + ring_index) ที่ตำแหน่งปัจจุบัน
 *   branch             → คัดลอก entries ตั้งแต่ tail → checkpoint ไป ring ใหม่
 *
 * No data copying — pointer journaling only.
 * 1024 entries ≈ 3 tokens (for 170 tensors × 2 swap cycles/token)
 *
 * Key concept:
 *   เหมือน git log ของ tensor swap — commit (checkpoint), reset (rewind),
 *   revert (fast-forward), branch (branch) แต่ทุก operation เป็น pointer swap O(1)
 *
 * Time travel use cases:
 *   1. เช็ค output ก่อน/หลัง SID injection
 *   2. เปรียบเทียบ SID config ต่างกันจาก checkpoint เดียวกัน
 *   3. Branch เพื่อลอง experiment โดยไม่แตะ main ring
 */
#ifndef SID_DELTA_RING_H
#define SID_DELTA_RING_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define SID_DELTA_MAX_ENTRIES   1024
#define SID_CHECKPOINT_MAX      64
#define SID_FFWD_STACK_MAX      128

typedef struct {
    uint32_t timestamp;
    int      ft_idx;
    void    *tensor_ptr;
    void    *orig_data;
    void    *sid_data;
    size_t   size_bytes;
    uint8_t  active;
    uint8_t  pad[7];
} SidDeltaEntry;

typedef struct {
    uint32_t id;
    char     name[64];
    uint32_t ring_index;
    uint32_t timestamp;
} SidCheckpoint;

typedef struct {
    SidDeltaEntry ring[SID_DELTA_MAX_ENTRIES];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t timestamp;

    SidCheckpoint checkpoints[SID_CHECKPOINT_MAX];
    uint32_t n_checkpoints;

    uint64_t total_pushed;
    uint64_t total_rewound;
    uint64_t total_ffwd;
} SidDeltaRing;

static inline void sid_delta_ring_init(SidDeltaRing *r)
{
    memset(r, 0, sizeof(*r));
}

static inline uint32_t sid_delta_push(SidDeltaRing *r,
                                       int      ft_idx,
                                       void    *tensor_ptr,
                                       void    *orig_data,
                                       void    *sid_data,
                                       size_t   size_bytes)
{
    if (r->count >= SID_DELTA_MAX_ENTRIES) {
        /* warn if any checkpoint sits at the tail position being evicted */
        for (uint32_t ci = 0; ci < r->n_checkpoints; ci++) {
            if (r->checkpoints[ci].ring_index == r->tail) {
                fprintf(stderr, "[delta_ring] WARN: checkpoint '%s' evicted (ring full)\n",
                        r->checkpoints[ci].name);
                for (uint32_t k = ci; k + 1 < r->n_checkpoints; k++)
                    r->checkpoints[k] = r->checkpoints[k + 1];
                r->n_checkpoints--;
                ci--;
            }
        }
        r->tail = (r->tail + 1) % SID_DELTA_MAX_ENTRIES;
        r->count--;
    }
    SidDeltaEntry *e = &r->ring[r->head];
    e->timestamp  = r->timestamp++;
    e->ft_idx     = ft_idx;
    e->tensor_ptr = tensor_ptr;
    e->orig_data  = orig_data;
    e->sid_data   = sid_data;
    e->size_bytes = size_bytes;
    e->active     = 1;

    uint32_t idx = r->head;
    r->head = (r->head + 1) % SID_DELTA_MAX_ENTRIES;
    r->count++;
    r->total_pushed++;
    return idx;
}

static inline int sid_checkpoint(SidDeltaRing *r, const char *name)
{
    if (r->n_checkpoints >= SID_CHECKPOINT_MAX) return -1;
    uint32_t idx = r->n_checkpoints++;
    SidCheckpoint *cp = &r->checkpoints[idx];
    cp->id = idx;
    cp->ring_index = (r->head == 0) ? SID_DELTA_MAX_ENTRIES - 1 : r->head - 1;
    cp->timestamp = r->timestamp;
    int nlen = (int)strnlen(name, 63);
    memcpy(cp->name, name, nlen);
    cp->name[nlen] = 0;
    return (int)idx;
}

static inline int sid_find_checkpoint(const SidDeltaRing *r, const char *name)
{
    for (uint32_t i = 0; i < r->n_checkpoints; i++) {
        if (strcmp(r->checkpoints[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static inline int sid_rewind_to_checkpoint(SidDeltaRing *r, uint32_t cp_id)
{
    if (cp_id >= r->n_checkpoints) return -1;
    SidCheckpoint *cp = &r->checkpoints[cp_id];
    uint32_t target = cp->ring_index;
    uint32_t replayed = 0;

    while (r->count > 0) {
        uint32_t last = (r->head == 0) ? SID_DELTA_MAX_ENTRIES - 1 : r->head - 1;
        SidDeltaEntry *e = &r->ring[last];

        if (last == target) break;

        void *tensor_data_ptr = (char*)e->tensor_ptr + 248;
        void *data; memcpy(&data, tensor_data_ptr, sizeof(void*));
        if (data == e->sid_data) {
            memcpy(tensor_data_ptr, &e->orig_data, sizeof(void*));
        }

        e->active = 0;
        r->head = last;
        r->count--;
        replayed++;
    }
    r->total_rewound += replayed;
    return (int)replayed;
}

static inline int sid_ffwd_from_checkpoint(SidDeltaRing *r, uint32_t cp_id)
{
    if (cp_id >= r->n_checkpoints) return -1;
    SidCheckpoint *cp = &r->checkpoints[cp_id];
    uint32_t pos = (cp->ring_index + 1) % SID_DELTA_MAX_ENTRIES;
    int replayed = 0;
    int guard = 0;

    while (pos != r->head && guard < SID_DELTA_MAX_ENTRIES) {
        SidDeltaEntry *e = &r->ring[pos];
        if (!e->active) {
            void *tensor_data_ptr = (char*)e->tensor_ptr + 248;
            memcpy(tensor_data_ptr, &e->sid_data, sizeof(void*));
            e->active = 1;
        }
        pos = (pos + 1) % SID_DELTA_MAX_ENTRIES;
        replayed++;
        guard++;
    }
    r->total_ffwd += replayed;
    return replayed;
}

static inline int sid_branch(const SidDeltaRing *src, uint32_t cp_id,
                              SidDeltaRing *dst)
{
    if (!src || !dst) return -1;
    if (cp_id >= src->n_checkpoints) return -1;
    memset(dst, 0, sizeof(*dst));

    uint32_t start_tail = src->tail;
    uint32_t end = (src->checkpoints[cp_id].ring_index + 1) % SID_DELTA_MAX_ENTRIES;

    uint32_t pos = start_tail;
    int guard = 0;
    while (pos != end && guard < SID_DELTA_MAX_ENTRIES) {
        const SidDeltaEntry *se = &src->ring[pos];
        if (se->active) {
            sid_delta_push(dst, se->ft_idx, se->tensor_ptr,
                          se->orig_data, se->sid_data, se->size_bytes);
        }
        pos = (pos + 1) % SID_DELTA_MAX_ENTRIES;
        guard++;
    }

    for (uint32_t i = 0; i <= cp_id && i < src->n_checkpoints; i++) {
        const SidCheckpoint *scp = &src->checkpoints[i];
        SidCheckpoint *dcp = &dst->checkpoints[dst->n_checkpoints];
        dcp->id = dst->n_checkpoints;
        memcpy(dcp->name, scp->name, 64);
        dcp->ring_index = dst->head == 0 ? 0 : dst->head - 1;
        dcp->timestamp = dst->timestamp;
        dst->n_checkpoints++;
    }
    return (int)dst->n_checkpoints;
}

static inline void sid_delta_mark_all_inactive(SidDeltaRing *r)
{
    for (uint32_t i = 0; i < SID_DELTA_MAX_ENTRIES; i++)
        r->ring[i].active = 0;
}

#endif
