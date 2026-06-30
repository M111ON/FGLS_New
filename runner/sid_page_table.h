/*
 * sid_page_table.h — Page Table Indirect for SID zero-copy
 *
 * Core equation:  128 × 162 = 144 × 144 = 20736
 * 128 = 2⁷ (binary/Hilbert), 162 = 2×3⁴ (icosphere f=4)
 * 144 = F(12) = 2⁴×3² (Fibonacci bridge)
 *
 * DRamTile / Y-triangle / GPU buffer / GGUF share the SAME address space.
 * SID just needs to flip which page (orig vs face) the reader sees.
 *
 * bit[N]=0 → read from orig_base[N]
 * bit[N]=1 → read from face_base[N]
 *
 * CPU: tensor->data points to either orig or face (pointer swap, 8 bytes)
 * GPU: kernel tests bit[N] → selects orig_buf or face_buf (0 extra bandwidth)
 *
 * Memory: 20736 bits = 2592 bytes.  face_base is lazy-allocated per tensor.
 */

#ifndef SID_PAGE_TABLE_H
#define SID_PAGE_TABLE_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define SID_PT_BITS      20736
#define SID_PT_BYTES     (SID_PT_BITS / 8)  /* 2592 */

#define SID_PT_MAX_TENSORS 4096

/* ── Page table: 2592 bytes, one bit per address slot ── */
typedef struct {
    uint8_t  bits[SID_PT_BYTES];   /* 2592 bytes, 20736 bits */
    uint32_t n_faces;               /* how many face buffers allocated */
    uint64_t face_bytes;            /* total face memory committed */
} SidPageTable;

/* ── Face entry: per-tensor face buffer (lazy allocated) ── */
typedef struct {
    int      addr;        /* 0..20735 */
    uint8_t *face_data;   /* malloc'd CPU face buffer, NULL if not yet populated */
    size_t   face_size;   /* bytes (matches orig tensor) */
    int      populated;   /* 1 = face data ready (CPU) */

    /* GPU twin-buffer swap: zero-copy face via tensor->buffer swap.
     * gpu_twin_buf = ggml_backend_buffer* containing pre-uploaded face data.
     * gpu_twin_ptr = tensor->data value pointing into gpu_twin_buf at same
     *                offset as original tensor within its buffer.
     * gpu_orig_buf / gpu_orig_ptr = saved original for restore. */
    void    *gpu_twin_buf;
    void    *gpu_twin_ptr;
    void    *gpu_orig_buf;
    void    *gpu_orig_ptr;
} SidFaceEntry;

/* ── Journal entry: records a single bit flip for time travel ── */
typedef struct {
    int      addr;        /* 0..20735 */
    uint8_t  old_bit;     /* value before flip */
    uint8_t  new_bit;     /* value after flip */
    uint32_t tick;
} SidPTJournalEntry;

#define SID_PT_JOURNAL_MAX  8192
#define SID_PT_CHECKPOINT_MAX 64

/* Journal is a sequential array [0..count-1].  When full, oldest entries
 * are evicted (head advances).  A checkpoint stores the count at that point. */
typedef struct {
    SidPTJournalEntry entries[SID_PT_JOURNAL_MAX];
    uint32_t head;          /* oldest alive entry index */
    uint32_t count;         /* number of alive entries */
    uint32_t tick;

    char     cp_names[SID_PT_CHECKPOINT_MAX][64];
    uint32_t cp_counts[SID_PT_CHECKPOINT_MAX];   /* count value at checkpoint */
    uint32_t n_checkpoints;

    uint64_t total_flips;
} SidPTJournal;

/* ── Combined page table context ── */
typedef struct {
    SidPageTable  pt;
    SidFaceEntry  faces[SID_PT_MAX_TENSORS];
    int           n_faces;
    SidPTJournal  journal;
} SidPTContext;

/* ── Page table ops ── */

static inline void sid_pt_init(SidPageTable *pt) {
    memset(pt, 0, sizeof(*pt));
}

static inline int sid_pt_test(const SidPageTable *pt, int addr) {
    if (addr < 0 || addr >= SID_PT_BITS) return 0;
    return (pt->bits[addr >> 3] >> (addr & 7)) & 1;
}

