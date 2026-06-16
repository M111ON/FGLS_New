/*
 * sid_timetravel.h — SID Time Travel Orchestrator
 * ═════════════════════════════════════════════════
 *
 * ตัว orchestrate ที่ต่อ delta ring เข้ากับ runner loop:
 *
 *   sid_timetravel_before_decode()  → push entries ก่อน swap
 *   sid_timetravel_after_decode()   → process pending commands หลัง restore
 *
 * Commands (set ผ่าน --sid-* CLI flags หรือ /chat command):
 *   checkpoint NAME   → mark ring position
 *   rewind NAME       → undo swaps กลับไป checkpoint
 *   ff NAME           → redo swaps จาก checkpoint
 *   branch NAME       → fork ring state สำหรับ experiment
 *   /tt               → print state (ring count, checkpoints, stats)
 *
 * Flow in runner:
 *   sid_swap_apply()
 *     ├─ delta_push(entry) for each swap  ← BEFORE actual pointer swap
 *     └─ tensor_set_data(sid_data)        ← swap happens
 *   llama_decode()
 *     ├─ ใช้ sid_data จริง (backend reads from swapped pointer)
 *   sid_swap_restore()
 *     ├─ tensor_set_data(orig_data)       ← restore
 *     └─ after_decode()                   ← process pending commands HERE
 *
 * Pending command model:
 *   CLI flag / chat command → set pending_* flag
 *   after_decode() → check and execute pending
 *   → ทำให้ command เหมือน "รอ decode เสร็จก่อน" ไม่ใช่แทรกกลาง decode
 *
 * Integration:
 *   SidTimeTravel tt;
 *   sid_timetravel_init(&tt);
 *   // ใน sid_swap_apply():
 *   sid_timetravel_before_decode(&tt, n, ft_idx, ptrs, orig, sid, sizes);
 *   // ใน sid_swap_restore():
 *   sid_timetravel_after_decode(&tt);
 *
 *   CLI parsing:
 *   sid_timetravel_set_checkpoint(&tt, name);
 *   sid_timetravel_set_rewind(&tt, name);
 *   sid_timetravel_set_ffwd(&tt, name);
 *   sid_timetravel_set_branch(&tt, name);
 */
#ifndef SID_TIMETRAVEL_H
#define SID_TIMETRAVEL_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "sid_delta_ring.h"

#define TT_CMD_NONE     0
#define TT_CMD_CHECKPOINT 1
#define TT_CMD_REWIND   2
#define TT_CMD_FFWD     3
#define TT_CMD_BRANCH   4

typedef struct {
    SidDeltaRing ring;
    int32_t      pending_checkpoint;
    int32_t      pending_rewind;
    int32_t      pending_ffwd;
    int32_t      pending_branch_cp;
    char         pending_checkpoint_name[64];
    char         pending_rewind_name[64];
    char         pending_ffwd_name[64];
    char         pending_branch_name[64];
    uint32_t     branch_count;
    uint32_t     checkpoint_seq;
} SidTimeTravel;

static inline void sid_timetravel_init(SidTimeTravel *tt)
{
    memset(tt, 0, sizeof(*tt));
    tt->pending_checkpoint = -1;
    tt->pending_rewind = -1;
    tt->pending_ffwd = -1;
    tt->pending_branch_cp = -1;
}

static inline void sid_timetravel_set_checkpoint(SidTimeTravel *tt, const char *name)
{
    tt->pending_checkpoint = 1;
    int nlen = (int)strnlen(name, 63);
    memcpy(tt->pending_checkpoint_name, name, nlen);
    tt->pending_checkpoint_name[nlen] = 0;
}

static inline void sid_timetravel_set_rewind(SidTimeTravel *tt, const char *name)
{
    tt->pending_rewind = 1;
    int nlen = (int)strnlen(name, 63);
    memcpy(tt->pending_rewind_name, name, nlen);
    tt->pending_rewind_name[nlen] = 0;
}

static inline void sid_timetravel_set_ffwd(SidTimeTravel *tt, const char *name)
{
    tt->pending_ffwd = 1;
    int nlen = (int)strnlen(name, 63);
    memcpy(tt->pending_ffwd_name, name, nlen);
    tt->pending_ffwd_name[nlen] = 0;
}

