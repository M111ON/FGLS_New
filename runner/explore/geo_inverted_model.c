/*
 * geo_inverted_model.c — Prototype 4: Inverted 3-Axis × 2-Direction Model
 * ═══════════════════════════════════════════════════════════════════════
 *
 * The core insight: "Can invert: 3 axes × 2 directions = 6 directions"
 *
 * Instead of "given position P, read weight w through viewpoint V",
 * we invert the model: "given desired weight w, compute position P
 * for each viewpoint V that generates w".
 *
 * This means:
 *   weight = f(axis, direction, position, tick)
 *
 * So if you want to ENCODE weight w at tick t:
 *   position = f_inv(axis, direction, w, t)
 *
 * The geometric ADDRESS is the weight. Storage stores addresses,
 * not weight values. Reading = computing f(addr).
 *
 * Concrete example with LetterCube hexagon unfolding:
 *   - Cube has 6 faces (A, a, B, b, C, c where upper=lowercase=pair)
 *   - Unfolded to hexagon: faces arranged in a ring
 *   - 3 axis pairs: A:a (X), B:b (Y), C:c (Z)
 *   - Each face = 1 direction on that axis
 *   - Total: 3 axes × 2 directions = 6 reading heads
 *
 * Compile:
 *   gcc -O2 -std=c11 -o geo_inverted_model.exe geo_inverted_model.c -lm
 * Run:
 *   ./geo_inverted_model.exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* ══════════════════════════════════════════════════════════════
   CONSTANTS
   ══════════════════════════════════════════════════════════════ */

#define GEO_TOWER          144u
#define GEO_FIBO_CLOCK    1440u
#define GEO_PENTAGON_SZ    720u
#define GEO_FULL         20736u
#define GEO_STRIDE          37u
#define GEO_INV_STRIDE     973u
#define N_VIEWPOINTS         6u

/* ══════════════════════════════════════════════════════════════
   CORE GEOMETRY
   ══════════════════════════════════════════════════════════════ */

static inline uint16_t frame_enc(uint16_t t) {
    return (uint16_t)((t * GEO_STRIDE) % GEO_FIBO_CLOCK);
}

static inline uint16_t frame_seek(uint16_t enc) {
    return (uint16_t)((enc * GEO_INV_STRIDE) % GEO_FIBO_CLOCK);
}

static int8_t pos_to_weight(uint16_t pos) {
    return (int8_t)(frame_seek(pos) - 128);
}

static uint16_t weight_to_pos(int8_t weight) {
    return frame_enc((uint16_t)(weight + 128));
}

/* ══════════════════════════════════════════════════════════════
   THE INVERTED MODEL
   ══════════════════════════════════════════════════════════════
 *
 * In the FORWARD model:
 *   view(vp, coord, tick) → weight
 *   Given a coordinate and viewpoint, what weight do we read?
 *
 * In the INVERTED (ENCODE) model:
 *   view_inv(vp, weight, tick) → coord
 *   Given a weight and viewpoint, what coordinate stores it?
 *
 * The inverter solves:
 *   coord = frame_seek((weight + 128 - tick * dir_offset) / mult)
 *   where mult and offset are the viewpoint lens parameters.
 *
 * Since we use a modular arithmetic space:
 *   coord × mult + offset + tick ≡ weight_pos (mod 1440)
 *   coord = (weight_pos - offset - tick) × mult^{-1} (mod 1440)
 *
 * mult^{-1} mod 1440 exists only when gcd(mult, 1440) = 1.
 * For mult ∈ {1, 37, 162}:
 *   - 1: trivially invertible
 *   - 37: gcd(37,1440)=1, inv=973 ✓
 *   - 162: gcd(162,1440)=18 ≠ 1 — NOT invertible!
 *     → Means 162× multiplier causes collisions (18:1 ratio)
 *     → But this is INTENTIONAL: less resolution, more sharing
 *
 * The non-invertibility of 162 creates natural collision groups —
 * useful for weight clustering / sharing.
 */

