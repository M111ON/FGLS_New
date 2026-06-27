/*
 * geo_config.h — Geomatrix Single Source of Truth
 * ═══════════════════════════════════════════════
 * ALL geometry constants live here.
 * Every other file #include this — never redefine.
 *
 * Number chain:
 *   6 × 9 × 64 = 3456 = 144 × 24
 *   576 = 24² = 9 × 64
 *   288 = 576/2 = 2 × 144
 *   digit_sum: 3456→9  576→9  288→9  144→9  54→9
 */

#ifndef GEO_CONFIG_H
#define GEO_CONFIG_H

#include "coord_spine.h"    /* GEO_* constants (single source of truth) */

#endif /* GEO_CONFIG_H */
