#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "geo_config.h"

#define TE_CYCLE        GEO_TE_CYCLE
#define TE_MAX_SNAP     GEO_TE_SNAPS
#define TE_FULL_CYCLES  GEO_TE_FULL_CYCLES
#define TE_FULL_OPS     GEO_FULL_N

#define QRPN_NORMAL     0
#define QRPN_STRESSED   1
#define QRPN_ANOMALY    2

#define QRPN_HOT_THRESH     GEO_HOT_THRESH
#define QRPN_IMBAL_THRESH   GEO_IMBAL_THRESH
#define QRPN_ANOMALY_HOT    GEO_ANOMALY_HOT

typedef struct { uint64_t gen2; uint64_t gen3; } GeoSeed;

typedef struct {
    uint16_t spoke_count[6];
    uint16_t hot_slots;
    uint8_t  qrpn_state;
    uint8_t  _pad;
} GeoSnap;

typedef struct {
    GeoSeed  genesis;
    GeoSeed  ring[TE_MAX_SNAP];
    GeoSnap  snap[TE_MAX_SNAP];
    GeoSnap  cur;
    uint32_t op_count;
    uint8_t  head;
    uint8_t  count;
    uint8_t  qrpn_state;
    uint8_t  _pad;
} ThirdEye;

static inline void te_init(ThirdEye *te, GeoSeed g) {
    memset(te, 0, sizeof(ThirdEye));
    te->genesis    = g;
    te->qrpn_state = QRPN_NORMAL;
}

static inline uint8_t te_mirror_mask(uint8_t spoke, uint8_t state) {
    if (state == QRPN_ANOMALY) return 0x3F;

    uint8_t mask = 0;
    mask |= (uint8_t)(1u << ((spoke + 1) % 6));
    mask |= (uint8_t)(1u << ((spoke + 5) % 6));

    if (state >= QRPN_STRESSED)
        mask |= (uint8_t)(1u << ((spoke + 3) % 6));

    return mask;
}

static inline uint8_t te_eval_state(const GeoSnap *s) {
    uint8_t imbal = 0;
    for (int i = 0; i < 3; i++) {
        int diff = (int)s->spoke_count[i] - (int)s->spoke_count[i+3];
        if (diff < 0) diff = -diff;
        if (diff > (int)QRPN_IMBAL_THRESH) imbal = 1;
    }
    uint8_t hot      = (s->hot_slots > QRPN_HOT_THRESH);
    uint8_t very_hot = (s->hot_slots > QRPN_ANOMALY_HOT);

    if (imbal || very_hot) return QRPN_ANOMALY;
    if (hot) return QRPN_STRESSED;
    return QRPN_NORMAL;
}

static inline void te_tick(ThirdEye *te, GeoSeed cur, uint8_t spoke, uint8_t slot_hot,
                           uint32_t val_drift) {
    te->op_count++;
    te->cur.spoke_count[spoke & 0x7]++;
    if (slot_hot) te->cur.hot_slots++;
    if (val_drift > 2u) {
        uint16_t w = (uint16_t)(val_drift >> 1);
        te->cur.hot_slots = (te->cur.hot_slots + w > 0xFFFFu)
                            ? 0xFFFFu
                            : (uint16_t)(te->cur.hot_slots + w);
    }

    if (te->op_count % TE_CYCLE == 0) {
        te->cur.qrpn_state = te_eval_state(&te->cur);
        te->qrpn_state     = te->cur.qrpn_state;

        te->head = (te->head + 1) % TE_MAX_SNAP;
        te->ring[te->head] = cur;
        te->snap[te->head] = te->cur;
        if (te->count < TE_MAX_SNAP) te->count++;

        memset(&te->cur, 0, sizeof(GeoSnap));
    }
}

static inline uint8_t te_get_mask(const ThirdEye *te, uint8_t spoke) {
    return te_mirror_mask(spoke, te->qrpn_state);
}

static inline GeoSeed te_reset(ThirdEye *te) {
    memset(te->ring, 0, sizeof(te->ring));
    memset(te->snap, 0, sizeof(te->snap));
    memset(&te->cur,  0, sizeof(GeoSnap));
    te->op_count   = 0;
    te->head       = 0;
    te->count      = 0;
    te->qrpn_state = QRPN_NORMAL;
    return te->genesis;
}

static inline GeoSeed te_rewind(ThirdEye *te, int n) {
    if (n <= 0 || te->count == 0) return te->genesis;
    if (n >= te->count)           return te->genesis;
    int slot = ((int)te->head - n + TE_MAX_SNAP) % TE_MAX_SNAP;
    return te->ring[slot];
}

static inline const char* te_state_name(uint8_t s) {
    if (s == QRPN_ANOMALY)  return "ANOMALY";
    if (s == QRPN_STRESSED) return "STRESSED";
    return "NORMAL";
}

static inline void te_status(const ThirdEye *te) {
    printf("[ThirdEye] op=%u  next=%u  ring=%d/%d  head=%d\n",
           te->op_count,
           TE_CYCLE - (te->op_count % TE_CYCLE),
           te->count, TE_MAX_SNAP, te->head);
    printf("  QRPN state : %s\n", te_state_name(te->qrpn_state));
    printf("  cycle prog : %d/%d (full=%d)\n",
           te->op_count / TE_CYCLE, TE_FULL_CYCLES, TE_FULL_OPS);

    if (te->count > 0) {
        const GeoSnap *last = &te->snap[te->head];
        printf("  last snap  : spoke[");
        for (int i = 0; i < 6; i++)
            printf("%d%s", last->spoke_count[i], i<5?",":"]");
        printf("  hot=%d\n", last->hot_slots);

        printf("  A↔B balance: ");
        for (int i = 0; i < 3; i++) {
            int d = (int)last->spoke_count[i] - (int)last->spoke_count[i+3];
            if (d<0) d=-d;
            printf("%d↔%d|diff=%d%s  ", i, i+3, d,
                   d>(int)QRPN_IMBAL_THRESH?"!":"✓");
        }
        printf("\n");
    }
}
