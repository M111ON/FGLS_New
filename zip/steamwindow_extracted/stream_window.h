/*
 * stream_window.h — Twin Geometry Weight Streaming
 *
 * Caller handles threading. No malloc after init. O(1) slot lookup.
 * Supports PWC V3 and GGUF direct tensors via format tag.
 *
 * Flow:
 *   sw_init()  → allocate VRAM window (caller provides buffer)
 *   sw_request() → CPU seeks header, queues tensor into slot
 *   sw_ready()   → check if slot is loaded (caller drives timing)
 *   sw_consume() → GPU gets pointer, marks slot in-use
 *   sw_release() → slot free for next tensor
 *   sw_flush()   → sync boundary (call at gear_lock c144 boundary)
 */

#ifndef STREAM_WINDOW_H
#define STREAM_WINDOW_H

#include <stdint.h>
#include <string.h>
#include "gear_lock.h"

/* ── Format tag ─────────────────────────────────────────────────── */
#define SW_FMT_PWC   0u   /* PWC V3: name_table + payload offsets  */
#define SW_FMT_GGUF  1u   /* GGUF direct: tensor offset in file    */

/* ── Slot states ────────────────────────────────────────────────── */
#define SW_SLOT_FREE    0u
#define SW_SLOT_PENDING 1u   /* CPU queued, not yet loaded          */
#define SW_SLOT_READY   2u   /* loaded into window buffer           */
#define SW_SLOT_INUSE   3u   /* GPU consuming                       */

/* ── Limits ─────────────────────────────────────────────────────── */
#define SW_MAX_SLOTS    32u  /* concurrent tensors in window        */
#define SW_NAME_LEN     64u

/* ── Tensor descriptor ──────────────────────────────────────────── */
typedef struct {
    char     name[SW_NAME_LEN];
    uint64_t file_offset;     /* byte offset in PWC/GGUF file       */
    uint64_t byte_size;       /* tensor size in bytes               */
    uint32_t geo_addr;        /* SID geometric address (optional)   */
    uint8_t  fmt;             /* SW_FMT_PWC / SW_FMT_GGUF          */
    uint8_t  slot;            /* assigned window slot               */
    uint8_t  state;           /* SW_SLOT_*                          */
    uint8_t  _pad;
} SwTensor;

/* ── Window slot ────────────────────────────────────────────────── */
typedef struct {
    uint8_t  *buf;            /* pointer into window_buf            */
    uint64_t  capacity;       /* bytes allocated to this slot       */
    uint64_t  used;           /* bytes actually loaded              */
    uint8_t   state;
    uint8_t   _pad[7];
} SwSlot;

/* ── Stream window ──────────────────────────────────────────────── */
typedef struct {
    /* VRAM window — caller provides buffer */
    uint8_t  *window_buf;     /* flat buffer (caller alloc)         */
    uint64_t  window_bytes;   /* total VRAM budget                  */
    uint64_t  used_bytes;

    /* Slots */
    SwSlot    slots[SW_MAX_SLOTS];
    SwTensor  tensors[SW_MAX_SLOTS];
    uint8_t   n_slots;

    /* Gear sync */
    GearLock *gear;

    /* Stats */
    uint32_t  loads;
    uint32_t  evictions;
    uint32_t  flushes;
} StreamWindow;

/* ── Init ───────────────────────────────────────────────────────── */
static inline void sw_init(StreamWindow *sw,
                            uint8_t *buf, uint64_t bytes,
                            GearLock *gear)
{
    memset(sw, 0, sizeof(*sw));
    sw->window_buf   = buf;
    sw->window_bytes = bytes;
    sw->gear         = gear;

    /* Divide window evenly across slots */
    uint64_t slot_cap = bytes / SW_MAX_SLOTS;
    for (uint8_t i = 0; i < SW_MAX_SLOTS; i++) {
        sw->slots[i].buf      = buf + (uint64_t)i * slot_cap;
        sw->slots[i].capacity = slot_cap;
        sw->slots[i].state    = SW_SLOT_FREE;
    }
    sw->n_slots = SW_MAX_SLOTS;
}

/* ── Find free slot ─────────────────────────────────────────────── */
static inline int sw_alloc_slot(StreamWindow *sw, uint64_t need_bytes)
{
    for (uint8_t i = 0; i < sw->n_slots; i++) {
        if (sw->slots[i].state == SW_SLOT_FREE &&
            sw->slots[i].capacity >= need_bytes)
            return i;
    }
    return -1; /* no space — caller must release or flush */
}