static inline void sid_pt_set(SidPageTable *pt, int addr) {
    if (addr < 0 || addr >= SID_PT_BITS) return;
    pt->bits[addr >> 3] |= (uint8_t)(1 << (addr & 7));
}

static inline void sid_pt_clear(SidPageTable *pt, int addr) {
    if (addr < 0 || addr >= SID_PT_BITS) return;
    pt->bits[addr >> 3] &= (uint8_t)~(1 << (addr & 7));
}

/* flip bit, return old value */
static inline int sid_pt_flip(SidPageTable *pt, int addr) {
    int old = sid_pt_test(pt, addr);
    if (old)
        sid_pt_clear(pt, addr);
    else
        sid_pt_set(pt, addr);
    return old;
}

/* ── Face entry ops ── */

/* Find or create face entry for addr. Returns index or -1. */
static inline int sid_pt_face_ensure(SidPTContext *ctx, int addr, size_t sz) {
    /* check existing */
    for (int i = 0; i < ctx->n_faces; i++) {
        if (ctx->faces[i].addr == addr) return i;
    }
    /* create new */
    if (ctx->n_faces >= SID_PT_MAX_TENSORS) return -1;
    int fi = ctx->n_faces++;
    ctx->faces[fi].addr = addr;
    ctx->faces[fi].face_data = NULL;
    ctx->faces[fi].face_size = sz;
    ctx->faces[fi].populated = 0;
    ctx->pt.n_faces++;
    return fi;
}

/* Get face data pointer. If not populated, allocates + populates. */
/* Returns NULL on failure. populate_fn is called to fill the face buffer. */
typedef int (*sid_face_populate_fn)(int addr, uint8_t *buf, size_t sz, void *user);

static inline uint8_t *sid_pt_face_get(SidPTContext *ctx, int addr,
                                        sid_face_populate_fn populate_fn,
                                        void *user) {
    /* find existing face entry */
    for (int i = 0; i < ctx->n_faces; i++) {
        if (ctx->faces[i].addr != addr) continue;
        SidFaceEntry *fe = &ctx->faces[i];
        if (fe->populated) return fe->face_data;
        if (!fe->face_data && fe->face_size > 0) {
            fe->face_data = (uint8_t *)malloc(fe->face_size);
            if (!fe->face_data) return NULL;
            ctx->pt.face_bytes += fe->face_size;
        }
        if (populate_fn && fe->face_data) {
            if (populate_fn(addr, fe->face_data, fe->face_size, user) == 0)
                fe->populated = 1;
        }
        return fe->face_data;
    }
    return NULL;
}

/* Free all face data (CPU + GPU). GPU twin buffers must be freed by caller. */
static inline void sid_pt_face_free_all(SidPTContext *ctx) {
    for (int i = 0; i < ctx->n_faces; i++) {
        free(ctx->faces[i].face_data);
        ctx->faces[i].face_data = NULL;
        ctx->faces[i].populated = 0;
        ctx->faces[i].gpu_twin_buf = NULL;
        ctx->faces[i].gpu_twin_ptr = NULL;
        ctx->faces[i].gpu_orig_buf = NULL;
        ctx->faces[i].gpu_orig_ptr = NULL;
    }
    ctx->n_faces = 0;
    ctx->pt.n_faces = 0;
    ctx->pt.face_bytes = 0;
}

/* ── Journal ops (time travel) ── */

static inline void sid_pt_journal_init(SidPTJournal *j) {
    memset(j, 0, sizeof(*j));
}

/* Get linear index of entry N (0 = oldest, count-1 = newest) */
static inline SidPTJournalEntry *sid_pt_journal_entry(SidPTJournal *j, uint32_t n) {
    return &j->entries[(j->head + n) % SID_PT_JOURNAL_MAX];
}

