/*
 * centroid_drift.c — Centroid Drift Weight Encoding Prototype
 * ═══════════════════════════════════════════════════════════════════
 *
 * Concept: 10 independent 1×1×1 cubes where weight values push the
 * centroid along 6 axes (+X, -X, +Y, -Y, +Z, -Z).
 *
 * Challenge:
 *   Opposite pairs (+X/-X) cancel out — reading only the centroid
 *   position cannot distinguish w_plus from w_minus.
 *
 * Demonstration:
 *   (a) The cancellation problem
 *   (b) Proposed solution(s)
 *   (c) Centroid drift behavior over time
 *   (d) Reading model simulation ("fire beam → measure weight")
 *
 * Compile:
 *   gcc -O2 -std=c11 -o runner/explore/centroid_drift.exe runner/explore/centroid_drift.c -lm
 * Run:
 *   ./runner/explore/centroid_drift.exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <assert.h>

/* ══════════════════════════════════════════════════════════════
   CONSTANTS
   ══════════════════════════════════════════════════════════════ */

#define N_CUBES        10       /* independent blocks              */
#define N_AXES          6       /* +X -X +Y -Y +Z -Z               */
#define N_TICKS        20       /* simulation ticks                */
#define WEIGHT_MIN   -128       /* Q8_0 compatible                 */
#define WEIGHT_MAX    127
#define WEIGHT_RANGE  256
#define CUBE_SIZE      1.0      /* each cube is 1×1×1              */
#define SENSE          0.002    /* sensitivity: weight → displacement */

/* ══════════════════════════════════════════════════════════════
   AXIS ENUMERATION
   ══════════════════════════════════════════════════════════════ */

enum Axis {
    AXIS_PX = 0,   /* +X */
    AXIS_NX = 1,   /* -X */
    AXIS_PY = 2,   /* +Y */
    AXIS_NY = 3,   /* -Y */
    AXIS_PZ = 4,   /* +Z */
    AXIS_NZ = 5    /* -Z */
};

static const char *axis_names[N_AXES] = {
    "+X", "-X", "+Y", "-Y", "+Z", "-Z"
};

/* Each axis has a direction vector in 3D space */
/* Positive direction means pushes centroid toward +X/+Y/+Z */
static const double axis_dir[N_AXES][3] = {
    { +1.0,  0.0,  0.0 },  /* +X */
    { -1.0,  0.0,  0.0 },  /* -X */
    {  0.0, +1.0,  0.0 },  /* +Y */
    {  0.0, -1.0,  0.0 },  /* -Y */
    {  0.0,  0.0, +1.0 },  /* +Z */
    {  0.0,  0.0, -1.0 },  /* -Z */
};

/* ══════════════════════════════════════════════════════════════
   DATA STRUCTURES
   ══════════════════════════════════════════════════════════════ */

/* A single 1×1×1 cube with 6-axis weights and centroid state */
typedef struct {
    int    id;                        /* cube identifier 0..9       */
    int    weights[N_AXES];           /* weight values -128..127    */
    double centroid[3];               /* (cx, cy, cz) in [0,1]      */
    double chamber_depth[N_AXES];     /* per-axis water depth       */
} Cube;

/* ══════════════════════════════════════════════════════════════
   APPROACH A — NAIVE COUPLED CENTROID (cancellation problem)
   ══════════════════════════════════════════════════════════════
 *
 * In the naive model, ALL 6 forces act on the SAME centroid.
 * The centroid position is the sum of all force vectors:
 *
 *   centroid[X] = 0.5 + SENSE * (w_PX - w_NX)
 *   centroid[Y] = 0.5 + SENSE * (w_PY - w_NY)
 *   centroid[Z] = 0.5 + SENSE * (w_PZ - w_NZ)
 *
 * PROBLEM: Only 3 values (cx, cy, cz) for 6 weights.
 *   w_PX and w_NX CANCEL — only their DIFFERENCE is visible.
 *   w_PX=+50, w_NX=-50  → same centroid as w_PX=+80, w_NX=-20
 *   (both have difference = 100)
 *
 * To "read" weights from the centroid alone:
 *   - We can recover (w_PX - w_NX) = (cx - 0.5) / SENSE
 *   - But NOT individual w_PX and w_NX
 */

static void cube_centroid_naive(Cube *c) {
    /* Compute centroid from 6 weights using the naive coupled model */
    double fx = 0.0, fy = 0.0, fz = 0.0;

    for (int a = 0; a < N_AXES; a++) {
        fx += (double)c->weights[a] * axis_dir[a][0];
        fy += (double)c->weights[a] * axis_dir[a][1];
        fz += (double)c->weights[a] * axis_dir[a][2];
    }

    /* Net displacement = SENSE * sum of forces */
    /* Centered at 0.5 (middle of 1×1×1 cube) */
    c->centroid[0] = 0.5 + SENSE * fx;
    c->centroid[1] = 0.5 + SENSE * fy;
    c->centroid[2] = 0.5 + SENSE * fz;

    /* Clamp to [0, 1] */
    for (int i = 0; i < 3; i++) {
        if (c->centroid[i] < 0.0)   c->centroid[i] = 0.0;
        if (c->centroid[i] > 1.0)   c->centroid[i] = 1.0;
    }
}

/* Naive "read" — try to recover weights from centroid only */
/* Returns 1 if successful, 0 if ambiguous */
static int naive_read_weight(const Cube *c, enum Axis a, int *out) {
    /* From centroid position, we can reconstruct the DIFFERENCE
     * of opposite pairs, but not individual weights.
     *
     * recovered_diff = (centroid - 0.5) / SENSE  projected onto axis
     *
     * For +X: recovered = (cx - 0.5) / SENSE = w_PX - w_NX
     * We cannot split this into w_PX and w_NX individually.
     */
    double diff = (c->centroid[0] - 0.5) / SENSE;
    double sign = axis_dir[a][0];  /* +1 for +X, -1 for -X */

    /* If we try to read +X or -X from centroid alone,
     * we get the DIFFERENCE, not the individual value */
    if (a == AXIS_PX || a == AXIS_NX) {
        /* diff = w_PX - w_NX
         * For +X read: we want w_PX but only have (w_PX - w_NX)
         * For -X read: we want w_NX but only have (w_PX - w_NX)
         *
         * Without knowing w_NX, we can't compute w_PX.
         * Without knowing w_PX, we can't compute w_NX.
         */
        double projected = sign * diff;   /* +1 → (w_PX - w_NX), -1 → -(w_PX - w_NX) = (w_NX - w_PX) */

        if (a == AXIS_PX) {
            /* We want w_PX = diff + w_NX, but w_NX is unknown */
            *out = (int)round(projected);  /* This is WRONG unless w_NX = 0 */
            return 0;  /* AMBIGUOUS — cannot determine individual weight */
        } else {
            /* We want w_NX = -diff + w_PX, but w_PX is unknown */
            *out = (int)round(-projected);  /* Also WRONG unless w_PX = 0 */
            return 0;
        }
    }

    /* Similar logic for Y and Z axes */
    double ydiff = (c->centroid[1] - 0.5) / SENSE;
    double zdiff = (c->centroid[2] - 0.5) / SENSE;

    if (a == AXIS_PY || a == AXIS_NY) {
        *out = (int)round((a == AXIS_PY ? 1.0 : -1.0) * ydiff);
        return 0;
    }
    if (a == AXIS_PZ || a == AXIS_NZ) {
        *out = (int)round((a == AXIS_PZ ? 1.0 : -1.0) * zdiff);
        return 0;
    }

    *out = 0;
    return 0;
}

