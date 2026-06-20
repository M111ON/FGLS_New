#ifndef GEAR_LOCK_H
#define GEAR_LOCK_H

#include <stdint.h>
#include <string.h>
#include <math.h>

/*
 * gear_lock.h — Icosa lane feedback for dynamic SID swap scheduling
 *
 * Tracks per-tensor icosa lane route history across decode cycles.
 * Computes:
 *   stability  — how consistent the route is (0=chaotic 1=locked)
 *   priority   — stability × (1 - boundary_fraction) for swap scheduling
 *
 * High priority = tensor is in stable icosa lane flow → prime SID target.
 * Low priority = tensor hitting boundaries → reduce or delay SID injection.
 *
 * The "gear lock" metaphor: when the icosa lane gear is locked (stable route),
 * power transfers efficiently. When it slips (route changes/boundary), ease off.
 */

#define GL_HISTORY_DEPTH  8
#define GL_MAX_TENSORS    4096
#define GL_DEFAULT_THRESHOLD 0.30f

typedef struct {
    uint64_t routes[GL_HISTORY_DEPTH];
    uint8_t  samples;          /* how many samples collected (capped at GL_HISTORY_DEPTH) */
    uint8_t  boundaries;       /* boundary hit count in window */
    float    stability;        /* 0=route chaotic 1=route locked */
    float    priority;         /* composite score for swap scheduling */
} GLTensor;

typedef struct {
    GLTensor tensors[GL_MAX_TENSORS];
    int      cycles;           /* total icosa lane cycles processed */
    int      n_active;         /* count of active (nonzero samples) tensors */
} GearLockState;

static inline void gear_lock_init(GearLockState *gs) {
    memset(gs, 0, sizeof(*gs));
}

static inline int gl_popcnt64(uint64_t x) {
#if defined(_MSC_VER)
    return (int)__popcnt64(x);
#elif defined(__GNUC__) || defined(__clang__)
    return (int)__builtin_popcountll(x);
#else
    /* portable fallback */
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
#endif
}

/* Update gear lock with one cycle of icosa lane output (routes + events).
 * ft_indices[i] maps output slot i → found_tensors index (for state tracking). */
static inline void gear_lock_update(GearLockState *gs,
    const uint64_t *routes, const uint8_t *events, int n,
    const int *ft_indices)
{
    if (!gs || !routes || !events || n <= 0) return;

    gs->cycles++;

    for (int i = 0; i < n; i++) {
        int ti = ft_indices[i];
        if (ti < 0 || ti >= GL_MAX_TENSORS) continue;

        GLTensor *t = &gs->tensors[ti];
        int pos = t->samples % GL_HISTORY_DEPTH;

        t->routes[pos] = routes[i];

        if (events[i] & 0x02) {
            if (t->boundaries < 255) t->boundaries++;
        }

        if (t->samples < GL_HISTORY_DEPTH) t->samples++;

        /* compute stability from consecutive pairwise XOR popcount */
        if (t->samples >= 2) {
            int n_valid = t->samples;
            int np = n_valid - 1;
            int oldest = n_valid < GL_HISTORY_DEPTH ? 0 : (pos + 1) % GL_HISTORY_DEPTH;
            uint64_t sum = 0;
            for (int j = 0; j < np; j++) {
                int a = (oldest + j) % GL_HISTORY_DEPTH;
                int b = (a + 1) % GL_HISTORY_DEPTH;
                sum += gl_popcnt64(t->routes[a] ^ t->routes[b]);
            }
            float avg = (float)sum / (float)np;
            t->stability = 1.0f - (avg / 64.0f);
            if (t->stability < 0.0f) t->stability = 0.0f;
        }

        /* priority = stability × (1 - boundary_fraction) */
        float br = (float)t->boundaries / (float)(t->samples);
        if (br > 1.0f) br = 1.0f;
        t->priority = t->stability * (1.0f - br);
    }

    /* recount active tensors */
    gs->n_active = 0;
    for (int i = 0; i < GL_MAX_TENSORS; i++) {
        if (gs->tensors[i].samples > 0) gs->n_active++;
    }
}

/* Get priority for a tensor by ft_idx */
static inline float gear_lock_score(const GearLockState *gs, int ft_idx) {
    if (!gs || ft_idx < 0 || ft_idx >= GL_MAX_TENSORS) return 0.0f;
    return gs->tensors[ft_idx].priority;
}

/* Build active mask: 1 for swaps whose tensor meets priority threshold.
 * Returns number of active swaps. */
static inline int gear_lock_build_mask(const GearLockState *gs,
    int n_swaps, const int *ft_indices,
    uint8_t *mask_out, float threshold)
{
    if (!gs || !ft_indices || !mask_out || n_swaps <= 0) return 0;

    int n = 0;
    for (int i = 0; i < n_swaps; i++) {
        float s = gear_lock_score(gs, ft_indices[i]);
        mask_out[i] = (s >= threshold) ? 1 : 0;
        if (mask_out[i]) n++;
    }
    return n;
}

/* Print gear lock summary (top-N tensors by priority) */
static inline void gear_lock_print_summary(const GearLockState *gs,
    const char *const *names, int n_names, int top_n)
{
    if (!gs || !names) return;

    /* collect indices with samples */
    int indices[GL_MAX_TENSORS];
    int n_indices = 0;
    for (int i = 0; i < GL_MAX_TENSORS && i < n_names; i++) {
        if (gs->tensors[i].samples > 0) {
            indices[n_indices++] = i;
        }
    }

    if (n_indices == 0) {
        printf("[gear] no active tensors\n");
        return;
    }

    /* compute stats */
    float avg_priority = 0, avg_stability = 0;
    int total_boundaries = 0;
    for (int i = 0; i < n_indices; i++) {
        int ti = indices[i];
        avg_priority += gs->tensors[ti].priority;
        avg_stability += gs->tensors[ti].stability;
        total_boundaries += gs->tensors[ti].boundaries;
    }
    avg_priority /= (float)n_indices;
    avg_stability /= (float)n_indices;

    printf("[gear] cycles=%d active=%d avg_stability=%.2f avg_priority=%.2f boundaries=%d\n",
        gs->cycles, gs->n_active, avg_stability, avg_priority, total_boundaries);

    /* find top-N by priority (simple O(n*top) selection) */
    if (top_n <= 0) top_n = 5;
    if (top_n > n_indices) top_n = n_indices;

    int top_idx[16];
    float top_val[16];
    int n_top = top_n > 16 ? 16 : top_n;

    for (int i = 0; i < n_top; i++) {
        top_idx[i] = -1;
        top_val[i] = -1.0f;
    }

    for (int i = 0; i < n_indices; i++) {
        int ti = indices[i];
        float p = gs->tensors[ti].priority;
        for (int j = 0; j < n_top; j++) {
            if (p > top_val[j]) {
                for (int k = n_top - 1; k > j; k--) {
                    top_idx[k] = top_idx[k-1];
                    top_val[k] = top_val[k-1];
                }
                top_idx[j] = ti;
                top_val[j] = p;
                break;
            }
        }
    }

    printf("[gear] top-%d by priority:\n", n_top);
    for (int i = 0; i < n_top; i++) {
        if (top_idx[i] < 0) break;
        int ti = top_idx[i];
        printf("  %.2f  stab=%.2f  bnd=%d  %s\n",
            gs->tensors[ti].priority,
            gs->tensors[ti].stability,
            (int)gs->tensors[ti].boundaries,
            (top_idx[i] < n_names && names[top_idx[i]]) ? names[top_idx[i]] : "?");
    }
}

#endif /* GEAR_LOCK_H */