static inline void sid_timetravel_set_branch(SidTimeTravel *tt, const char *name)
{
    tt->pending_branch_cp = 1;
    int nlen = (int)strnlen(name, 63);
    memcpy(tt->pending_branch_name, name, nlen);
    tt->pending_branch_name[nlen] = 0;
}

static inline int sid_timetravel_before_decode(SidTimeTravel *tt,
                                                int n_swaps,
                                                int *ft_indices,
                                                void **tensor_ptrs,
                                                void **orig_datas,
                                                void **sid_datas,
                                                size_t *sizes)
{
    for (int i = 0; i < n_swaps; i++) {
        sid_delta_push(&tt->ring,
                       ft_indices[i],
                       tensor_ptrs[i],
                       orig_datas[i],
                       sid_datas[i],
                       sizes[i]);
    }
    return 0;
}

static inline int sid_timetravel_after_decode(SidTimeTravel *tt)
{
    int did_action = 0;

    if (tt->pending_checkpoint >= 0) {
        int cp_id = sid_checkpoint(&tt->ring, tt->pending_checkpoint_name);
        if (cp_id >= 0) {
            fprintf(stderr, "[timetravel] checkpoint '%s' #%d at ring[%u]\n",
                    tt->pending_checkpoint_name, cp_id,
                    (unsigned)tt->ring.checkpoints[cp_id].ring_index);
        }
        tt->pending_checkpoint = -1;
        did_action = 1;
    }

    if (tt->pending_rewind >= 0) {
        int cp_id = sid_find_checkpoint(&tt->ring, tt->pending_rewind_name);
        if (cp_id >= 0) {
            int n = sid_rewind_to_checkpoint(&tt->ring, (uint32_t)cp_id);
            fprintf(stderr, "[timetravel] rewind to '%s': %d entries undone\n",
                    tt->pending_rewind_name, n);
        } else {
            fprintf(stderr, "[timetravel] checkpoint '%s' not found\n",
                    tt->pending_rewind_name);
        }
        tt->pending_rewind = -1;
        did_action = 1;
    }

    if (tt->pending_ffwd >= 0) {
        int cp_id = sid_find_checkpoint(&tt->ring, tt->pending_ffwd_name);
        if (cp_id >= 0) {
            int n = sid_ffwd_from_checkpoint(&tt->ring, (uint32_t)cp_id);
            fprintf(stderr, "[timetravel] fast-forward to '%s': %d entries reapplied\n",
                    tt->pending_ffwd_name, n);
        } else {
            fprintf(stderr, "[timetravel] checkpoint '%s' not found\n",
                    tt->pending_ffwd_name);
        }
        tt->pending_ffwd = -1;
        did_action = 1;
    }

    if (tt->pending_branch_cp >= 0) {
        int cp_id = sid_checkpoint(&tt->ring, tt->pending_checkpoint_name);
        if (cp_id >= 0) {
            fprintf(stderr, "[timetravel] checkpoint '%s' #%d at ring[%u]\n",
                    tt->pending_checkpoint_name, cp_id,
                    (unsigned)tt->ring.checkpoints[cp_id].ring_index);
        }
        tt->pending_checkpoint = -1;
        did_action = 1;
    }

    return did_action;
}

static inline void sid_timetravel_print_state(const SidTimeTravel *tt)
{
    fprintf(stderr, "\n[timetravel] delta ring: %u entries (head=%u tail=%u)\n",
            tt->ring.count, tt->ring.head, tt->ring.tail);
    fprintf(stderr, "[timetravel] total pushed=%llu rewound=%llu ffwd=%llu\n",
            (unsigned long long)tt->ring.total_pushed,
            (unsigned long long)tt->ring.total_rewound,
            (unsigned long long)tt->ring.total_ffwd);
    fprintf(stderr, "[timetravel] checkpoints: %u\n", tt->ring.n_checkpoints);
    for (uint32_t i = 0; i < tt->ring.n_checkpoints; i++) {
        const SidCheckpoint *cp = &tt->ring.checkpoints[i];
        fprintf(stderr, "  cp[%u] '%s' ring[%u] ts=%u\n",
                cp->id, cp->name, cp->ring_index, cp->timestamp);
    }
}

#endif
