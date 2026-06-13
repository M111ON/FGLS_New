/*
 * tw_capture_int.h — Integer-Only Hierarchical Sparse Triwheel
 *
 * SCALE = 12^4 * 10 = 207360  (dodecahedron-aligned fixed-point unit)
 * All coordinates, residuals, and angle tests are int32/int64.
 * No float, no trig at runtime. Pure integer relationships (POGLS rule).
 *
 * 207360 = 288 * 720 (TRing). Divisible by TRing=720, SHELL_TOTAL=288,
 * DODECA_FACES=12, TW_SLOTS=60, GEO_FULL/6=3456. Clean alignment.
 * 12^4 structure matches dodeca geometry.
 *
 * Geometry: 1 base edge + {36deg, 60deg} rules
 *   10 sectors (pentagon-pair, parent, 36deg each)
 *   each sector -> 6 child slots (hex-cast, 60deg) = 60 total
 *
 * Sector lookup: cross-product sign test against 10 boundary direction
 * vectors (no atan2). O(10) sign tests -> primary sector.
 *
 * Drain/bundle: margin band near a sector boundary activates BOTH
 * adjacent sectors. Margin expressed as integer cross-product threshold
 * (proportional to |v|, scale-invariant via squared-magnitude compare).
 *
 * Lossless: residual = v - slot_centroid, exact int64 subtraction.
 */

#ifndef TW_CAPTURE_INT_H
#define TW_CAPTURE_INT_H

#include <stdint.h>

#define TW_SCALE       207360   /* 12^4 * 10 */
#define TW_N_SECTORS   10
#define TW_SLOTS_PER   6
#define TW_N_SLOTS     60

/* margin band as permille of |v| cross-product magnitude (~0.5deg ~ 8.7e-3 rad
 * -> sin(0.5deg)=0.00873 ; use 9/1000 as integer ratio (9 permille) */
#define TW_MARGIN_NUM  9
#define TW_MARGIN_DEN  1000

/* Unit boundary direction vectors * TW_SCALE, at angles (90 - 36*k) deg */
static const int32_t TW_BOUNDARY_DIR[TW_N_SECTORS][2] = {
    {      0, 207360},
    { 121883, 167758},
    { 197211,  64078},
    { 197211, -64078},
    { 121883,-167758},
    {      0,-207360},
    {-121883,-167758},
    {-197211, -64078},
    {-197211,  64078},
    {-121883, 167758},
};

/* 6 slot centroids per sector, int32 * TW_SCALE */
static const int32_t TW_SLOT_LOCAL_I[TW_N_SECTORS][TW_SLOTS_PER][2] = {
  {{      0, 238464},{ -26937, 222912},{ -26937, 191808},{      0, 176256},{  26937, 191808},{  26937, 222912}},
  {{ 140166, 192922},{ 109232, 196172},{  90949, 171009},{ 103601, 142594},{ 134534, 139342},{ 152817, 164507}},
  {{ 226792,  73689},{ 203678,  94502},{ 174097,  84890},{ 167629,  54466},{ 190744,  33653},{ 220326,  43265}},
  {{ 226792, -73689},{ 220326, -43265},{ 190744, -33653},{ 167629, -54466},{ 174097, -84890},{ 203678, -94502}},
  {{ 140166,-192922},{ 152817,-164507},{ 134534,-139342},{ 103601,-142594},{  90949,-171009},{ 109232,-196172}},
  {{      0,-238464},{  26937,-222912},{  26937,-191808},{      0,-176256},{ -26937,-191808},{ -26937,-222912}},
  {{-140166,-192922},{-109232,-196172},{ -90949,-171009},{-103601,-142594},{-134534,-139342},{-152817,-164507}},
  {{-226792, -73689},{-203678, -94502},{-174097, -84890},{-167629, -54466},{-190744, -33653},{-220326, -43265}},
  {{-226792,  73689},{-220326,  43265},{-190744,  33653},{-167629,  54466},{-174097,  84890},{-203678,  94502}},
  {{-140166, 192922},{-152817, 164507},{-134534, 139342},{-103601, 142594},{ -90949, 171009},{-109232, 196172}},
};

typedef struct {
    uint8_t zone;
    uint8_t slot;
    int64_t resid_x;
    int64_t resid_y;
    uint8_t drain;
    uint8_t drain_zone;
    uint8_t drain_slot;
    int64_t drain_resid_x;
    int64_t drain_resid_y;
} TWCaptureInt;

/* cross product (z component) of two 2D vectors, int64 to avoid overflow */
static inline int64_t _tw_cross(int32_t ax, int32_t ay, int64_t bx, int64_t by)
{
    return (int64_t)ax*by - (int64_t)ay*bx;
}

static inline int64_t _tw_dot(int32_t ax, int32_t ay, int64_t bx, int64_t by)
{
    return (int64_t)ax*bx + (int64_t)ay*by;
}

/* squared magnitude */
static inline int64_t _tw_mag2(int64_t x, int64_t y)
{
    return x*x + y*y;
}

/* abs for int64 */
static inline int64_t _tw_abs64(int64_t v) { return v < 0 ? -v : v; }

