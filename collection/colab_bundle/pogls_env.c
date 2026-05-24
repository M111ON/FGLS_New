/*
 * pogls_env.c — POGLS RL Environment
 * Compile: gcc -O2 -shared -fPIC -I. pogls_env.c -o pogls_env.so
 *
 * State  : uint64_t addr (8 bytes, no copy)
 * Actions: 0=ORBITAL 1=CHIRAL 2=CROSS 3=HUB
 * Reward : geometry-driven (isect_pop + zone bonus)
 * Done   : zone boundary (isect==0) or max_steps
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "skeleton_index.h"
#include "geo_reshape_junction.h"

/* ── Config ─────────────────────────────────────────────── */
#define ENV_MAX_STEPS   512u
#define ENV_CHUNK_SZ     64u
#define ENV_DATA_SZ    (20736u * 64u)  /* 1 full junction × lens */

/* ── Env state ──────────────────────────────────────────── */
typedef struct {
    uint64_t  addr;
    uint32_t  steps;
    uint32_t  max_steps;
    uint8_t  *data;       /* ENV_DATA_SZ bytes, caller-provided */
    uint32_t  data_sz;
    /* stats */
    uint32_t  zone_resets;
    uint32_t  hits[4];    /* per-action hit count */
} PoglsEnv;

/* ── Traverse: addr + action → next addr ────────────────── */
static uint64_t _traverse(uint64_t addr, int action) {
    SkeletonIdx sk = skeleton_lookup(addr);

    switch (action) {
    case 0: /* ORBITAL: next hex in ring (slot +1 within face) */
        return addr + 1;

    case 1: /* CHIRAL: jump to mirror face (f ↔ f+6) */
        {
            /* partner face start = (zone+6)%12 * FACE_SZ */
            uint64_t base = (uint64_t)((sk.zone + 6u) % 12u) * SKEL_FACE_SZ;
            uint64_t slot = sk.enc % SKEL_FACE_SZ;
            return base + slot;
        }

    case 2: /* CROSS: Metatron cross partner face */
        {
            uint64_t base = (uint64_t)sk.partner * SKEL_FACE_SZ;
            uint64_t slot = sk.enc % SKEL_FACE_SZ;
            return base + slot;
        }

    case 3: /* HUB: jump to pentagon anchor start */
        return (uint64_t)sk.zone * RJ_PENTAGON;

    default:
        return addr;
    }
}

/* ── Reward: geometry-driven ────────────────────────────── */
static float _reward(uint64_t addr, const uint8_t *data, uint32_t data_sz) {
    /* get chunk at addr */
    uint64_t offset = (addr * ENV_CHUNK_SZ) % (data_sz - ENV_CHUNK_SZ);
    const uint8_t *chunk = data + offset;

    uint8_t ip = skel_isect_pop(chunk);
    PentagonAnchor a = rj_anchor_lookup(addr);

    float r = 0.0f;

    /* structured zone bonus */
    if (ip == 0)         r += 3.0f;   /* dead zone = boundary found */
    else if (ip < 8)     r += 2.0f;   /* fibo territory */
    else if (ip < 16)    r += 1.0f;   /* structured */
    else                 r -= 0.5f;   /* residual zone penalty */

    /* near pentagon center bonus */
    if (a.cell_icosa == 0)  r += 0.5f;
    if (a.frame_rubik == 0) r += 0.3f;

    /* exploration bonus: pair diversity */
    r += (float)a.pair * 0.1f;

    return r;
}

/* ══════════════════════════════════════════════════════════
   PUBLIC API (exported via shared lib)
   ══════════════════════════════════════════════════════════ */

/* Create env — data must be ENV_DATA_SZ bytes */
PoglsEnv* pogls_env_create(uint8_t *data, uint32_t data_sz, uint32_t max_steps) {
    PoglsEnv *e = (PoglsEnv*)calloc(1, sizeof(PoglsEnv));
    if (!e) return NULL;
    e->data      = data;
    e->data_sz   = data_sz ? data_sz : ENV_DATA_SZ;
    e->max_steps = max_steps ? max_steps : ENV_MAX_STEPS;
    return e;
}

void pogls_env_free(PoglsEnv *e) { free(e); }

/* Reset: returns initial addr */
uint64_t pogls_env_reset(PoglsEnv *e) {
    e->addr  = 0;
    e->steps = 0;
    return e->addr;
}

/*
 * Step: returns packed result in out[4]
 *   out[0] = next_addr (uint64, split lo/hi)
 *   out[0] = next_addr_lo (uint32)
 *   out[1] = next_addr_hi (uint32)
 *   out[2] = reward as float bits (uint32)
 *   out[3] = done (0/1)
 */