/* ── CPU: queue tensor request ──────────────────────────────────── */
/* Returns slot index, or -1 if window full */
static inline int sw_request(StreamWindow *sw,
                              const char *name,
                              uint64_t file_offset,
                              uint64_t byte_size,
                              uint32_t geo_addr,
                              uint8_t  fmt)
{
    int s = sw_alloc_slot(sw, byte_size);
    if (s < 0) return -1;

    SwTensor *t = &sw->tensors[s];
    strncpy(t->name, name, SW_NAME_LEN - 1);
    t->file_offset = file_offset;
    t->byte_size   = byte_size;
    t->geo_addr    = geo_addr;
    t->fmt         = fmt;
    t->slot        = (uint8_t)s;
    t->state       = SW_SLOT_PENDING;

    sw->slots[s].state = SW_SLOT_PENDING;
    sw->slots[s].used  = 0;
    sw->used_bytes    += byte_size;

    if (sw->gear) gear_cpu_tick(sw->gear);
    return s;
}

/* ── CPU: mark slot loaded (after fread/mmap into slot buf) ─────── */
static inline void sw_mark_ready(StreamWindow *sw, int slot, uint64_t loaded)
{
    if (slot < 0 || slot >= SW_MAX_SLOTS) return;
    sw->slots[slot].used  = loaded;
    sw->slots[slot].state = SW_SLOT_READY;
    sw->tensors[slot].state = SW_SLOT_READY;
    sw->loads++;
}

/* ── GPU: check if slot ready ───────────────────────────────────── */
static inline int sw_ready(const StreamWindow *sw, int slot)
{
    if (slot < 0 || slot >= SW_MAX_SLOTS) return 0;
    return sw->slots[slot].state == SW_SLOT_READY;
}

/* ── GPU: consume — returns pointer to buffer ───────────────────── */
static inline uint8_t *sw_consume(StreamWindow *sw, int slot, uint64_t *out_size)
{
    if (!sw_ready(sw, slot)) return (uint8_t *)0;
    sw->slots[slot].state   = SW_SLOT_INUSE;
    sw->tensors[slot].state = SW_SLOT_INUSE;
    if (out_size) *out_size = sw->slots[slot].used;
    if (sw->gear) gear_gpu_tick(sw->gear, 1);
    return sw->slots[slot].buf;
}

/* ── GPU: release slot after done ───────────────────────────────── */
static inline void sw_release(StreamWindow *sw, int slot)
{
    if (slot < 0 || slot >= SW_MAX_SLOTS) return;
    sw->used_bytes -= sw->slots[slot].used;
    sw->slots[slot].state   = SW_SLOT_FREE;
    sw->slots[slot].used    = 0;
    sw->tensors[slot].state = SW_SLOT_FREE;
}

/* ── Evict oldest FREE slot to make room ────────────────────────── */
static inline int sw_evict(StreamWindow *sw, uint64_t need_bytes)
{
    for (uint8_t i = 0; i < sw->n_slots; i++) {
        if (sw->slots[i].state == SW_SLOT_FREE) {
            /* already free — try alloc again */
            if (sw->slots[i].capacity >= need_bytes) {
                sw->evictions++;
                return i;
            }
        }
    }
    return -1;
}

/* ── Flush boundary (call at c144 / gear boundary) ─────────────── */
/* Releases all READY slots not yet consumed — safe drain point     */
static inline void sw_flush(StreamWindow *sw)
{
    for (uint8_t i = 0; i < sw->n_slots; i++) {
        if (sw->slots[i].state == SW_SLOT_READY)
            sw_release(sw, i);
    }
    sw->flushes++;
}

/* ── Stats ──────────────────────────────────────────────────────── */
static inline void sw_print_stats(const StreamWindow *sw, FILE *fp)
{
    fprintf(fp, "=== StreamWindow ===\n");
    fprintf(fp, "  budget=%llu used=%llu (%.1f%%)\n",
            (unsigned long long)sw->window_bytes,
            (unsigned long long)sw->used_bytes,
            sw->window_bytes ? 100.0*sw->used_bytes/sw->window_bytes : 0.0);
    fprintf(fp, "  loads=%u evictions=%u flushes=%u\n",
            sw->loads, sw->evictions, sw->flushes);
    uint8_t free=0, pend=0, ready=0, inuse=0;
    for (uint8_t i = 0; i < sw->n_slots; i++) {
        switch(sw->slots[i].state) {
            case SW_SLOT_FREE:    free++;  break;
            case SW_SLOT_PENDING: pend++;  break;
            case SW_SLOT_READY:   ready++; break;
            case SW_SLOT_INUSE:   inuse++; break;
        }
    }
    fprintf(fp, "  slots: free=%u pending=%u ready=%u inuse=%u\n",
            free, pend, ready, inuse);
}

#endif /* STREAM_WINDOW_H */