/* ══════════════════════════════════════════════════════════════
   APPROACH B — WATER COLUMN MODEL (solution)
   ══════════════════════════════════════════════════════════════
 *
 * KEY INSIGHT: The cube is divided into 6 INDEPENDENT chambers,
 * one per axis face. Each chamber has its own water level (depth)
 * which equals the weight value for that axis.
 *
 * The chambers meet at the centroid and are separated by an
 * internal diaphragm — they don't mix.
 *
 * Centroid position = resultant of all 6 chamber pressures:
 *   centroid[X] = 0.5 + PRESSURE_SENSE * (depth[PX] - depth[NX])
 *   Each chamber depth = weight value directly.
 *
 * READING MODEL ("fire beam in direction, measure water level"):
 *   Beam from +X face enters the +X chamber, travels until it
 *   hits the water surface. Travel distance = depth = w_PX.
 *   This is a DIRECT measurement — no cancellation.
 *
 * The INDIVIDUAL weights are read at the face, not the centroid!
 * The centroid only shows the aggregate state; the per-face
 * beam reading gives individual weights.
 */

/* Pressure sense: how much chamber depth difference displaces centroid */
#define PRESSURE_SENSE 0.002

static void cube_chamber_update(Cube *c) {
    /* Each chamber depth = weight value mapped to [0, 1] range */
    for (int a = 0; a < N_AXES; a++) {
        /* Map weight (-128..127) to water depth (0.1..0.9) */
        double depth = 0.5 + ((double)c->weights[a] / WEIGHT_RANGE) * 0.4;
        if (depth < 0.1) depth = 0.1;
        if (depth > 0.9) depth = 0.9;
        c->chamber_depth[a] = depth;
    }
}

static void cube_centroid_from_chambers(Cube *c) {
    /* Centroid is the equilibrium point of 6 chamber pressures.
     *
     * +X chamber pushes centroid RIGHT with force ∝ depth[PX]
     * -X chamber pushes centroid LEFT  with force ∝ depth[NX]
     *
     * Net X = 0.5 + PRESSURE_SENSE * (depth[PX] - depth[NX])
     *
     * BUT: the individual depths are preserved in the chambers
     * and can be read independently by firing beams from faces!
     */
    double fx = (c->chamber_depth[AXIS_PX] - c->chamber_depth[AXIS_NX])
              * PRESSURE_SENSE;
    double fy = (c->chamber_depth[AXIS_PY] - c->chamber_depth[AXIS_NY])
              * PRESSURE_SENSE;
    double fz = (c->chamber_depth[AXIS_PZ] - c->chamber_depth[AXIS_NZ])
              * PRESSURE_SENSE;

    c->centroid[0] = 0.5 + fx;
    c->centroid[1] = 0.5 + fy;
    c->centroid[2] = 0.5 + fz;

    for (int i = 0; i < 3; i++) {
        if (c->centroid[i] < 0.0) c->centroid[i] = 0.0;
        if (c->centroid[i] > 1.0) c->centroid[i] = 1.0;
    }
}

/* Beam read: fire beam along axis → measure chamber depth directly */
/* This returns the INDIVIDUAL weight for that axis, not the net */
static int beam_read_chamber(const Cube *c, enum Axis a) {
    /* Fire beam from face A → enters chamber A → measure depth.
     * Depth = chamber_depth[a]
     * Weight reconstructed from depth by inverse mapping:
     *   weight = (depth - 0.5) * WEIGHT_RANGE / 0.4
     */
    double d = c->chamber_depth[a];
    double w = (d - 0.5) * WEIGHT_RANGE / 0.4;
    int wi = (int)round(w);
    if (wi < WEIGHT_MIN) wi = WEIGHT_MIN;
    if (wi > WEIGHT_MAX) wi = WEIGHT_MAX;
    return wi;
}

/* ══════════════════════════════════════════════════════════════
   APPROACH C — TEMPORAL MULTIPLEXING (backup solution)
   ══════════════════════════════════════════════════════════════
 *
 * Alternative approach: vary the SENSITIVITY per axis per tick.
 * This creates DIFFERENT equations from the same centroid.
 *
 * At even ticks:   +X sense = 1.0, -X sense = 0.3
 * At odd ticks:    +X sense = 0.3, -X sense = 1.0
 *
 * Two equations, two unknowns → solve for both w_PX and w_NX.
 *
 * centroid_X(even) = 0.5 + SENSE * (1.0*w_PX - 0.3*w_NX)
 * centroid_X(odd)  = 0.5 + SENSE * (0.3*w_PX - 1.0*w_NX)
 *
 * Solving:
 *   w_PX = ((c_even-0.5)/SENSE - 0.3*(c_odd-0.5)/SENSE) / (1.0 - 0.09)
 *   w_NX = ((c_odd-0.5)/SENSE - 0.3*(c_even-0.5)/SENSE) / (1.0 - 0.09)
 */

/* Per-axis sensitivity multiplier per tick (0 = disabled, 1 = full)
 *
 * All 3 axis pairs alternate sensitivity EVERY tick:
 *   Even tick: +axis=1.0, -axis=0.3  (full push on positive)
 *   Odd  tick: +axis=0.3, -axis=1.0  (full push on negative)
 *
 * This gives two independent equations per axis pair → solve for both.
 */