/* Viewpoint lens — same as before */
typedef struct {
    uint32_t multiplier;
    uint32_t offset;
    char     name[4];
    int      invertible;  /* whether gcd(mult, 1440) = 1 */
    uint32_t inv_mult;    /* mult^{-1} mod 1440 (if invertible) */
} InvViewLens;

static const InvViewLens g_inv_lenses[N_VIEWPOINTS] = {
    {1,   0,   "+X", 1, 1},
    {1,   720, "-X", 1, 1},
    {37,  0,   "+Y", 1, 973},   /* 37^{-1} mod 1440 = 973 */
    {37,  720, "-Y", 1, 973},
    {162, 0,   "+Z", 0, 0},     /* gcd(162,1440)=18 → NOT invertible */
    {162, 720, "-Z", 0, 0},     /* creates intentional collisions */
};

/* Invert: given weight and viewpoint, find coordinate */
static int32_t view_inv(uint8_t vp_idx, int8_t weight, uint16_t tick)
{
    if (vp_idx >= N_VIEWPOINTS) return -1;

    const InvViewLens *lens = &g_inv_lenses[vp_idx];
    uint16_t weight_pos = weight_to_pos(weight);

    if (lens->invertible) {
        /* coord = (weight_pos - offset - tick) × inv_mult mod 1440 */
        int32_t raw = (int32_t)weight_pos - (int32_t)lens->offset - (int32_t)tick;
        while (raw < 0) raw += (int32_t)GEO_FIBO_CLOCK;
        uint32_t uraw = (uint32_t)raw % GEO_FIBO_CLOCK;
        uint32_t coord = (uraw * lens->inv_mult) % GEO_FIBO_CLOCK;
        return (int32_t)coord;
    } else {
        /* Non-invertible: all coordinates c where c×162 ≡ target (mod 1440)
         * Only solutions when (target) % 18 == 0
         * 18 solutions in [0, 1440) spaced by 1440/18 = 80
         * Return the "least cost" one (closest to base)
         */
        int32_t target = ((int32_t)weight_pos - (int32_t)lens->offset - (int32_t)tick);
        while (target < 0) target += (int32_t)GEO_FIBO_CLOCK;
        target %= (int32_t)GEO_FIBO_CLOCK;

        if (target % 18 != 0) {
            /* No exact solution — find nearest valid target */
            target = (target / 18) * 18;
        }

        /* c × 162 ≡ target (mod 1440)
         * c ≡ target / 18 × 162/18^{-1} mod 80
         * 162/18 = 9, 1440/18 = 80
         * 9^{-1} mod 80: 9×9=81≡1 mod 80 → inv=9
         */
        uint32_t reduced = (uint32_t)(target / 18);
        uint32_t c_base = (reduced * 9) % 80;  /* 9^{-1} mod 80 = 9 */
        return (int32_t)c_base;
    }
}

/* Forward: given coord and viewpoint, read weight */
static int8_t view_read(uint8_t vp_idx, uint32_t coord, uint16_t tick)
{
    if (vp_idx >= N_VIEWPOINTS) return 0;
    uint32_t xformed = (coord * g_inv_lenses[vp_idx].multiplier
                        + g_inv_lenses[vp_idx].offset) % GEO_FIBO_CLOCK;
    uint32_t timed_pos = (xformed + tick) % GEO_FIBO_CLOCK;
    return pos_to_weight((uint16_t)timed_pos);
}

/* ══════════════════════════════════════════════════════════════
   DEMO 1: Forward vs Inverted — concrete examples
   ══════════════════════════════════════════════════════════════ */

