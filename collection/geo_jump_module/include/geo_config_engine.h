#pragma once

#define GEO_SPOKES          6u
#define GEO_FACES           9u
#define GEO_FACE_UNITS      64u
#define GEO_SLOTS           576u
#define GEO_FULL_N          3456u
#define GEO_OUTER_SLOTS     512u
#define GEO_CENTER_BASE     512u
#define GEO_SIDE_FULL       54u

#define GEO_HILBERT_N       576u
#define GEO_BLOCK_BOUNDARY  288u
#define GEO_GROUP_SIZE      8u
#define GEO_BUNDLE_WORDS    9u
#define GEO_PHASE_COUNT     4u

#define GEO_TE_CYCLE        144u
#define GEO_TE_FULL_CYCLES  24u
#define GEO_TE_SNAPS        6u

#define GEO_HOT_THRESH      64u
#define GEO_IMBAL_THRESH    72u
#define GEO_ANOMALY_HOT     96u

#if (GEO_SPOKES * GEO_SLOTS != GEO_FULL_N)
#  error "GEO: SPOKES * SLOTS != FULL_N"
#endif
#if (GEO_TE_CYCLE * GEO_TE_FULL_CYCLES != GEO_FULL_N)
#  error "GEO: TE_CYCLE * TE_FULL_CYCLES != FULL_N"
#endif
#if (GEO_FACES * GEO_FACE_UNITS != GEO_SLOTS)
#  error "GEO: FACES * FACE_UNITS != SLOTS"
#endif
#if (GEO_BLOCK_BOUNDARY * 2 != GEO_SLOTS)
#  error "GEO: BLOCK_BOUNDARY*2 != SLOTS"
#endif