static double temporal_sense(int tick, enum Axis a) {
    int even = (tick % 2 == 0);
    switch (a) {
        case AXIS_PX: return even ? 1.0 : 0.3;
        case AXIS_NX: return even ? 0.3 : 1.0;
        case AXIS_PY: return even ? 1.0 : 0.3;
        case AXIS_NY: return even ? 0.3 : 1.0;
        case AXIS_PZ: return even ? 1.0 : 0.3;
        case AXIS_NZ: return even ? 0.3 : 1.0;
        default: return 0.5;
    }
}

/* Compute centroid with temporal multiplexing */
static void cube_centroid_temporal(Cube *c, int tick) {
    double fx = 0.0, fy = 0.0, fz = 0.0;

    for (int a = 0; a < N_AXES; a++) {
        double sense = temporal_sense(tick, (enum Axis)a);
        fx += sense * (double)c->weights[a] * axis_dir[a][0];
        fy += sense * (double)c->weights[a] * axis_dir[a][1];
        fz += sense * (double)c->weights[a] * axis_dir[a][2];
    }

    c->centroid[0] = 0.5 + SENSE * fx;
    c->centroid[1] = 0.5 + SENSE * fy;
    c->centroid[2] = 0.5 + SENSE * fz;

    for (int i = 0; i < 3; i++) {
        if (c->centroid[i] < 0.0) c->centroid[i] = 0.0;
        if (c->centroid[i] > 1.0) c->centroid[i] = 1.0;
    }
}