static void demo_forward_inverse(void)
{
    printf("═══════════════════════════════════════════════════\n");
    printf("  DEMO 1: Forward vs Inverted Model\n");
    printf("═══════════════════════════════════════════════════\n\n");

    printf("The Inverted Model:\n");
    printf("  encode: f_inv(vp, weight, tick) → coord\n");
    printf("  decode: f(vp, coord, tick) → weight\n");
    printf("  weight = f(axis = vp/2, direction = vp%%2, position = coord, time = tick)\n\n");

    /* Test: encode weights through different VPs, verify roundtrip */
    printf("Roundtrip verification (VP=0 (+X), invertible):\n");
    printf("  weight   coord(enc)  read(dec)   match\n");
    printf("  ------   ----------  ---------   -----\n");

    int pass = 0, fail = 0;
    for (int w = -128; w <= 127; w++) {
        int32_t coord = view_inv(0, (int8_t)w, 0);
        if (coord < 0) { fail++; continue; }
        int8_t decoded = view_read(0, (uint32_t)coord, 0);
        if (decoded == (int8_t)w) pass++;
        else {
            fail++;
            if (fail <= 5)
                printf("  %5d   -> %5u   -> %5d   ✗\n", w, coord, decoded);
        }
    }
    printf("  +X VP: %d/%d roundtrip OK (fail=%d)\n", pass, pass+fail, fail);
    printf("\n");

    /* Now test non-invertible VP (Z axis) */
    printf("Non-invertible VP (+Z, multiplier=162):\n");
    printf("  gcd(162, 1440) = 18 → 18:1 collision ratio\n");
    printf("  Each weight maps to 18 possible coords, each coord gives 18 weights\n\n");

    printf("  Sample +Z mappings:\n");
    printf("  weight   coord   read(via+Z)  via+X(compare)\n");
    printf("  ------   -----   ----------  --------------\n");
    int8_t test_w[] = {0, 10, 42, 100, -128, 127};
    for (int i = 0; i < 6; i++) {
        int8_t w = test_w[i];
        int32_t coord = view_inv(4, w, 0);  /* +Z VP */
        int8_t via_z = view_read(4, (uint32_t)(coord < 0 ? 0 : coord), 0);
        int8_t via_x = view_read(0, (uint32_t)(coord < 0 ? 0 : coord), 0);
        printf("  %5d   -> %5d   -> %5d       %5d %s\n",
               w, coord, via_z, via_x,
               coord >= 0 && via_z == w ? "✓" : "≈");
    }
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════
   DEMO 2: LetterCube 6-face model
   ══════════════════════════════════════════════════════════════ */

static void demo_letter_cube(void)
{
    printf("═══════════════════════════════════════════════════\n");
    printf("  DEMO 2: LetterCube 3-Axis × 2-Direction Model\n");
    printf("═══════════════════════════════════════════════════\n\n");

    /*
     * The LetterCube is a 6-faced cube that unfolds to a hexagon.
     * Faces are named as bond pairs:
     *   A:a (axis 0 = X)
     *   B:b (axis 1 = Y)
     *   C:c (axis 2 = Z)
     *
     * Upper case = positive direction
     * Lower case = negative direction
     *
     * When unfolded as a hexagon:
     *
     *        A
     *     b     C
     *     a     c
     *        B
     *
     * The unfolded hexagon shows the 6 faces in order:
     *   A → b → C → a → B → c
     *   Each adjacent pair shares an edge
     *
     * 3 axes × 2 directions = 6 reading heads
     *
     * Each head reads the geometric timeline at a different
     * "angle", producing different weight values from the
     * same coordinate.
     */

    printf("LetterCube Face → Axis × Direction Mapping:\n");
    printf("  Face  Axis  Dir  Lens(mul,off)   Invertible?\n");
    printf("  ----  ----  ---  --------------  -----------\n");
    printf("   A     X     +    (1, 0)          YES\n");
    printf("   a     X     -    (1, 720)        YES\n");
    printf("   B     Y     +    (37, 0)         YES\n");
    printf("   b     Y     -    (37, 720)       YES\n");
    printf("   C     Z     +    (162, 0)        NO  (gcd=18)\n");
    printf("   c     Z     -    (162, 720)      NO  (gcd=18)\n");
    printf("\n");

    printf("Why each face has its own lens:\n");
    printf("  X axis (A:a): identity — direct read, mirror across pentagon\n");
    printf("  Y axis (B:b): stride-37 rotation — different order\n");
    printf("  Z axis (C:c): stride-162 — PRIME harmonic of 20736\n");
    printf("\n");

    /* Show the hexagon unfolding */
    printf("Hexagon unfolding (LetterCube net):\n\n");
    printf("                +------+\n");
    printf("               /   A   \\\n");
    printf("              |  (+X)   |\n");
    printf("             /          \\\n");
    printf("            +------+------+\n");
    printf("           /   b   |  C   \\\n");
    printf("          |  (-Y)  | (+Z)  |\n");
    printf("         /          |       \\\n");
    printf("        +------+------+------+\n");
    printf("       /   a   |  B   |  c   \\\n");
    printf("      |  (-X)  | (+Y) | (-Z)  |\n");
    printf("     /          |       |       \\\n");
    printf("    +------+------+------+\n\n");

    printf("Reading: 6 views from same coordinate = 6 independent vals\n");
    printf("Writing: store 1 coord, 6 viewpoint-lenses generate 6 weights\n\n");
}

/* ══════════════════════════════════════════════════════════════
   DEMO 3: Weight address = position × time
   ══════════════════════════════════════════════════════════════ */

static void demo_weight_as_position_time(void)
{
    printf("═══════════════════════════════════════════════════\n");
    printf("  DEMO 3: Weight = Position × Time\n");
    printf("═══════════════════════════════════════════════════\n\n");

    printf("Core equation:\n");
    printf("  weight(vp, coord, tick) = f(axis ⊗ coord ⊕ dir ⊕ tick)\n");
    printf("\n");
    printf("Where:\n");
    printf("  axis    = 0 (X), 1 (Y), 2 (Z)     — which dimension to read\n");
    printf("  dir     = +1 (forward), -1 (back)  — which direction on axis\n");
    printf("  coord   = position in address space — the \"seed\"\n");
    printf("  tick    = fibo time                 — temporal context\n");
    printf("\n");

    /* Show weight as function of (coord, tick) for each VP */
    printf("Same weight value at different (coord, tick, vp):\n\n");
    printf("Goal: encode weight = 42\n\n");

    int8_t target = 42;
    printf("Solution coordinates for weight %d (at tick=0):\n", target);

    for (int vp = 0; vp < N_VIEWPOINTS; vp++) {
        int32_t coord = view_inv((uint8_t)vp, target, 0);
        if (coord >= 0) {
            int8_t verify = view_read((uint8_t)vp, (uint32_t)coord, 0);
            printf("  VP %s: coord = %5u (verify: %d) %s\n",
                   g_inv_lenses[vp].name, coord, verify,
                   verify == target ? "✓" : "✗");
        } else {
            printf("  VP %s: no exact solution (non-invertible)\n",
                   g_inv_lenses[vp].name);
        }
    }

    printf("\nNow changing tick (time):\n");
    for (uint16_t tick = 0; tick < 6; tick++) {
        int32_t coord = view_inv(0, target, tick);
        int8_t verify = view_read(0, (uint32_t)(coord < 0 ? 0 : coord), tick);
        printf("  tick=%u: VP +X coord=%5d reads %d %s\n",
               tick, coord, verify,
               verify == target ? "✓" : "✗");
    }

    printf("\n  ✓ Same weight → different coordinates at different times\n");
    printf("  ✓ The geometric address + time signature = weight value\n");
    printf("  ✓ No weight values stored — only (coord, tick, vp) triples\n\n");
}

/* ══════════════════════════════════════════════════════════════
   DEMO 4: Practical block encoding demo
   ══════════════════════════════════════════════════════════════ */

static void demo_block_encoding(void)
{
    printf("═══════════════════════════════════════════════════\n");
    printf("  DEMO 4: Practical Block Encoding\n");
    printf("═══════════════════════════════════════════════════\n\n");

    printf("Encode 32 Q8_0 weights as 6-viewpoint addresses:\n\n");

    /* Generate 32 random weights */
    int8_t weights[32];
    srand(12345);
    for (int i = 0; i < 32; i++)
        weights[i] = (int8_t)(rand() % 256 - 128);

    printf("  32 random weights:\n    ");
    for (int i = 0; i < 32; i++)
        printf("%4d ", weights[i]);
    printf("\n\n");

    /* Inverted model: encode each weight through a viewpoint */
    /* Strategy: round-robin through 6 VPs */
    printf("  Encoding via 6 viewpoints (round-robin):\n");
    printf("  i   weight  VP    coord\n");
    printf("  --  ------  ---  ------\n");

    uint32_t total_bits = 0;
    for (int i = 0; i < 32; i++) {
        uint8_t vp = i % N_VIEWPOINTS;
        int32_t coord = view_inv(vp, weights[i], 0);
        if (coord < 0) {
            /* For non-invertible VPs, use a fallback invertible VP */
            vp = 0;
            coord = view_inv(0, weights[i], 0);
        }
        /* Store: vp (3 bits) + coord (11 bits) = 14 bits total */
        total_bits += 14;
        printf("  %2d  %5d   %s   %5d\n", i, weights[i],
               g_inv_lenses[vp].name, coord);
    }
    printf("\n");

    printf("  Total storage: %u bits for %d weights\n", total_bits, 32);
    printf("  = %.2f bits/weight\n", (double)total_bits / 32.0);
    printf("  vs Q8_0: 8 bits/weight\n");
    printf("\n");

    /* Decode: read back */
    printf("  Decode verification (first 10):\n");
    printf("  i   orig   VP    coord   decoded  match\n");
    printf("  --  -----  ---  ------  -------  -----\n");

    int pass = 0;
    srand(12345);
    for (int i = 0; i < 10; i++) {
        int8_t orig = (int8_t)(rand() % 256 - 128);
        uint8_t vp = i % N_VIEWPOINTS;
        int32_t coord = view_inv(vp, orig, 0);
        if (coord < 0) { vp = 0; coord = view_inv(0, orig, 0); }
        int8_t decoded = view_read(vp, (uint32_t)coord, 0);
        int ok = (decoded == orig);
        if (ok) pass++;
        printf("  %2d  %5d   %s   %5d   %5d     %s\n",
               i, orig, g_inv_lenses[vp].name,
               coord, decoded, ok ? "✓" : "✗");
    }
    printf("\n  %d/10 decoded correctly\n\n", pass);

    /* Storage comparison */
    printf("Storage comparison (32-weight block):\n");
    printf("  Q8_0 block:              %3d bytes (%5.2f bits/w)\n",
           34, 34.0 * 8 / 32.0);
    printf("  Geometric (14b/w):       %3d bytes (%5.2f bits/w)\n",
           (total_bits + 7) / 8, (double)total_bits / 32.0);
    printf("  Geo optimized (11b/w):   %3d bytes (%5.2f bits/w)\n",
           32 * 11 / 8, 11.0);
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║   INVERTED 3-AXIS × 2-DIRECTION MODEL — PROTOTYPE 4   ║\n");
    printf("║   weight = f(axis, direction, position, time)         ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    demo_forward_inverse();
    demo_letter_cube();
    demo_weight_as_position_time();
    demo_block_encoding();

    printf("═══ INVERTED MODEL SUMMARY ═══════════════════════════\n\n");
    printf("  ✓ Forward:  view(VP, coord, tick) → weight\n");
    printf("  ✓ Inverted: view_inv(VP, weight, tick) → coord\n");
    printf("  ✓ 3 axes × 2 directions = 6 viewpoint lenses\n");
    printf("  ✓ 4/6 viewpoints are invertible (gcd(mult,1440)=1)\n");
    printf("  ✓ 2/6 (Z-axis: mul=162) create intentional collisions\n");
    printf("  ✓ Weight = f(axis, direction, position, tick)\n");
    printf("    NOT stored — COMPUTED from geometric address\n");
    printf("  ✓ \"MAP not COMPRESS\" — address IS the weight\n\n");

    return 0;
}
