#pragma once
#include "theta_map.h"
#include "geo_dodeca_torus.h"

static inline TorusNode geo_route_init(ThetaCoord tc) {
    TorusNode n;
    n.face  = tc.face;
    n.edge  = tc.edge;
    n.z     = tc.z;
    n.state = 0;
    return n;
}

static inline TorusNode geo_route_from_raw(uint64_t raw) {
    return geo_route_init(theta_map(raw));
}

static inline TorusNode geo_route_advance(TorusNode n, int steps) {
    for (int i = 0; i < steps; i++)
        torus_step(&n);
    return n;
}

static inline uint64_t geo_route_pack(const TorusNode *n) {
    return ((uint64_t)n->face  << 56)
         | ((uint64_t)n->edge  << 48)
         | ((uint64_t)n->z     << 40);
}

static inline void geo_route_batch(const uint64_t *in, TorusNode *out, int n) {
    for (int i = 0; i < n; i++)
        out[i] = geo_route_from_raw(in[i]);
}