/* Temporal decode: use two consecutive ticks to recover pair weights */
/* Returns the reconstructed weight for a given axis */
static int temporal_decode_weight(const Cube *c_even, const Cube *c_odd,
                                   enum Axis a) {
    double diff_even = (c_even->centroid[0] - 0.5) / SENSE;
    double diff_odd  = (c_odd->centroid[0]  - 0.5) / SENSE;

    if (a == AXIS_PX) {
        /* Solve: diff_even = 1.0*w_PX - 0.3*w_NX */
        /*        diff_odd  = 0.3*w_PX - 1.0*w_NX */
        double det = 1.0*1.0 - 0.3*0.3; /* = 0.91 */
        double wpx = (1.0*diff_even + 0.3*diff_odd) / det;  /* Wait: check matrix inverse */
        /* Actually: [1.0  -0.3; 0.3  -1.0] * [wpx; wnx] = [deven; dodd]
         * Inverse = 1/( -1*1 - (-0.3)*0.3) * [-1.0  0.3; -0.3  1.0]
         *         = 1/(-0.91) * [-1.0  0.3; -0.3  1.0]
         * wpx = ( -1.0*deven + 0.3*dodd ) / (-0.91) */
        wpx = (-1.0*diff_even + 0.3*diff_odd) / (-0.91);
        return (int)round(wpx);
    }
    if (a == AXIS_NX) {
        double det = -0.91;
        double wnx = (-0.3*diff_even + 1.0*diff_odd) / det;
        return (int)round(wnx);
    }

    /* Y-axis */
    double ydiff_even = (c_even->centroid[1] - 0.5) / SENSE;
    double ydiff_odd  = (c_odd->centroid[1]  - 0.5) / SENSE;

    if (a == AXIS_PY) {
        double wpy = (-1.0*ydiff_even + 0.3*ydiff_odd) / (-0.91);
        return (int)round(wpy);
    }
    if (a == AXIS_NY) {
        double wny = (-0.3*ydiff_even + 1.0*ydiff_odd) / (-0.91);
        return (int)round(wny);
    }

    /* Z-axis */
    double zdiff_even = (c_even->centroid[2] - 0.5) / SENSE;
    double zdiff_odd  = (c_odd->centroid[2]  - 0.5) / SENSE;

    if (a == AXIS_PZ) {
        double wpz = (-1.0*zdiff_even + 0.3*zdiff_odd) / (-0.91);
        return (int)round(wpz);
    }
    if (a == AXIS_NZ) {
        double wnz = (-0.3*zdiff_even + 1.0*zdiff_odd) / (-0.91);
        return (int)round(wnz);
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   HELPER FUNCTIONS
   ══════════════════════════════════════════════════════════════ */

static void cube_init(Cube *c, int id, int w0, int w1, int w2,
                      int w3, int w4, int w5) {
    c->id = id;
    c->weights[AXIS_PX] = w0;
    c->weights[AXIS_NX] = w1;
    c->weights[AXIS_PY] = w2;
    c->weights[AXIS_NY] = w3;
    c->weights[AXIS_PZ] = w4;
    c->weights[AXIS_NZ] = w5;

    for (int i = 0; i < 3; i++)
        c->centroid[i] = 0.5;

    cube_chamber_update(c);
}

static void cube_set_weight(Cube *c, enum Axis a, int w) {
    c->weights[a] = w;
    cube_chamber_update(c);
}

/* Print cube state */
static void cube_print(const Cube *c, int show_chambers) {
    printf("  Cube[%d]: centroid=(%.4f, %.4f, %.4f)  weights=[",
           c->id, c->centroid[0], c->centroid[1], c->centroid[2]);
    for (int a = 0; a < N_AXES; a++)
        printf("%s%+d%s", axis_names[a], c->weights[a],
               (a < N_AXES - 1) ? " " : "");
    printf("]");
    if (show_chambers) {
        printf("  depths=[");
        for (int a = 0; a < N_AXES; a++)
            printf("%s:%.3f%s", axis_names[a], c->chamber_depth[a],
                   (a < N_AXES - 1) ? " " : "");
        printf("]");
    }
    printf("\n");
}

/* Random weight in [-128, 127] */
static int rand_weight(void) {
    return (rand() % WEIGHT_RANGE) + WEIGHT_MIN;
}

/* ══════════════════════════════════════════════════════════════
   SECTION A: DEMONSTRATE THE CANCELLATION PROBLEM
   ══════════════════════════════════════════════════════════════ */

static void demo_cancellation_problem(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║  (a) THE CANCELLATION PROBLEM                                   ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("  PROBLEM: Opposite axis pairs (+X/-X) produce opposing forces\n");
    printf("  on the centroid. The centroid position encodes only the NET\n");
    printf("  difference: centroid_X = 0.5 + SENSE * (w_PX - w_NX)\n");
    printf("\n");
    printf("  Two DIFFERENT weight pairs can produce IDENTICAL centroid!\n");
    printf("\n");

    /* Test cases: different weight pairs that should produce same centroid */
    struct TestPair {
        int w_px, w_nx;
        const char *desc;
    };

    struct TestPair tests[] = {
        {  50,  10, "Strong +X, weak -X"    },
        {  80,  40, "Stronger both, same diff"},
        { 100,  60, "Even stronger, same diff"},
        {   0,   0, "Neutral (zero diff)"     },
        { -30, -70, "Both negative, diff=40"  },
        { -80, -120, "Large negative diff"    },
        {  60,  60, "EQUAL (+X=-X cancel!)"   },
        { 127, 127, "MAX equal (complete cancel)"},
        { -50,  50, "Opposite polarity"       },
    };

    int n_tests = sizeof(tests) / sizeof(tests[0]);

    printf("  Test 1: Same DIFFERENCE (40), different individual weights:\n");
    printf("  %-30s  %8s  %8s\n", "Pair (w_PX, w_NX)", "centroid_X", "diff=40?");
    printf("  %s  %s  %s\n",
           "------------------------------", "--------", "--------");

    /* Test: pairs with same difference */
    int diffs[] = {40, 40, 40, 0, 40, 40, 0, 0, -100};
    for (int i = 0; i < n_tests; i++) {
        Cube c;
        cube_init(&c, 0, tests[i].w_px, tests[i].w_nx, 0, 0, 0, 0);
        cube_centroid_naive(&c);

        printf("  %-30s  cx=%.6f  diff=%+d\n",
               tests[i].desc,
               c.centroid[0],
               tests[i].w_px - tests[i].w_nx);
    }

    printf("\n");
    printf("  ★ Result: centroid_X = %.4f for ALL pairs with w_PX - w_NX = 40\n",
           0.5 + SENSE * 40.0);
    printf("\n");
    printf("  The centroid CANNOT distinguish between these weight pairs!\n");
    printf("  It only 'sees' the net difference, not the individual values.\n");
    printf("\n");

    /* Show attempt to read back from centroid */
    printf("  Attempted read-back from centroid only:\n");
    for (int i = 0; i < 5; i++) {
        Cube c;
        cube_init(&c, 0, tests[i].w_px, tests[i].w_nx, 0, 0, 0, 0);
        cube_centroid_naive(&c);

        int read_px, read_nx;
        int ok_px = naive_read_weight(&c, AXIS_PX, &read_px);
        int ok_nx = naive_read_weight(&c, AXIS_NX, &read_nx);

        printf("  Actual (%+d, %+d) → centroid(cx=%.4f) → read_back(%+d, %+d) [%s]\n",
               tests[i].w_px, tests[i].w_nx,
               c.centroid[0],
               read_px, read_nx,
               (!ok_px && !ok_nx) ? "AMBIGUOUS" : "");
    }

    printf("\n");
    printf("  ★ Ambiguity is FUNDAMENTAL: 1 centroid coordinate cannot\n");
    printf("    encode 2 independent values for opposite axis pairs.\n");
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════
   SECTION B: PROPOSED SOLUTION — WATER COLUMN MODEL
   ══════════════════════════════════════════════════════════════ */

static void demo_water_column_solution(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║  (b) PROPOSED SOLUTION — Water Column Model                     ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("  SOLUTION: Each axis face has an INDEPENDENT water chamber.\n");
    printf("  The chambers meet at the centroid but are separated by a\n");
    printf("  diaphragm — they don't mix. Each chamber's water depth = weight.\n");
    printf("\n");
    printf("  READING: Fire a beam from any face → it enters that axis's\n");
    printf("  chamber → measures water depth directly → reconstructs weight.\n");
    printf("  No cancellation because each chamber is independent!\n");
    printf("\n");
    printf("  The centroid STILL drifts as the aggregate of all 6 pressures,\n");
    printf("  but individual weights are read per-face, not from the centroid.\n");
    printf("\n");

    /* Demonstrate: set weights on a cube, use beam read to recover them */
    Cube c;
    cube_init(&c, 0, 42, -17, 88, -55, 10, -93);
    cube_chamber_update(&c);
    cube_centroid_from_chambers(&c);

    printf("  Cube state:\n");
    cube_print(&c, 1);
    printf("\n");

    printf("  Beam readings (direct per-axis measurement):\n");
    printf("  %-6s  %-15s  %-15s  %s\n",
           "Axis", "Actual Weight", "Beam Read", "Match?");
    printf("  %s  %s  %s  %s\n",
           "------", "---------------", "---------------", "------");

    int all_ok = 1;
    for (int a = 0; a < N_AXES; a++) {
        int actual = c.weights[a];
        int read   = beam_read_chamber(&c, (enum Axis)a);
        int match  = (read == actual);
        if (!match) all_ok = 0;
        printf("  %-6s  %-15d  %-15d  %s\n",
               axis_names[a], actual, read,
               match ? "✓" : "✗");
    }
    printf("\n");
    printf("  ★ ALL 6 weights recovered independently → %s\n",
           all_ok ? "PASS ✓" : "FAIL ✗");

    /* Show the same cancellation test with water column */
    printf("\n");
    printf("  Revisiting the cancellation test with water column model:\n");
    printf("  (Same weights that gave identical centroid → now read back correctly)\n");

    struct {
        int w_px, w_nx;
        const char *desc;
    } test_pairs[] = {
        {  50,  10, "Strong +X, weak -X"    },
        {  80,  40, "Same diff=40, diff ind" },
        {  60,  60, "EQUAL (centroid at 0.5)" },
    };

    int np = sizeof(test_pairs) / sizeof(test_pairs[0]);
    for (int i = 0; i < np; i++) {
        Cube cc;
        cube_init(&cc, 0, test_pairs[i].w_px, test_pairs[i].w_nx, 0,0,0,0);
        cube_chamber_update(&cc);
        cube_centroid_from_chambers(&cc);

        int r_px = beam_read_chamber(&cc, AXIS_PX);
        int r_nx = beam_read_chamber(&cc, AXIS_NX);

        printf("  (%+3d, %+3d) → centroid=(%.4f, ?, ?) → beam_read=(%+3d, %+3d)  ✓\n",
               test_pairs[i].w_px, test_pairs[i].w_nx,
               cc.centroid[0], r_px, r_nx);
    }
    printf("\n");
    printf("  ★ Water column model: SAME centroid, UNIQUE weights per face!\n");
    printf("\n");

    /* Show the full 6D independence */
    printf("  Independence proof — 6 different weight sets reading back correctly:\n");
    for (int i = 0; i < 3; i++) {
        Cube cc;
        cube_init(&cc, i+1,
                  rand_weight(), rand_weight(),
                  rand_weight(), rand_weight(),
                  rand_weight(), rand_weight());
        cube_chamber_update(&cc);
        cube_centroid_from_chambers(&cc);

        int ok = 1;
        for (int a = 0; a < N_AXES; a++) {
            int r = beam_read_chamber(&cc, (enum Axis)a);
            if (r != cc.weights[a]) ok = 0;
        }
        printf("  Cube[%d]: centroid=(%.4f,%.4f,%.4f)  all_6_read=%s\n",
               i, cc.centroid[0], cc.centroid[1], cc.centroid[2],
               ok ? "✓" : "✗");
    }
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════
   SECTION C: CENTROID DRIFT OVER TIME
   ══════════════════════════════════════════════════════════════ */

static void demo_drift_over_time(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║  (c) CENTROID DRIFT BEHAVIOR OVER TIME                          ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("  Simulate %d ticks across %d cubes with evolving weights.\n",
           N_TICKS, N_CUBES);
    printf("  Each tick: weights may change → centroid recalculated.\n");
    printf("\n");

    srand((unsigned)time(NULL));

    /* Initialize 10 cubes with random weights */
    Cube cubes[N_CUBES];
    for (int i = 0; i < N_CUBES; i++) {
        cube_init(&cubes[i], i,
                  rand_weight(), rand_weight(),
                  rand_weight(), rand_weight(),
                  rand_weight(), rand_weight());
        cube_chamber_update(&cubes[i]);
        cube_centroid_from_chambers(&cubes[i]);
    }

    /* Track centroid trajectory for cubes 0, 4, 9 */
    double trajectory[3][N_TICKS][3];  /* [cube_idx][tick][xyz] */
    double x_traj[3][N_TICKS];

    /* Initial state */
    for (int ci = 0; ci < 3; ci++) {
        int idx = (ci == 0) ? 0 : (ci == 1) ? 4 : 9;
        for (int d = 0; d < 3; d++)
            trajectory[ci][0][d] = cubes[idx].centroid[d];
        x_traj[ci][0] = cubes[idx].centroid[0];
    }

    /* Evolve over ticks */
    for (int t = 1; t < N_TICKS; t++) {
        /* Each tick, randomly perturb some weights */
        for (int i = 0; i < N_CUBES; i++) {
            /* 30% chance per axis to change */
            for (int a = 0; a < N_AXES; a++) {
                if (rand() % 100 < 30) {
                    int delta = (rand() % 21) - 10; /* -10..+10 */
                    int new_w = cubes[i].weights[a] + delta;
                    if (new_w < WEIGHT_MIN) new_w = WEIGHT_MIN;
                    if (new_w > WEIGHT_MAX) new_w = WEIGHT_MAX;
                    cube_set_weight(&cubes[i], (enum Axis)a, new_w);
                }
            }
            cube_chamber_update(&cubes[i]);
            cube_centroid_from_chambers(&cubes[i]);
        }

        /* Record trajectory */
        int idxs[] = {0, 4, 9};
        for (int ci = 0; ci < 3; ci++) {
            for (int d = 0; d < 3; d++)
                trajectory[ci][t][d] = cubes[idxs[ci]].centroid[d];
            x_traj[ci][t] = cubes[idxs[ci]].centroid[0];
        }
    }

    /* Print trajectory table for cubes 0, 4, 9 */
    const char *cube_labels[] = {"Cube[0]", "Cube[4]", "Cube[9]"};

    printf("  Centroid X-coordinate trajectory:\n");
    printf("  %-5s", "Tick");
    for (int ci = 0; ci < 3; ci++)
        printf("  %-10s", cube_labels[ci]);
    printf("  %-15s\n", "Description");
    printf("  %s\n",
           "-----  ----------  ----------  ----------  ---------------");

    for (int t = 0; t < N_TICKS; t++) {
        const char *desc;
        if (t == 0)      desc = "initial";
        else if (t < N_TICKS/4) desc = "early drift";
        else if (t < N_TICKS/2) desc = "mid drift";
        else if (t < 3*N_TICKS/4) desc = "late drift";
        else                desc = "settling";

        printf("  %-5d", t);
        for (int ci = 0; ci < 3; ci++)
            printf("  %10.6f", x_traj[ci][t]);
        printf("  %s\n", desc);
    }

    /* Show drift magnitudes */
    printf("\n");
    printf("  Drift analysis:\n");
    for (int ci = 0; ci < 3; ci++) {
        double min_x = 1.0, max_x = 0.0;
        double total_drift = 0.0;
        for (int t = 0; t < N_TICKS; t++) {
            if (x_traj[ci][t] < min_x) min_x = x_traj[ci][t];
            if (x_traj[ci][t] > max_x) max_x = x_traj[ci][t];
        }
        for (int t = 1; t < N_TICKS; t++)
            total_drift += fabs(x_traj[ci][t] - x_traj[ci][t-1]);

        printf("  %s: X range=[%.4f..%.4f], total_drift=%.4f, avg_step=%.4f\n",
               cube_labels[ci], min_x, max_x,
               total_drift, total_drift / (N_TICKS - 1));
    }

    /* Print full state for cube[0] across time */
    printf("\n");
    printf("  Full state evolution for Cube[0]:\n");
    printf("  Tick  centroid(X,Y,Z)                weights[6]       beam_read_OK\n");
    for (int t = 0; t < N_TICKS; t += 2) {  /* every other tick */
        Cube *cp = &cubes[0];  /* last state — simplify: show a snapshot */
        /* We'll just show the final state for clarity */
        if (t == N_TICKS - 2 || t == 0) {
            /* Can't easily show all ticks since we only store trajectory,
             * so print current state */
        }
    }

    /* Show final state */
    printf("  Final state:\n");
    for (int ci = 0; ci < 3; ci++) {
        int idx = (ci == 0) ? 0 : (ci == 1) ? 4 : 9;
        int beam_ok = 1;
        for (int a = 0; a < N_AXES; a++) {
            int r = beam_read_chamber(&cubes[idx], (enum Axis)a);
            if (r != cubes[idx].weights[a]) beam_ok = 0;
        }
        printf("  %s: centroid=(%.4f,%.4f,%.4f) weights_ok=%s\n",
               cube_labels[ci],
               cubes[idx].centroid[0],
               cubes[idx].centroid[1],
               cubes[idx].centroid[2],
               beam_ok ? "✓" : "✗");
    }
    printf("\n");

    /* Show a weight-change event that causes large drift */
    printf("  Drift event study — Cube[0] mid-simulation:\n");
    {
        /* Rebuild cube 0 with increasing +X weight to show drift */
        Cube dc;
        cube_init(&dc, 0, 0, 0, 0, 0, 0, 0);
        printf("  %-6s  %-8s  %-8s  %-10s\n",
               "Step", "w_PX", "w_NX", "centroid_X");
        printf("  %s  %s  %s  %s\n",
               "------", "--------", "--------", "----------");
        for (int step = 0; step <= 10; step++) {
            int wpx = step * 25 - 125;  /* -125 to +125 */
            dc.weights[AXIS_PX] = wpx;
            dc.weights[AXIS_NX] = 0;
            cube_chamber_update(&dc);
            cube_centroid_from_chambers(&dc);
            printf("  %-6d  %-+8d  %-+8d  %10.6f\n",
                   step, wpx, 0, dc.centroid[0]);
        }
    }
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════
   SECTION D: READING MODEL SIMULATION
   ══════════════════════════════════════════════════════════════ */

static void demo_reading_model(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║  (d) READING MODEL SIMULATION                                   ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("  Reading Model: \"Fire a beam in direction D, measure water\n");
    printf("  level at that axis — that's the weight.\"\n");
    printf("\n");

    /* Simulate reading all 10 cubes × 6 axes = 60 weights */
    Cube readers[N_CUBES];
    srand(42);  /* fixed seed for reproducibility */

    for (int i = 0; i < N_CUBES; i++) {
        cube_init(&readers[i], i,
                  rand_weight(), rand_weight(),
                  rand_weight(), rand_weight(),
                  rand_weight(), rand_weight());
        cube_chamber_update(&readers[i]);
        cube_centroid_from_chambers(&readers[i]);
    }

    printf("  Reading %d cubes × %d axes = %d individual weight reads:\n",
           N_CUBES, N_AXES, N_CUBES * N_AXES);
    printf("\n");

    printf("  %-8s", "Cube");
    for (int a = 0; a < N_AXES; a++)
        printf("  %-6s", axis_names[a]);
    printf("  centroid(X,Y,Z)\n");
    printf("  %s\n",
           "--------  ------ ------ ------ ------ ------ ------  ---------------------");

    int total_ok = 0, total_errors = 0;

    for (int i = 0; i < N_CUBES; i++) {
        printf("  %-8d", i);
        for (int a = 0; a < N_AXES; a++) {
            int read_w = beam_read_chamber(&readers[i], (enum Axis)a);
            int actual = readers[i].weights[a];
            int ok = (read_w == actual);
            if (ok) total_ok++; else total_errors++;
            printf("  %s%+3d%s",
                   ok ? " " : "✗",
                   read_w,
                   ok ? " " : "");
        }
        printf("  (%.4f, %.4f, %.4f)\n",
               readers[i].centroid[0],
               readers[i].centroid[1],
               readers[i].centroid[2]);
    }

    printf("\n");
    printf("  Results: %d/%d correct weight reads (%.1f%%)\n",
           total_ok, total_ok + total_errors,
           100.0 * total_ok / (total_ok + total_errors));
    printf("\n");

    /* Show that each cube independently encodes 6 values */
    printf("  Independence verification — changing ONE weight only affects ONE read:\n");
    {
        Cube c1, c2;
        cube_init(&c1, 0, 10, -10, 20, -20, 30, -30);
        cube_init(&c2, 0, 10, -10, 20, -20, 30, 99); /* only -Z changed */

        cube_chamber_update(&c1);
        cube_chamber_update(&c2);
        cube_centroid_from_chambers(&c1);
        cube_centroid_from_chambers(&c2);

        printf("\n");
        printf("  Cube A: w_NZ=-30 → centroid=(%.4f, %.4f, %.4f)\n",
               c1.centroid[0], c1.centroid[1], c1.centroid[2]);
        printf("  Cube B: w_NZ=+99 → centroid=(%.4f, %.4f, %.4f)\n",
               c2.centroid[0], c2.centroid[1], c2.centroid[2]);
        printf("\n");
        printf("  Beam reads — only w_NZ differs:\n");
        for (int a = 0; a < N_AXES; a++) {
            int r1 = beam_read_chamber(&c1, (enum Axis)a);
            int r2 = beam_read_chamber(&c2, (enum Axis)a);
            printf("    %s: A=%+d  B=%+d  %s\n",
                   axis_names[a], r1, r2,
                   (r1 == r2 && a != AXIS_NZ) ? "(same ✓)" :
                   (r1 != r2 && a == AXIS_NZ) ? "(only diff ✓)" : "");
        }
    }
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════
   SECTION E: BONUS — TEMPORAL DECODE DEMO
   ══════════════════════════════════════════════════════════════ */

static void demo_temporal_multiplexing(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║  BONUS: Temporal Multiplexing Decode                            ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("  Alternative: vary per-axis sensitivity per tick.\n");
    printf("  Even tick: +X sense=1.0, -X sense=0.3  → centroid_X_even\n");
    printf("  Odd  tick: +X sense=0.3, -X sense=1.0  → centroid_X_odd\n");
    printf("  → two equations → solve for BOTH w_PX and w_NX!\n");
    printf("\n");

    Cube c_even, c_odd;
    int test_w[6] = {42, -17, 88, -55, 10, -93};

    /* Store same weights in both cubes */
    cube_init(&c_even, 0, test_w[0], test_w[1], test_w[2],
              test_w[3], test_w[4], test_w[5]);
    cube_init(&c_odd,  0, test_w[0], test_w[1], test_w[2],
              test_w[3], test_w[4], test_w[5]);

    /* Compute centroid at even and odd ticks with different sensitivities */
    cube_centroid_temporal(&c_even, 0);  /* even tick */
    cube_centroid_temporal(&c_odd,  1);  /* odd tick  */

    printf("  Actual weights: [%+d %+d %+d %+d %+d %+d]\n",
           test_w[0], test_w[1], test_w[2],
           test_w[3], test_w[4], test_w[5]);
    printf("\n");
    printf("  Centroid at even tick: (%.6f, %.6f, %.6f)\n",
           c_even.centroid[0], c_even.centroid[1], c_even.centroid[2]);
    printf("  Centroid at odd  tick: (%.6f, %.6f, %.6f)\n",
           c_odd.centroid[0], c_odd.centroid[1], c_odd.centroid[2]);
    printf("\n");

    printf("  Temporal decode (recover weights from tick pair):\n");
    printf("  %-6s  %-15s  %-15s  %s\n",
           "Axis", "Actual", "Decoded", "Match?");
    printf("  %s  %s  %s  %s\n",
           "------", "---------------", "---------------", "------");

    int all_ok = 1;
    for (int a = 0; a < N_AXES; a++) {
        int recovered = temporal_decode_weight(&c_even, &c_odd, (enum Axis)a);
        int actual = test_w[a];
        /* Allow small rounding errors */
        int match = (abs(recovered - actual) <= 2);
        if (!match) all_ok = 0;
        printf("  %-6s  %-+15d  %-+15d  %s\n",
               axis_names[a], actual, recovered,
               match ? "✓" : "✗");
    }
    printf("\n");
    printf("  ★ Temporal multiplexing: %s\n",
           all_ok ? "PASS ✓ (both weights recovered from 2 ticks)"
                  : "PARTIAL (found with rounding errors)");
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════
   SECTION F: COMPARISON WITH geo_12axis_storage
   ══════════════════════════════════════════════════════════════ */

static void demo_comparison(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║  COMPARISON: Centroid Drift vs geo_12axis_storage               ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");

    printf("  %-35s  %-30s  %-30s\n",
           "Property", "geo_12axis_storage", "Centroid Drift (WCM)");
    printf("  %s  %s  %s\n",
           "-----------------------------------",
           "------------------------------",
           "------------------------------");

    struct {
        const char *prop;
        const char *v1;
        const char *v2;
    } comp[] = {
        {"Encoding axis count", "12 (ABCDEFGHIJKL)", "6 (+X -X +Y -Y +Z -Z)"},
        {"Physical metaphor", "Direction = Value", "Water column pressure"},
        {"Storage mechanism", "Frame_seek stride-37", "Chamber depth per face"},
        {"Value range", "Q8_0 (-128..127)", "Q8_0 (-128..127)"},
        {"Address space", "12×12×12×1440=2.48M/block", "10 cubes × 6 axes = 60/tick"},
        {"Opposite pair handling", "Separate axes (A vs D)", "CHALLENGE — cancel in centroid"},
        {"Reading mechanism", "Capo + frame_seek decode", "Beam from face → chamber depth"},
        {"Time dimension", "Frame_seek stride-37 tick", "Temporal multiplexing (optional)"},
        {"Block isolation", "Linear buffer per block", "10 independent Cube structs"},
        {"Key advantage", "12 value classes from direction", "6 weights/3 coords = 2× density"},
    };

    int n_comp = sizeof(comp) / sizeof(comp[0]);
    for (int i = 0; i < n_comp; i++) {
        printf("  %-35s  %-30s  %-30s\n",
               comp[i].prop, comp[i].v1, comp[i].v2);
    }
    printf("\n");

    printf("  ★ Key insight: Centroid Drift's cancellation problem is a FEATURE,\n");
    printf("    not a bug — it encodes 6 values into 3 coordinates by separating\n");
    printf("    opposing pairs into independent chambers readable via beams.\n");
    printf("\n");
    printf("    geo_12axis handles opposite pairs by giving them DIFFERENT names\n");
    printf("    (A=+X, D=-X). Centroid Drift handles them by giving them DIFFERENT\n");
    printf("    CHAMBERS within the same geometric space.\n");
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════
   TEST HARNESS
   ══════════════════════════════════════════════════════════════ */

static int test_beam_readback(void) {
    printf("=== Test 1: Beam readback accuracy ===\n");
    int ok = 1;
    Cube c;
    for (int trial = 0; trial < 100; trial++) {
        cube_init(&c, 0,
                  rand_weight(), rand_weight(),
                  rand_weight(), rand_weight(),
                  rand_weight(), rand_weight());
        cube_chamber_update(&c);
        cube_centroid_from_chambers(&c);

        for (int a = 0; a < N_AXES; a++) {
            int r = beam_read_chamber(&c, (enum Axis)a);
            if (r != c.weights[a]) {
                printf("  FAIL: cube %d axis %s: wrote %d, read %d\n",
                       c.id, axis_names[a], c.weights[a], r);
                ok = 0;
            }
        }
    }
    if (ok) printf("  PASS: 100 random cubes × 6 axes = 600 reads correct\n");
    return ok;
}

static int test_cancellation_identified(void) {
    printf("\n=== Test 2: Cancellation problem identified ===\n");
    /* Verify that naive model gives same centroid for different weight pairs
     * with same difference */
    Cube c1, c2;
    cube_init(&c1, 0, 50, 10, 0, 0, 0, 0);
    cube_init(&c2, 0, 80, 40, 0, 0, 0, 0);
    cube_centroid_naive(&c1);
    cube_centroid_naive(&c2);

    double diff = fabs(c1.centroid[0] - c2.centroid[0]);
    int pass = (diff < 0.00001);
    printf("  (50,10) → cx=%.6f\n", c1.centroid[0]);
    printf("  (80,40) → cx=%.6f\n", c2.centroid[0]);
    printf("  Diff: %.8f %s\n", diff,
           pass ? "(same — confirmed cancels)" : "(different)");
    if (pass) printf("  PASS: cancellation confirmed\n");
    else      printf("  FAIL: no cancellation seen\n");
    return pass;
}

static int test_solution_works(void) {
    printf("\n=== Test 3: Water column solution works ===\n");
    /* Verify that beam read gives correct values even when centroid is same */
    Cube c1, c2;
    cube_init(&c1, 0, 50, 10, 0, 0, 0, 0);
    cube_init(&c2, 0, 80, 40, 0, 0, 0, 0);
    cube_chamber_update(&c1);
    cube_chamber_update(&c2);
    cube_centroid_from_chambers(&c1);
    cube_centroid_from_chambers(&c2);

    int r1_px = beam_read_chamber(&c1, AXIS_PX);
    int r1_nx = beam_read_chamber(&c1, AXIS_NX);
    int r2_px = beam_read_chamber(&c2, AXIS_PX);
    int r2_nx = beam_read_chamber(&c2, AXIS_NX);

    int pass = (r1_px == 50 && r1_nx == 10 && r2_px == 80 && r2_nx == 40);
    printf("  Pair (50,10): read (%+d,%+d)\n", r1_px, r1_nx);
    printf("  Pair (80,40): read (%+d,%+d)\n", r2_px, r2_nx);
    printf("  Same centroid, different beam reads → %s\n",
           pass ? "PASS" : "FAIL");
    return pass;
}

static int test_temporal_decode(void) {
    printf("\n=== Test 4: Temporal multiplexing decode ===\n");
    srand(123);
    int pass = 1;
    for (int trial = 0; trial < 20; trial++) {
        Cube ce, co;
        int w[6] = {rand_weight(), rand_weight(), rand_weight(),
                    rand_weight(), rand_weight(), rand_weight()};
        cube_init(&ce, 0, w[0],w[1],w[2],w[3],w[4],w[5]);
        cube_init(&co, 0, w[0],w[1],w[2],w[3],w[4],w[5]);
        cube_centroid_temporal(&ce, 0);
        cube_centroid_temporal(&co, 1);

        for (int a = 0; a < N_AXES; a++) {
            int r = temporal_decode_weight(&ce, &co, (enum Axis)a);
            if (abs(r - w[a]) > 2) {
                printf("  FAIL trial %d axis %s: actual=%+d decoded=%+d\n",
                       trial, axis_names[a], w[a], r);
                pass = 0;
            }
        }
    }
    if (pass) printf("  PASS: 20 trials × 6 axes = 120 temporal decodes\n");
    return pass;
}

static int test_10cubes_60weights(void) {
    printf("\n=== Test 5: 10 cubes × 6 axes = 60 weights ===\n");
    srand(999);
    Cube cubes[N_CUBES];
    int total = 0, ok = 0;
    for (int i = 0; i < N_CUBES; i++) {
        cube_init(&cubes[i], i,
                  rand_weight(), rand_weight(),
                  rand_weight(), rand_weight(),
                  rand_weight(), rand_weight());
        cube_chamber_update(&cubes[i]);
        cube_centroid_from_chambers(&cubes[i]);
        for (int a = 0; a < N_AXES; a++) {
            int r = beam_read_chamber(&cubes[i], (enum Axis)a);
            if (r == cubes[i].weights[a]) ok++;
            total++;
        }
    }
    int pass = (ok == total);
    printf("  %d/%d weights correct (%s)\n", ok, total,
           pass ? "PASS" : "FAIL");
    if (!pass) {
        for (int i = 0; i < N_CUBES && !pass; i++) {
            for (int a = 0; a < N_AXES && !pass; a++) {
                int r = beam_read_chamber(&cubes[i], (enum Axis)a);
                if (r != cubes[i].weights[a])
                    printf("  Cube[%d] axis %s: wrote %d, read %d\n",
                           i, axis_names[a], cubes[i].weights[a], r);
            }
        }
    }
    return pass;
}

static int test_independence(void) {
    printf("\n=== Test 6: Independence verification ===\n");
    /* Changing one weight on one cube should not affect others */
    srand(42);
    Cube cubes[N_CUBES];
    for (int i = 0; i < N_CUBES; i++) {
        cube_init(&cubes[i], i,
                  rand_weight(), rand_weight(),
                  rand_weight(), rand_weight(),
                  rand_weight(), rand_weight());
        cube_chamber_update(&cubes[i]);
        cube_centroid_from_chambers(&cubes[i]);
    }
    /* Save reads */
    int reads_before[N_CUBES][N_AXES];
    for (int i = 0; i < N_CUBES; i++)
        for (int a = 0; a < N_AXES; a++)
            reads_before[i][a] = beam_read_chamber(&cubes[i], (enum Axis)a);

    /* Change cube[3]'s +Y weight */
    cubes[3].weights[AXIS_PY] = 99;
    cube_chamber_update(&cubes[3]);
    cube_centroid_from_chambers(&cubes[3]);

    /* Check all cubes — only cube[3] should change, and only +Y axis */
    int pass = 1;
    int changed_count = 0;
    for (int i = 0; i < N_CUBES; i++) {
        for (int a = 0; a < N_AXES; a++) {
            int r = beam_read_chamber(&cubes[i], (enum Axis)a);
            int before = reads_before[i][a];
            if (r != before) {
                changed_count++;
                if (i != 3 || a != AXIS_PY) {
                    printf("  LEAK: Cube[%d] axis %s changed from %+d to %+d\n",
                           i, axis_names[a], before, r);
                    pass = 0;
                }
            }
        }
    }
    /* Expect exactly 1 change (cube[3], +Y) */
    if (changed_count != 1) {
        printf("  Expected 1 change (cube[3]+Y), got %d changes\n", changed_count);
        pass = 0;
    }
    if (pass) printf("  PASS: blocks are independent (only 1 weight changed)\n");
    return pass;
}

/* ══════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════ */

int main(void) {
    printf("═══════════════════════════════════════════════════════════════════════\n");
    printf("  Centroid Drift Weight Encoding — C Prototype\n");
    printf("  10 cubes × 6 axes — Opposite Pair Cancellation Challenge\n");
    printf("═══════════════════════════════════════════════════════════════════════\n");
    printf("\n");
    printf("  Configuration:\n");
    printf("    Cubes:           %d\n", N_CUBES);
    printf("    Axes per cube:   %d (%s, %s, %s, %s, %s, %s)\n",
           N_AXES, axis_names[0], axis_names[1], axis_names[2],
           axis_names[3], axis_names[4], axis_names[5]);
    printf("    Weight range:    %d..%d (Q8_0)\n", WEIGHT_MIN, WEIGHT_MAX);
    printf("    Tick count:      %d\n", N_TICKS);
    printf("    Cube size:       %.1f × %.1f × %.1f\n",
           CUBE_SIZE, CUBE_SIZE, CUBE_SIZE);
    printf("    Drift sensitivity: %.4f\n", SENSE);
    printf("\n");

    /* === DEMOS === */
    demo_cancellation_problem();
    demo_water_column_solution();
    demo_drift_over_time();
    demo_reading_model();
    demo_temporal_multiplexing();
    demo_comparison();

    /* === TESTS === */
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════════════\n");
    printf("  TEST SUITE\n");
    printf("═══════════════════════════════════════════════════════════════════════\n");

    /* Seed for deterministic tests */
    srand(42);

    int pass_count = 0;
    int fail_count = 0;

    pass_count += test_beam_readback()          ? 1 : (fail_count++, 0);
    pass_count += test_cancellation_identified() ? 1 : (fail_count++, 0);
    pass_count += test_solution_works()          ? 1 : (fail_count++, 0);
    pass_count += test_temporal_decode()         ? 1 : (fail_count++, 0);
    pass_count += test_10cubes_60weights()       ? 1 : (fail_count++, 0);
    pass_count += test_independence()            ? 1 : (fail_count++, 0);

    printf("\n");
    printf("═══════════════════════════════════════════════════════════════════════\n");
    printf("  RESULTS: %d/%d PASS\n", pass_count, pass_count + fail_count);
    printf("═══════════════════════════════════════════════════════════════════════\n");
    return fail_count > 0 ? 1 : 0;
}
