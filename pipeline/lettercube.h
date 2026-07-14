/*
 * lettercube.h — 24-pair face:face bond for 6-lane frustum assembly
 *
 * Concept:
 *   - 24 pairs = pool (dual dodeca 24 pentagon faces)
 *   - 6 lanes = frustum faces (6! window in 24! pool)
 *   - Each lane has a pair identity (0-23)
 *   - Two lanes bond when pair_ids satisfy complement condition
 *   - Bond = face:face connection (like Tetris bond, larger space)
 *
 * Architecture:
 *   lane[0..5] → each has pair_id[0..23] + angle[0..5]
 *   bond(lane_i, lane_j) → complement(pair_id[i], pair_id[j], angle)
 *   lock → bond verified (angle XOR + pair match)
 *
 * Pool = 24! permutations
 * Working window = 6! (6 lanes at a time)
 */
#ifndef LETTERCUBE_H
#define LETTERCUBE_H

#include <stdint.h>
#include <string.h>

#define LC_PAIRS       24    /* dual dodeca pentagon faces */
#define LC_LANES       6     /* frustum faces (6 directions) */
#define LC_SIDES       6     /* cube sides */

/* Bond states */
#define LC_BOND_FREE   0
#define LC_BOND_PEND   1     /* candidate, not yet verified */
#define LC_BOND_LOCK   2     /* locked (verified) */

/* Complement table: pair_id → complement pair_id */
/* For dual dodeca: opposite faces share complement relationship */
static const uint8_t LC_COMPLEMENT[LC_PAIRS] = {
    12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11
};

/* Angle XOR table: 6 rotations × 6 rotations → 6-bit mask */
/* Two faces match if (angle_i XOR angle_j) satisfies threshold */
#define LC_ANGLE_MATCH(a, b, threshold)  (((a) ^ (b)) <= (threshold))

/* ── Data Structures ──────────────────────────────────────────── */

typedef struct {
    uint8_t pair_id;        /* 0-23: which pair from pool */
    uint8_t angle;          /* 0-5: rotation angle */
    uint8_t bond_state;     /* LC_BOND_FREE/PEND/LOCK */
    uint8_t bonded_to;      /* lane index this lane is bonded to (0-5 or 0xFF) */
    uint8_t core_seed[8];   /* geometric seed for this lane */
} LCLane;

typedef struct {
    LCLane lanes[LC_LANES]; /* 6 lanes */
    uint8_t n_locked;       /* number of locked bonds */
    uint8_t depth;          /* nesting depth (0 = base) */
    uint8_t parent_idx;     /* parent cube index (0xFF = root) */
    uint8_t reserved[3];
} LetterCube;

/* ── Core Functions ───────────────────────────────────────────── */

/*
 * lc_init: Initialize a fresh LetterCube with 24-pair pool.
 * Assigns pair_ids 0-5 to lanes 0-5 (default starting positions).
 * All angles = 0, all bonds = FREE.
 */
static inline void lc_init(LetterCube *cube)
{
    memset(cube, 0, sizeof(LetterCube));
    for (int i = 0; i < LC_LANES; i++) {
        cube->lanes[i].pair_id = (uint8_t)i;  /* default: lane i gets pair i */
        cube->lanes[i].angle = 0;
        cube->lanes[i].bond_state = LC_BOND_FREE;
        cube->lanes[i].bonded_to = 0xFF;
        memset(cube->lanes[i].core_seed, 0, 8);
    }
    cube->n_locked = 0;
    cube->depth = 0;
    cube->parent_idx = 0xFF;
}

/*
 * lc_assign_lane: Assign a pair_id from the 24-pool to a lane.
 * The pair_id determines the lane's identity in the frustum.
 */
static inline void lc_assign_lane(LetterCube *cube, uint8_t lane, uint8_t pair_id, uint8_t angle)
{
    if (lane >= LC_LANES || pair_id >= LC_PAIRS) return;
    cube->lanes[lane].pair_id = pair_id;
    cube->lanes[lane].angle = angle;
}

/*
 * lc_check_complement: Check if two lanes can bond.
 * Bond condition: pair_ids are complementary AND angles are compatible.
 * Returns 1 if bond is possible.
 */
static inline int lc_check_complement(const LetterCube *cube, uint8_t lane_a, uint8_t lane_b)
{
    if (lane_a >= LC_LANES || lane_b >= LC_LANES) return 0;
    uint8_t pa = cube->lanes[lane_a].pair_id;
    uint8_t pb = cube->lanes[lane_b].pair_id;
    uint8_t aa = cube->lanes[lane_a].angle;
    uint8_t ab = cube->lanes[lane_b].angle;

    /* Complement check: pa's complement == pb */
    if (LC_COMPLEMENT[pa] != pb) return 0;

    /* Angle check: XOR within threshold (threshold=2 for 6-lane system) */
    if (!LC_ANGLE_MATCH(aa, ab, 2)) return 0;

    return 1;
}

/*
 * lc_bond: Attempt to bond two lanes (face:face connection).
 * Sets bond_state to PEND if complement check passes.
 * Returns 1 if bond candidate accepted, 0 if rejected.
 */
static inline int lc_bond(LetterCube *cube, uint8_t lane_a, uint8_t lane_b)
{
    if (lane_a >= LC_LANES || lane_b >= LC_LANES) return 0;
    if (lane_a == lane_b) return 0;
    if (cube->lanes[lane_a].bond_state != LC_BOND_FREE) return 0;
    if (cube->lanes[lane_b].bond_state != LC_BOND_FREE) return 0;

    if (!lc_check_complement(cube, lane_a, lane_b)) return 0;

    cube->lanes[lane_a].bond_state = LC_BOND_PEND;
    cube->lanes[lane_a].bonded_to = lane_b;
    cube->lanes[lane_b].bond_state = LC_BOND_PEND;
    cube->lanes[lane_b].bonded_to = lane_a;

    return 1;
}

