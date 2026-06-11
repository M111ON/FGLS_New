/*
 * fibo_spine.h — Async FiboSpine Tick System
 * each spine runs at own Fib(n) rate, no global sync
 * convergence = LCM point = CTD anchor meeting
 *
 * Rules:
 *   - No malloc, no global state
 *   - O(1) everything
 *   - gcd(Fib(n),Fib(n+1))=1 -> LCM = product (no deadlock)
 */

#ifndef FIBO_SPINE_H
#define FIBO_SPINE_H

#include <stdint.h>
#include "ctd_tri.h"

static const uint32_t FIBO_LUT[16] = {
    1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610, 987
};

typedef struct {
    uint32_t tick;
    uint32_t rate;
    uint8_t  fib_idx;
    uint8_t  chirality;
    uint8_t  face;
    uint8_t  active;
} FiboSpine;

typedef struct {
    uint32_t lcm_period;
    uint32_t tick_at;
    uint8_t  face_a;
    uint8_t  face_b;
    uint8_t  _pad[2];
} SpineConvergence;

static inline FiboSpine fibo_spine_init(const CTDCard *card, uint8_t fib_idx)
{
    FiboSpine s;
    s.tick = 0;
    s.fib_idx = fib_idx < 16 ? fib_idx : 15;
    s.rate = FIBO_LUT[s.fib_idx];
    s.chirality = card->chirality;
    s.face = card->dodeca_face;
    s.active = 1;
    return s;
}

static inline int fibo_spine_step(FiboSpine *s)
{
    if (!s->active) return 0;
    s->tick++;
    return (s->tick % s->rate == 0) ? 1 : 0;
}

static inline int fibo_spine_converge(const FiboSpine *a, const FiboSpine *b, SpineConvergence *out)
{
    uint32_t lcm = a->rate * b->rate;
    int hit = (a->tick % lcm == 0) && (b->tick % lcm == 0) && (a->tick > 0);
    if (hit && out) {
        out->lcm_period = lcm;
        out->tick_at = a->tick;
        out->face_a = a->face;
        out->face_b = b->face;
        out->_pad[0] = 0;
        out->_pad[1] = 0;
    }
    return hit;
}

typedef struct {
    FiboSpine L;
    FiboSpine R;
} HerringbonePair;

static inline HerringbonePair hbone_init(const CTDCard *card, uint8_t base_fib_idx)
{
    HerringbonePair hb;
    CTDCard cl = *card; cl.chirality = CTD_CHIRAL_L;
    CTDCard cr = *card; cr.chirality = CTD_CHIRAL_R;
    uint8_t ni = base_fib_idx < 15 ? base_fib_idx + 1 : 15;
    hb.L = fibo_spine_init(&cl, base_fib_idx);
    hb.R = fibo_spine_init(&cr, ni);
    return hb;
}

static inline int hbone_step(HerringbonePair *hb, SpineConvergence *out)
{
    fibo_spine_step(&hb->L);
    fibo_spine_step(&hb->R);
    return fibo_spine_converge(&hb->L, &hb->R, out);
}

/* P5H Pipe Domain Integration (opt-in via -DP5H_ENABLE) */
#ifdef P5H_ENABLE

#include "p5h_ribcage.h"

typedef struct {
    HerringbonePair hb;
    P5HField        field;
    uint32_t        global_tick;
} P5HHerringbone;

static inline P5HHerringbone p5h_hbone_init(const CTDCard *card, uint8_t base_fib_idx)
{
    P5HHerringbone ph;
    ph.hb = hbone_init(card, base_fib_idx);
    p5h_field_init(&ph.field);
    ph.global_tick = 0;
    return ph;
}

static inline int p5h_hbone_step(P5HHerringbone *ph, SpineConvergence *out)
{
    ph->global_tick++;
    hbone_step(&ph->hb, out);
    p5h_field_observe(&ph->field, (uint16_t)(ph->global_tick % 20736));
    return p5h_is_barrier(&ph->field);
}

static inline int p5h_hbone_step_textured(P5HHerringbone *ph, SpineConvergence *out)
{
    int barrier = p5h_hbone_step(ph, out);
    P5HField *f = &ph->field;
    uint32_t fid = f->flower_now;
    f->flowers[fid].branch_id = (uint16_t)(ph->hb.L.face + 1);
    if (barrier) {
        f->flowers[fid].resolved = 1;
    }
    return barrier;
}

static inline P5HFlower *p5h_hbone_peek(P5HHerringbone *ph, uint32_t flower_id)
{
    if (p5h_is_barrier(&ph->field)) return (P5HFlower *)0;
    return p5h_field_peek(&ph->field, flower_id);
}

#endif /* P5H_ENABLE */

#endif /* FIBO_SPINE_H */