static inline void _tw_pick_slot(int64_t vx, int64_t vy, int sector,
                                  uint8_t *slot_out, int64_t *rx, int64_t *ry)
{
    int best = 0;
    int64_t bd = -1;
    for (int j = 0; j < TW_SLOTS_PER; j++) {
        int64_t dx = vx - TW_SLOT_LOCAL_I[sector][j][0];
        int64_t dy = vy - TW_SLOT_LOCAL_I[sector][j][1];
        int64_t d = dx*dx + dy*dy;
        if (bd < 0 || d < bd) { bd = d; best = j; }
    }
    *slot_out = (uint8_t)(sector*TW_SLOTS_PER + best);
    *rx = vx - TW_SLOT_LOCAL_I[sector][best][0];
    *ry = vy - TW_SLOT_LOCAL_I[sector][best][1];
}

/*
 * Sector lookup via cross-product sign against boundary rays.
 * Boundary ray k separates sector (k-1) and sector k (going clockwise
 * from the +Y axis in the same convention as the original construction).
 * For each k, sign(cross(boundary_k, v)) tells which side v is on.
 * We find the sector by counting how many boundary rays v is "ahead of".
 */
static inline int _tw_find_sector(int64_t vx, int64_t vy, int64_t *out_mincross_abs)
{
    /* signed cross with each boundary direction */
    int64_t cross[TW_N_SECTORS];
    for (int k = 0; k < TW_N_SECTORS; k++)
        cross[k] = _tw_cross(TW_BOUNDARY_DIR[k][0], TW_BOUNDARY_DIR[k][1], vx, vy);

    /* sector k is the region between boundary_k and boundary_{k+1}
     * (going clockwise): v is in sector k if cross(boundary_k, v) <= 0
     * and cross(boundary_{k+1}, v) >= 0  (clockwise winding) */
    int sector = 0;
    int64_t mincross = -1;
    for (int k = 0; k < TW_N_SECTORS; k++) {
        int kn = (k+1) % TW_N_SECTORS;
        if (cross[k] <= 0 && cross[kn] >= 0) {
            sector = k;
        }
        int64_t a = _tw_abs64(cross[k]);
        if (mincross < 0 || a < mincross) mincross = a;
    }
    *out_mincross_abs = mincross;
    return sector;
}

/* Sparse hierarchical capture, integer-only */
static inline void tw_capture_int(int64_t vx, int64_t vy, TWCaptureInt *out)
{
    int64_t mincross;
    int primary = _tw_find_sector(vx, vy, &mincross);

    out->zone = (uint8_t)primary;
    _tw_pick_slot(vx, vy, primary, &out->slot, &out->resid_x, &out->resid_y);

    /* drain test: |cross| / |v||boundary| ~ sin(angle_to_boundary)
     * boundary dirs are unit*TW_SCALE, so |boundary|=TW_SCALE.
     * sin(angle) = |cross| / (|v| * TW_SCALE)
     * drain if sin(angle) < MARGIN_NUM/MARGIN_DEN
     * => |cross| * MARGIN_DEN < |v| * TW_SCALE * MARGIN_NUM  */
    int64_t vmag2 = _tw_mag2(vx, vy);
    /* compare mincross^2 * DEN^2  vs  vmag2 * SCALE^2 * NUM^2  (avoid sqrt) */
    __int128 lhs = (__int128)mincross * mincross * TW_MARGIN_DEN * TW_MARGIN_DEN;
    __int128 rhs = (__int128)vmag2 * TW_SCALE * TW_SCALE * (int64_t)TW_MARGIN_NUM * TW_MARGIN_NUM;

    if (lhs < rhs) {
        /* near boundary -> find neighbor sector with smaller |cross| on the
         * other side; check both neighbors and pick whichever's boundary
         * produced mincross */
        int64_t cross_prev = _tw_cross(TW_BOUNDARY_DIR[primary][0], TW_BOUNDARY_DIR[primary][1], vx, vy);
        int64_t cross_next = _tw_cross(TW_BOUNDARY_DIR[(primary+1)%TW_N_SECTORS][0],
                                        TW_BOUNDARY_DIR[(primary+1)%TW_N_SECTORS][1], vx, vy);
        int secondary;
        if (_tw_abs64(cross_prev) < _tw_abs64(cross_next))
            secondary = (primary - 1 + TW_N_SECTORS) % TW_N_SECTORS;
        else
            secondary = (primary + 1) % TW_N_SECTORS;

        out->drain = 1;
        out->drain_zone = (uint8_t)secondary;
        _tw_pick_slot(vx, vy, secondary, &out->drain_slot, &out->drain_resid_x, &out->drain_resid_y);
    } else {
        out->drain = 0;
        out->drain_zone = 0;
        out->drain_slot = 0;
        out->drain_resid_x = 0;
        out->drain_resid_y = 0;
    }
}

/* Reconstruct from primary half (lossless on its own, exact integers) */
static inline void tw_reconstruct_int(const TWCaptureInt *in, int64_t *vx, int64_t *vy)
{
    int sector = in->zone;
    int local  = in->slot - sector*TW_SLOTS_PER;
    *vx = TW_SLOT_LOCAL_I[sector][local][0] + in->resid_x;
    *vy = TW_SLOT_LOCAL_I[sector][local][1] + in->resid_y;
}

#endif /* TW_CAPTURE_INT_H */