/*
 * lc_lock: Lock a pending bond (verified).
 * Only bonds in PEND state can be locked.
 * Returns 1 if locked, 0 if not pend.
 */
static inline int lc_lock(LetterCube *cube, uint8_t lane_a, uint8_t lane_b)
{
    if (lane_a >= LC_LANES || lane_b >= LC_LANES) return 0;
    if (cube->lanes[lane_a].bond_state != LC_BOND_PEND) return 0;
    if (cube->lanes[lane_b].bond_state != LC_BOND_PEND) return 0;
    if (cube->lanes[lane_a].bonded_to != lane_b) return 0;

    cube->lanes[lane_a].bond_state = LC_BOND_LOCK;
    cube->lanes[lane_b].bond_state = LC_BOND_LOCK;
    cube->n_locked++;

    return 1;
}

/*
 * lc_force_match: Scan all free lanes and bond matching pairs.
 * This is the greedy matching pass — bond everything that fits.
 * Returns number of new bonds formed.
 */
static inline int lc_force_match(LetterCube *cube)
{
    int new_bonds = 0;
    for (int i = 0; i < LC_LANES; i++) {
        if (cube->lanes[i].bond_state != LC_BOND_FREE) continue;
        for (int j = i + 1; j < LC_LANES; j++) {
            if (cube->lanes[j].bond_state != LC_BOND_FREE) continue;
            if (lc_bond(cube, (uint8_t)i, (uint8_t)j)) {
                new_bonds++;
            }
        }
    }
    return new_bonds;
}

/*
 * lc_lock_all: Lock all pending bonds.
 * Returns number of bonds locked.
 */
static inline int lc_lock_all(LetterCube *cube)
{
    int locked = 0;
    for (int i = 0; i < LC_LANES; i++) {
        if (cube->lanes[i].bond_state != LC_BOND_PEND) continue;
        int j = cube->lanes[i].bonded_to;
        if (j >= 0 && j < LC_LANES && i < j) {
            if (lc_lock(cube, (uint8_t)i, (uint8_t)j)) {
                locked++;
            }
        }
    }
    return locked;
}

/*
 * lc_verify: Verify all bonds are consistent.
 * Returns 1 if cube is fully bonded and consistent, 0 otherwise.
 */
static inline int lc_verify(const LetterCube *cube)
{
    for (int i = 0; i < LC_LANES; i++) {
        uint8_t state = cube->lanes[i].bond_state;
        uint8_t partner = cube->lanes[i].bonded_to;

        if (state == LC_BOND_FREE) return 0;  /* all lanes must be bonded */
        if (partner >= LC_LANES) return 0;

        /* Verify bidirectional */
        if (cube->lanes[partner].bonded_to != (uint8_t)i) return 0;
        if (cube->lanes[partner].bond_state != state) return 0;

        /* Verify complement still holds */
        if (!lc_check_complement(cube, (uint8_t)i, partner)) return 0;
    }
    return 1;
}

/*
 * lc_assemble: Full assembly pipeline — match, lock, verify.
 * Returns 1 if successful, 0 if failed.
 */
static inline int lc_assemble(LetterCube *cube)
{
    lc_force_match(cube);
    int locked = lc_lock_all(cube);
    (void)locked;
    return lc_verify(cube);
}

/*
 * lcSerialize: Pack LetterCube into a byte buffer.
 * Size: 6 lanes × (1+1+1+1+8) bytes + 4 metadata bytes = 76 bytes.
 */
#define LC_SERIALIZED_SZ  (LC_LANES * 12 + 4)   /* 76 bytes */

static inline void lc_serialize(const LetterCube *cube, uint8_t *buf)
{
    int pos = 0;
    for (int i = 0; i < LC_LANES; i++) {
        buf[pos++] = cube->lanes[i].pair_id;
        buf[pos++] = cube->lanes[i].angle;
        buf[pos++] = cube->lanes[i].bond_state;
        buf[pos++] = cube->lanes[i].bonded_to;
        memcpy(buf + pos, cube->lanes[i].core_seed, 8);
        pos += 8;
    }
    buf[pos++] = cube->n_locked;
    buf[pos++] = cube->depth;
    buf[pos++] = cube->parent_idx;
    buf[pos++] = 0;  /* reserved */
}

/*
 * lc_deserialize: Unpack LetterCube from byte buffer.
 */
static inline void lc_deserialize(LetterCube *cube, const uint8_t *buf)
{
    int pos = 0;
    for (int i = 0; i < LC_LANES; i++) {
        cube->lanes[i].pair_id = buf[pos++];
        cube->lanes[i].angle = buf[pos++];
        cube->lanes[i].bond_state = buf[pos++];
        cube->lanes[i].bonded_to = buf[pos++];
        memcpy(cube->lanes[i].core_seed, buf + pos, 8);
        pos += 8;
    }
    cube->n_locked = buf[pos++];
    cube->depth = buf[pos++];
    cube->parent_idx = buf[pos++];
    pos++;  /* reserved */
}

/*
 * lc_print: Debug print of LetterCube state.
 */
static inline void lc_print(const LetterCube *cube)
{
    for (int i = 0; i < LC_LANES; i++) {
        const char *state_str = "FREE";
        if (cube->lanes[i].bond_state == LC_BOND_PEND) state_str = "PEND";
        if (cube->lanes[i].bond_state == LC_BOND_LOCK) state_str = "LOCK";
    }
}

#endif /* LETTERCUBE_H */