void pogls_env_step(PoglsEnv *e, int action, uint32_t *out) {
    uint64_t next = _traverse(e->addr, action);
    next = next % (e->data_sz / ENV_CHUNK_SZ);  /* wrap */

    float reward = _reward(next, e->data, e->data_sz);
    e->steps++;
    e->hits[action & 3]++;

    /* done conditions */
    uint64_t offset = (next * ENV_CHUNK_SZ) % (e->data_sz - ENV_CHUNK_SZ);
    uint8_t ip = skel_isect_pop(e->data + offset);
    int done = (ip == 0) || (e->steps >= e->max_steps);
    if (done) e->zone_resets++;

    e->addr = next;

    out[0] = (uint32_t)(next & 0xFFFFFFFFu);
    out[1] = (uint32_t)(next >> 32);
    uint32_t rbits; memcpy(&rbits, &reward, 4);
    out[2] = rbits;
    out[3] = (uint32_t)done;
}

/* Observation: decode addr → 8 floats (zone/pair/pole/isect/frame/cell/enc/steps) */
void pogls_env_obs(PoglsEnv *e, float *obs8) {
    SkeletonIdx sk    = skeleton_lookup(e->addr);
    PentagonAnchor a  = rj_anchor_lookup(e->addr);
    uint64_t offset   = (e->addr * ENV_CHUNK_SZ) % (e->data_sz - ENV_CHUNK_SZ);
    uint8_t ip        = skel_isect_pop(e->data + offset);

    obs8[0] = (float)sk.zone   / 12.0f;
    obs8[1] = (float)sk.pair   / 6.0f;
    obs8[2] = (float)sk.pole;
    obs8[3] = (float)ip        / 64.0f;
    obs8[4] = (float)a.frame_rubik / 32.0f;
    obs8[5] = (float)a.cell_icosa / 10.0f;
    obs8[6] = (float)sk.enc    / 720.0f;
    obs8[7] = (float)e->steps  / (float)e->max_steps;
}

/* Stats */
void pogls_env_stats(PoglsEnv *e, uint32_t *out4) {
    out4[0] = e->steps;
    out4[1] = e->zone_resets;
    out4[2] = e->hits[1] + e->hits[2];  /* jump count */
    out4[3] = e->hits[0];               /* walk count */
}

/* Verify constants */
int pogls_env_verify(void) {
    if (rj_verify() != 0) return -1;
    if (ENV_CHUNK_SZ != SKEL_CHUNK) return -2;
    return 0;
}

/* ══════════════════════════════════════════════════════════
   BATCH API — n envs stepped in 1 C call
   out: float32 array [n × 11] per env:
     [0..7]  = obs (8 floats)
     [8]     = reward
     [9]     = done
     [10]    = addr_lo (for info)
   actions: int32[n]
   ══════════════════════════════════════════════════════════ */

void pogls_env_step_batch(PoglsEnv **envs, const int *actions,
                           int n, float *out) {
    for (int i = 0; i < n; i++) {
        PoglsEnv *e = envs[i];
        uint32_t tmp[4];
        pogls_env_step(e, actions[i], tmp);

        float reward;
        memcpy(&reward, &tmp[2], 4);
        int   done = (int)tmp[3];

        float obs8[8];
        pogls_env_obs(e, obs8);

        float *dst = out + i * 11;
        memcpy(dst, obs8, 8 * sizeof(float));
        dst[8]  = reward;
        dst[9]  = (float)done;
        dst[10] = (float)(tmp[0]);   /* addr_lo */

        if (done) pogls_env_reset(e);
    }
}

/* Reset batch */
void pogls_env_reset_batch(PoglsEnv **envs, int n, float *out_obs) {
    for (int i = 0; i < n; i++) {
        pogls_env_reset(envs[i]);
        pogls_env_obs(envs[i], out_obs + i * 8);
    }
}

/* Create n envs at once — shared data, independent state */
void pogls_env_create_batch(PoglsEnv **out_envs, int n,
                             uint8_t *data, uint32_t data_sz,
                             uint32_t max_steps) {
    for (int i = 0; i < n; i++) {
        out_envs[i] = pogls_env_create(data, data_sz, max_steps);
        /* each env starts at different addr offset */
        if (out_envs[i]) out_envs[i]->addr = (uint64_t)i * 1728u;
    }
}

void pogls_env_free_batch(PoglsEnv **envs, int n) {
    for (int i = 0; i < n; i++) { pogls_env_free(envs[i]); envs[i] = NULL; }
}