static inline void sid_pt_journal_push(SidPTJournal *j, int addr,
                                        uint8_t old_bit, uint8_t new_bit) {
    /* If full, evict oldest entry (advance head) */
    if (j->count >= SID_PT_JOURNAL_MAX) {
        j->head = (j->head + 1) % SID_PT_JOURNAL_MAX;
        j->count--;
        /* Invalidate checkpoints that point before the evicted entry */
        uint32_t evicted_count = SID_PT_JOURNAL_MAX; /* all possible checkpoints now offset */
        for (uint32_t ci = 0; ci < j->n_checkpoints; ci++) {
            if (j->cp_counts[ci] <= evicted_count) {
                /* shift checkpoints down */
                for (uint32_t k = ci; k + 1 < j->n_checkpoints; k++) {
                    memcpy(j->cp_names[k], j->cp_names[k+1], 64);
                    j->cp_counts[k] = j->cp_counts[k+1];
                }
                j->n_checkpoints--;
                ci--;
            }
        }
    }
    uint32_t idx = (j->head + j->count) % SID_PT_JOURNAL_MAX;
    j->entries[idx].addr = addr;
    j->entries[idx].old_bit = old_bit;
    j->entries[idx].new_bit = new_bit;
    j->entries[idx].tick = j->tick++;
    j->count++;
    j->total_flips++;
}

static inline int sid_pt_journal_checkpoint(SidPTJournal *j, const char *name) {
    if (j->n_checkpoints >= SID_PT_CHECKPOINT_MAX) return -1;
    int ci = (int)j->n_checkpoints;
    int n = (int)strnlen(name, 63);
    memcpy(j->cp_names[ci], name, n);
    j->cp_names[ci][n] = 0;
    j->cp_counts[ci] = j->count;
    j->n_checkpoints++;
    return ci;
}

static inline int sid_pt_find_checkpoint(const SidPTJournal *j, const char *name) {
    for (uint32_t i = 0; i < j->n_checkpoints; i++)
        if (strcmp(j->cp_names[i], name) == 0) return (int)i;
    return -1;
}

/* Rewind to checkpoint: undo entries from newest back to cp_count.
 * undo_fn(addr, old_bit, new_bit, user) — will be called with old_bit
 * meaning what the bit should be set back TO. */
typedef void (*sid_pt_undo_fn)(int addr, uint8_t old_bit, void *user);

static inline int sid_pt_journal_rewind(SidPTJournal *j, uint32_t cp_id,
                                         sid_pt_undo_fn undo_fn, void *user) {
    if (cp_id >= j->n_checkpoints) return -1;
    uint32_t target = j->cp_counts[cp_id];
    int undone = 0;
    while (j->count > target) {
        SidPTJournalEntry *e = sid_pt_journal_entry(j, j->count - 1);
        if (undo_fn)
            undo_fn(e->addr, e->old_bit, user);
        j->count--;
        undone++;
    }
    return undone;
}

/* Fast-forward from checkpoint: redo entries from cp_count to count.
 * redo_fn(addr, new_bit, user) — applies the new bit value. */
typedef void (*sid_pt_redo_fn)(int addr, uint8_t new_bit, void *user);

static inline int sid_pt_journal_ffwd(SidPTJournal *j, uint32_t cp_id,
                                       sid_pt_redo_fn redo_fn, void *user) {
    if (cp_id >= j->n_checkpoints) return -1;
    uint32_t target = j->cp_counts[cp_id];
    int redone = 0;
    for (uint32_t i = target; i < j->count; i++) {
        SidPTJournalEntry *e = sid_pt_journal_entry(j, i);
        if (redo_fn)
            redo_fn(e->addr, e->new_bit, user);
        redone++;
    }
    return redone;
}

/* ── Combined context ops ── */

static inline void sid_pt_context_init(SidPTContext *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    sid_pt_init(&ctx->pt);
    sid_pt_journal_init(&ctx->journal);
}

static inline void sid_pt_context_destroy(SidPTContext *ctx) {
    sid_pt_face_free_all(ctx);
}

/* Print summary */
static inline void sid_pt_print(const SidPTContext *ctx) {
    int active = 0;
    for (int i = 0; i < SID_PT_BITS; i++)
        if (sid_pt_test(&ctx->pt, i)) active++;
    fprintf(stderr, "[sid-pt] pages=%d/%d  faces=%d face_bytes=%llu  journal=%u/%d flips=%llu\n",
            active, SID_PT_BITS, ctx->pt.n_faces,
            (unsigned long long)ctx->pt.face_bytes,
            ctx->journal.count, SID_PT_JOURNAL_MAX,
            (unsigned long long)ctx->journal.total_flips);
}

#endif /* SID_PAGE_TABLE_H */
