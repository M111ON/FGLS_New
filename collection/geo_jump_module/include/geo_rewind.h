#pragma once
#include "geo_temporal_lut.h"
#include "geo_tring_stream.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define REWIND_SLOTS  972u
#define REWIND_MAX_SNAPSHOTS 4u

typedef struct {
    uint32_t    enc;
    TStreamChunk chunk;
    bool        valid;
} RewindSlot;

typedef struct {
    RewindSlot  slots[REWIND_SLOTS];
    uint16_t    head;
    uint32_t    stored;
} RewindBuffer;

typedef struct {
    bool       active;
    uint16_t   head;
    uint32_t   pinned;
    RewindSlot slots[REWIND_SLOTS];
} RewindSnapshot;

typedef struct {
    const RewindBuffer *owner;
    RewindSnapshot      snaps[REWIND_MAX_SNAPSHOTS];
} RewindSnapshotBook;

static inline RewindSnapshotBook *_rewind_book(const RewindBuffer *rb)
{
    enum { REWIND_BOOKS = 8 };
    static RewindSnapshotBook books[REWIND_BOOKS];
    for (uint8_t i = 0u; i < REWIND_BOOKS; i++)
        if (books[i].owner == rb)
            return &books[i];
    for (uint8_t i = 0u; i < REWIND_BOOKS; i++) {
        if (books[i].owner == NULL) {
            books[i].owner = rb;
            memset(books[i].snaps, 0, sizeof(books[i].snaps));
            return &books[i];
        }
    }
    return &books[0];
}

static inline uint8_t _rewind_slot_pinned(const RewindBuffer *rb, uint16_t idx)
{
    RewindSnapshotBook *book = _rewind_book(rb);
    for (uint8_t s = 0u; s < REWIND_MAX_SNAPSHOTS; s++) {
        if (book->snaps[s].active && book->snaps[s].slots[idx].valid)
            return 1u;
    }
    return 0u;
}

static inline void rewind_init(RewindBuffer *rb) {
    memset(rb, 0, sizeof(*rb));
    RewindSnapshotBook *book = _rewind_book(rb);
    book->owner = rb;
    memset(book->snaps, 0, sizeof(book->snaps));
}

static inline void rewind_store(RewindBuffer *rb,
                                uint32_t enc,
                                const TStreamChunk *chunk)
{
    uint16_t idx = rb->head % REWIND_SLOTS;
    uint16_t start = idx;
    while (_rewind_slot_pinned(rb, idx)) {
        idx = (uint16_t)((idx + 1u) % REWIND_SLOTS);
        if (idx == start) break;
    }
    rb->slots[idx].enc   = enc;
    rb->slots[idx].chunk = *chunk;
    rb->slots[idx].valid = true;
    rb->head = (uint16_t)((idx + 1u) % REWIND_SLOTS);
    rb->stored++;
}

static inline const TStreamChunk *rewind_find(const RewindBuffer *rb,
                                               uint32_t enc)
{
    uint16_t hint = tring_pos(enc) % REWIND_SLOTS;
    if (rb->slots[hint].valid && rb->slots[hint].enc == enc)
        return &rb->slots[hint].chunk;
    for (uint16_t i = 0; i < REWIND_SLOTS; i++) {
        if (rb->slots[i].valid && rb->slots[i].enc == enc)
            return &rb->slots[i].chunk;
    }
    return NULL;
}

static inline bool rewind_has(const RewindBuffer *rb, uint32_t enc) {
    return rewind_find(rb, enc) != NULL;
}

typedef struct {
    uint32_t stored;
    uint32_t occupied;
    uint32_t pinned;
    uint32_t snapshots;
} RewindStats;

static inline void rewind_stats(const RewindBuffer *rb, RewindStats *st) {
    RewindSnapshotBook *book = _rewind_book(rb);
    st->stored   = rb->stored;
    st->occupied = 0;
    st->pinned   = 0;
    st->snapshots = 0;
    for (uint16_t i = 0; i < REWIND_SLOTS; i++)
        if (rb->slots[i].valid) st->occupied++;
    for (uint8_t s = 0u; s < REWIND_MAX_SNAPSHOTS; s++) {
        if (!book->snaps[s].active) continue;
        st->snapshots++;
        st->pinned += book->snaps[s].pinned;
    }
}

static inline bool rewind_snapshot_take(RewindBuffer *rb, uint32_t snap_id)
{
    if (snap_id >= REWIND_MAX_SNAPSHOTS) return false;
    RewindSnapshotBook *book = _rewind_book(rb);
    RewindSnapshot *snap = &book->snaps[snap_id];
    if (snap->active) return false;
    snap->active = true;
    snap->head   = rb->head;
    snap->pinned = 0u;
    memcpy(snap->slots, rb->slots, sizeof(rb->slots));
    for (uint16_t i = 0u; i < REWIND_SLOTS; i++)
        if (snap->slots[i].valid) snap->pinned++;
    return true;
}

static inline bool rewind_snapshot_drop(RewindBuffer *rb, uint32_t snap_id)
{
    if (snap_id >= REWIND_MAX_SNAPSHOTS) return false;
    RewindSnapshotBook *book = _rewind_book(rb);
    RewindSnapshot *snap = &book->snaps[snap_id];
    if (!snap->active) return false;
    memset(snap, 0, sizeof(*snap));
    return true;
}

static inline bool rewind_snapshot_restore(RewindBuffer *rb, uint32_t snap_id)
{
    if (snap_id >= REWIND_MAX_SNAPSHOTS) return false;
    RewindSnapshotBook *book = _rewind_book(rb);
    RewindSnapshot *snap = &book->snaps[snap_id];
    if (!snap->active) return false;
    memcpy(rb->slots, snap->slots, sizeof(rb->slots));
    rb->head = snap->head;
    return true;
}
