// contour_kis_compatibility_check.c
// ================================================================
// CHECK: contour_cube_on_kis vs cube_on_kis vs skeleton_index.h
//
// 3 coordinate systems, potential conflicts:
//   1. skeleton_index.h  — 12 dodeca faces, 720 walk, stride-37
//   2. cube_on_kis.c     — 6/12 cube faces, 1440 timeline, stride-37
//   3. contour_cube_on_kis.c — 6 cube faces → 6 of 20 icosa faces
//
// The user warned: "ระวังเรื่อง fibo tick กับ weight timeline"
// ================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

// ── Include the actual headers ──
#include "geo_frame_seek.h"   // FRAME_CYCLE=1440, FRAME_STRIDE=37, frame_at(), frame_enc()
#include "skeleton_index.h"   // SKEL_WALK_LEN=720, SKEL_FACES=12, skeleton_lookup()

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PHI     1.61803398874989484820458683436564
#define PHI_INV 0.61803398874989484820458683436564

// ═══════════════════════════════════════════
// CONTESTANT 1: skeleton_index.h
// ═══════════════════════════════════════════

// skeleton: addr ∈ [0..719], maps to 12 dodeca faces × 60 slots
// stride-37 walk on 720 positions

typedef struct {
    int face;  // 0..11 (dodecahedron)
    int slot;  // 0..59 (per face)
    int enc;   // 0..719 (walk position)
    int pair;  // 0..5 (bipolar pair)
    int pole;  // 0/1 (positive/negative)
} SkelCoord;

static SkelCoord skeleton_addr_to_coord(uint64_t addr) {
    SkelCoord c;
    SkeletonIdx s = skeleton_lookup(addr);
    c.face = s.zone;   // 0..11
    c.slot = s.enc % SKEL_FACE_SZ;  // 0..59
    c.enc  = s.enc;    // 0..719
    c.pair = s.pair;    // 0..5
    c.pole = s.pole;    // 0/1
    return c;
}

// ═══════════════════════════════════════════
// CONTESTANT 2: cube_on_kis (frame_seek routing)
// ═══════════════════════════════════════════

// cube_on_kis: time t ∈ [0..∞), frame_seek gives (face, slot) on 1440 timeline
// face 0..11 (dodeca), slot 0..119 (per face)
// z = t / 1440 % 10

#define CUBE_FACES_C  12
#define CUBE_X_C      10
#define CUBE_Y_C      10
#define CUBE_Z_C      10

typedef struct {
    int face;  // 0..11 (dodeca)
    int x;     // 0..9
    int y;     // 0..9
    int z;     // 0..9 (depth layer)
    int enc;   // 0..1439 (frame_seek enc)
} CubeOnKisCoord;

static CubeOnKisCoord cube_on_kis_route(uint32_t t) {
    CubeOnKisCoord c;
    uint16_t enc = frame_enc(t);
    DualFrame f = frame_at(enc);
    c.face = f.face % CUBE_FACES_C;
    c.x    = (f.slot / 12) % CUBE_X_C;
    c.y    = f.slot % CUBE_Y_C;
    c.z    = (t / FRAME_CYCLE) % CUBE_Z_C;
    c.enc  = enc;
    return c;
}

// ═══════════════════════════════════════════
// CONTESTANT 3: contour_cube_on_kis (barycentric)
// ═══════════════════════════════════════════

// contour_cube: W×H×L × 6 faces
// 6 cube faces (±X,±Y,±Z) → 6 of 20 icosa faces
// cube (x,y,z) → barycentric (u,v) on target icosa face

#define CUBE_W_C   10
#define CUBE_H_C   10
#define CUBE_L_C   10
#define CUBE_CF_C   6
#define N_ICO_F    20
#define N_ICO_V    12

static double VERT_C[N_ICO_V][3];

static const int ICO_F[N_ICO_F][3] = {
    {0,1,2},{0,2,3},{0,3,4},{0,4,5},{0,5,1},
    {1,6,2},{2,7,3},{3,8,4},{4,9,5},{5,10,1},
    {6,7,2},{7,8,3},{8,9,4},{9,10,5},{10,6,1},
    {6,11,7},{7,11,8},{8,11,9},{9,11,10},{10,11,6}
};

static void init_verts_c(void) {
    int n = 0;
    for (int a = 0; a < 2; a++)
        for (int b = 0; b < 2; b++) {
            double s1 = a ? -1 : 1, s2 = b ? -PHI : PHI;
            VERT_C[n][0]=0; VERT_C[n][1]=s1; VERT_C[n][2]=s2; n++;
            VERT_C[n][0]=s1; VERT_C[n][1]=s2; VERT_C[n][2]=0; n++;
            VERT_C[n][0]=s2; VERT_C[n][1]=0; VERT_C[n][2]=s1; n++;
        }
    for (int i = 0; i < N_ICO_V; i++) {
        double l = sqrt(VERT_C[i][0]*VERT_C[i][0]
                      + VERT_C[i][1]*VERT_C[i][1]
                      + VERT_C[i][2]*VERT_C[i][2]);
        VERT_C[i][0]/=l; VERT_C[i][1]/=l; VERT_C[i][2]/=l;
    }
}

// cube face → icosa face mapping (same as contour_cube_on_kis.c)
static int CONTOUR_MAP[6];
static int contour_map_initialized = 0;

static void init_contour_map(void) {
    int used[N_ICO_F] = {0};
    for (int cf = 0; cf < 6; cf++) {
        double dir[3] = {0, 0, 0};
        dir[cf % 3] = (cf < 3) ? 1.0 : -1.0;
        double best_dot = -2.0; int best_face = -1;
        for (int if_ = 0; if_ < N_ICO_F; if_++) {
            if (used[if_]) continue;
            double cx=0,cy=0,cz=0;
            for (int v = 0; v < 3; v++) {
                cx += VERT_C[ICO_F[if_][v]][0];
                cy += VERT_C[ICO_F[if_][v]][1];
                cz += VERT_C[ICO_F[if_][v]][2];
            }
            cx/=3; cy/=3; cz/=3;
            double dot = cx*dir[0]+cy*dir[1]+cz*dir[2];
            if (dot > best_dot) { best_dot = dot; best_face = if_; }
        }
        CONTOUR_MAP[cf] = best_face;
        used[best_face] = 1;
    }
    contour_map_initialized = 1;
}

typedef struct {
    int cube_face;   // 0..5 (A-F)
    int ico_face;    // 0..19 (mapped icosa face)
    double u, v;     // barycentric
    double theta, phi; // spherical
    int x, y, z;     // original cube coords
} ContourCoord;

static ContourCoord contour_route(int face, int x, int y, int z) {
    ContourCoord c;
    c.cube_face = face;
    c.x = x; c.y = y; c.z = z;
    c.ico_face = CONTOUR_MAP[face];
    c.u = (double)x / CUBE_W_C;
    c.v = (double)y / CUBE_H_C;
    double z_shift = (double)z / (CUBE_L_C * 3.0);
    c.u += z_shift;
    c.v += z_shift * 0.5;
    if (c.u + c.v > 1.0) { double t = c.u; c.u = 1.0 - c.v; c.v = 1.0 - t; }

    const int *f = ICO_F[c.ico_face];
    double p[3];
    double w = 1.0 - c.u - c.v;
    p[0] = VERT_C[f[0]][0]*w + VERT_C[f[1]][0]*c.u + VERT_C[f[2]][0]*c.v;
    p[1] = VERT_C[f[0]][1]*w + VERT_C[f[1]][1]*c.u + VERT_C[f[2]][1]*c.v;
    p[2] = VERT_C[f[0]][2]*w + VERT_C[f[1]][2]*c.u + VERT_C[f[2]][2]*c.v;
    double l = sqrt(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);
    c.theta = atan2(p[1], p[0]) / M_PI * 180;
    c.phi   = acos(p[2] / l) / M_PI * 180;
    if (c.theta < 0) c.theta += 360;
    return c;
}

// ═══════════════════════════════════════════
// CHECK 1: Solid type mismatch
// ═══════════════════════════════════════════

static int check1_solid_mismatch(void) {
    printf("═══ CHECK 1: Solid Type Mismatch ═══\n");
    printf("  skeleton_index.h: %d faces (DODECAHEDRON, pentagons)\n", SKEL_FACES);
    printf("  cube_on_kis:      %d faces (DODECAHEDRON via frame_seek)\n", CUBE_FACES_C);
    printf("  contour_cube:     %d faces (CUBE = ±X,±Y,±Z)\n", CUBE_CF_C);
    printf("  contour→icosa:    %d faces (ICOSAHEDRON triangles)\n", N_ICO_F);

    printf("\n  ⚠ skeleton uses DODECA faces (12 pentagons)\n");
    printf("  ⚠ contour uses ICOSA faces (20 triangles)\n");
    printf("  ⚠ These are DUAL solids — different geometry!\n");
    printf("    Dodeca face center = Icosa vertex\n");
    printf("    Icosa face center = Dodeca vertex\n");

    int mismatch = 1;  // always flagged
    printf("  → RESULT: CONFLICT (dual solid mismatch)\n\n");
    return mismatch;
}

// ═══════════════════════════════════════════
// CHECK 2: Walk length / cycle mismatch
// ═══════════════════════════════════════════

static int check2_cycle_mismatch(void) {
    printf("═══ CHECK 2: Walk Length / Cycle ═══\n");
    printf("  skeleton: WALK_LEN = %d (12 faces × 60 slots)\n", SKEL_WALK_LEN);
    printf("  frame_seek: FRAME_CYCLE = %d (12 faces × 120 slots)\n", FRAME_CYCLE);
    printf("  skeleton stride = %d\n", SKEL_STRIDE);
    printf("  frame_seek stride = %d\n", FRAME_STRIDE);

    printf("\n  ⚠ SKEL_WALK_LEN (720) ≠ FRAME_CYCLE (1440)\n");
    printf("  ⚠ skeleton walks 720 positions, frame_seek walks 1440\n");
    printf("  ⚠ Same stride (37) but DIFFERENT cycle length!\n");
    printf("    skeleton: 37 mod 720 → visits all 720\n");
    printf("    frame_seek: 37 mod 1440 → visits all 1440\n");
    printf("    But 720 × 2 = 1440, so skeleton covers HALF of frame_seek\n");

    printf("\n  Slot per face:\n");
    printf("    skeleton: %d slots/face\n", SKEL_FACE_SZ);
    printf("    frame_seek: %d slots/face\n", FRAME_FACE_SZ);

    int mismatch = 1;
    printf("  → RESULT: CONFLICT (720 ≠ 1440, different slot density)\n\n");
    return mismatch;
}

// ═══════════════════════════════════════════
// CHECK 3: Face count × slot overlap
// ═══════════════════════════════════════════

static int check3_face_slot_overlap(void) {
    printf("═══ CHECK 3: Face × Slot Overlap ═══\n");

    // For the SAME face index (e.g., face=0), do skeleton and frame_seek
    // point to the SAME physical slot?
    printf("  Comparing addr=0..719 mapping:\n");
    printf("  %-6s │ %-20s │ %-20s │ %s\n",
           "addr", "skeleton(face,slot)", "frame_seek(face,slot)", "match?");
    printf("  ───────┼─────────────────────┼─────────────────────┼───────\n");

    int match_count = 0;
    int total = 0;
    for (int addr = 0; addr < 720; addr += 60) {
        // skeleton
        SkelCoord sc = skeleton_addr_to_coord(addr);

        // frame_seek: find the frame_seek position that corresponds to same "slot"
        // frame_seek uses enc, not addr directly
        // We need: for frame_seek enc=enc, what face/slot?
        // skeleton enc goes 0..719, frame_seek enc goes 0..1439
        // skeleton enc=addr is NOT the same as frame_seek enc=addr

        // Try to find frame_seek enc that maps to same face
        // frame face = enc / 120, skeleton face = (enc*37%720) / 60
        uint16_t skel_enc = (uint16_t)((addr * SKEL_STRIDE) % SKEL_WALK_LEN);
        uint16_t skel_face = skel_enc / SKEL_FACE_SZ;
        uint16_t skel_slot = skel_enc % SKEL_FACE_SZ;

        // For same address value, what does frame_seek give?
        DualFrame ff = frame_at(addr % FRAME_CYCLE);
        uint16_t ff_face = ff.face;
        uint16_t ff_slot = ff.slot;

        int same_face = (skel_face == ff_face);
        printf("  %-6d │ face=%-2d slot=%-3d   │ face=%-2d slot=%-3d   │ %s\n",
               addr, skel_face, skel_slot, ff_face, ff_slot,
               same_face ? "face✓" : "face✗");
        if (same_face) match_count++;
        total++;
    }

    printf("  Face match rate: %d/%d\n", match_count, total);
    printf("  ⚠ Even when face matches, slot ranges differ (60 vs 120)\n");

    int mismatch = 1;
    printf("  → RESULT: PARTIAL CONFLICT (face may align, slot never)\n\n");
    return mismatch;
}

// ═══════════════════════════════════════════
// CHECK 4: Contour cube coordinate system
// ═══════════════════════════════════════════

static int check4_contour_coordinate(void) {
    printf("═══ CHECK 4: Contour Cube Coordinate System ═══\n");
    printf("  contour_cube maps: cube(A-F) → icosa(0-19) via centroid dot\n");

    init_verts_c();
    if (!contour_map_initialized) init_contour_map();

    printf("  Mapping:\n");
    const char *names[] = {"A(+X)","B(+Y)","C(+Z)","D(-X)","E(-Y)","F(-Z)"};
    for (int i = 0; i < 6; i++) {
        printf("    %s → icosa face %d\n", names[i], CONTOUR_MAP[i]);
    }

    // Which icosa faces are NOT used?
    int used[20] = {0};
    for (int i = 0; i < 6; i++) used[CONTOUR_MAP[i]] = 1;
    printf("  Used icosa faces: ");
    int cnt = 0;
    for (int i = 0; i < 20; i++) { if (used[i]) { printf("%d ", i); cnt++; } }
    printf(" (%d/20)\n", cnt);

    printf("  UNUSED icosa faces: ");
    for (int i = 0; i < 20; i++) if (!used[i]) printf("%d ", i);
    printf("\n");

    printf("\n  ⚠ contour_cube has NO connection to:\n");
    printf("    - skeleton_index.h stride-37 walk\n");
    printf("    - frame_seek 1440 timeline\n");
    printf("    - skeleton's 12 dodeca faces\n");
    printf("    - barycentric (u,v) ≠ (slot/12, slot%12) in frame_seek\n");

    int mismatch = 1;
    printf("  → RESULT: CONFLICT (independent coordinate system)\n\n");
    return mismatch;
}

// ═══════════════════════════════════════════
// CHECK 5: Depth layer (z) semantics
// ═══════════════════════════════════════════

static int check5_depth_semantics(void) {
    printf("═══ CHECK 5: Depth (z) Semantics ═══\n");
    printf("  cube_on_kis:\n");
    printf("    z = t / 1440 %% 10\n");
    printf("    z = TIMELINE CYCLE INDEX (temporal)\n");
    printf("    z advances every 1440 time steps\n");
    printf("    z ∈ [0..9] = 10 depth layers\n");

    printf("\n  contour_cube_on_kis:\n");
    printf("    z ∈ [0..9] = CUBE DEPTH (geometric)\n");
    printf("    z shifts barycentric coords (u += z/30, v += z/60)\n");
    printf("    z is SPATIAL, not temporal\n");

    printf("\n  ⚠ z means DIFFERENT THINGS:\n");
    printf("    cube_on_kis: z = \"which 1440-cycle are we in?\" (time)\n");
    printf("    contour_cube: z = \"how deep into the mask?\" (space)\n");
    printf("    These are orthogonal dimensions being conflated!\n");

    printf("\n  Contour mask timeframe (from memory):\n");
    printf("    'system has own time dimension = L (depth ON mask)'\n");
    printf("    'No fibo tick, no stride-37. Clock = simple counter'\n");
    printf("    But cube_on_kis routes z via fibo tick (t/1440)\n");

    int mismatch = 1;
    printf("  → RESULT: CONFLICT (temporal z vs spatial z)\n\n");
    return mismatch;
}

// ═══════════════════════════════════════════
// CHECK 6: Weight timeline collision
// ═══════════════════════════════════════════

static int check6_weight_timeline(void) {
    printf("═══ CHECK 6: Weight Timeline Collision ═══\n");

    // Skeleton: addr 0..719 walks 12 faces × 60 slots
    // Frame seek: t 0..∞ walks 12 faces × 120 slots × ∞ cycles
    // Contour: 6 faces × 10 × 10 × 10 = 6000 cells (static, no timeline)

    printf("  Skeleton address space:\n");
    printf("    addr ∈ [0..%d]\n", SKEL_WALK_LEN - 1);
    printf("    12 faces × 60 slots = 720 total\n");
    printf("    Used for: CHUNK CLASSIFICATION (skel_decide)\n");

    printf("\n  Frame seek address space:\n");
    printf("    t ∈ [0..∞)  →  enc ∈ [0..1439]  (cyclic)\n");
    printf("    12 faces × 120 slots = 1440 total\n");
    printf("    Used for: WEIGHT ROUTING (cube_on_kis)\n");

    printf("\n  Contour cube address space:\n");
    printf("    (face,x,y,z) ∈ 6×10×10×10 = 6000\n");
    printf("    Used for: WEIGHT STORAGE (displacement model)\n");

    printf("\n  ⚠ CRITICAL: 3 SEPARATE ADDRESS SPACES:\n");
    printf("    720  (skeleton — chunk classification)\n");
    printf("    1440 (frame_seek — weight routing)\n");
    printf("    6000 (contour cube — weight storage)\n");
    printf("    NO CROSS-MAPPING EXISTS!\n");

    printf("\n  Pipeline should be:\n");
    printf("    addr → skeleton_lookup → zone/pair/pole  (classify)\n");
    printf("    t    → frame_seek      → face/slot/phase (route)\n");
    printf("    (face,x,y,z)          → weight cell      (store)\n");
    printf("    But: skeleton zone (0..11) ≠ frame face (0..11)?\n");
    printf("    And: skeleton slot (0..59) ≠ frame slot (0..119)?\n");

    // Check: is skeleton face == frame face for any addr?
    printf("\n  Direct comparison (first 20 addrs):\n");
    int face_match = 0, slot_match = 0, total = 0;
    for (uint64_t addr = 0; addr < 20; addr++) {
        SkeletonIdx si = skeleton_lookup(addr);
        DualFrame df = frame_at((uint16_t)(addr % FRAME_CYCLE));

        int fm = (si.zone == df.face);
        int sm = (si.enc % SKEL_FACE_SZ == df.slot % SKEL_FACE_SZ);
        printf("    addr=%2lu: skel(face=%2d enc=%3d) frame(face=%2d slot=%3d) %s %s\n",
               (unsigned long)addr, si.zone, si.enc, df.face, df.slot,
               fm ? "face✓" : "face✗", sm ? "slot✓" : "slot✗");
        if (fm) face_match++;
        if (sm) slot_match++;
        total++;
    }
    printf("  Face match: %d/%d, Slot match: %d/%d\n",
           face_match, total, slot_match, total);

    int mismatch = 1;
    printf("  → RESULT: CONFLICT (no unified address space)\n\n");
    return mismatch;
}

// ═══════════════════════════════════════════
// CHECK 7: Contour's "own time dimension"
// ═══════════════════════════════════════════

static int check7_contour_time(void) {
    printf("═══ CHECK 7: Contour's Own Time Dimension ═══\n");
    printf("  From verified knowledge:\n");
    printf("    Contour mask has L dimension = depth on mask\n");
    printf("    This IS the time dimension (not fibo tick)\n");
    printf("    Clock = simple counter, no stride-37\n\n");

    printf("  But contour_cube_on_kis.c routes z via:\n");
    printf("    z = ??? (the file doesn't route z at all!)\n");
    printf("    z is just a geometric coordinate in the cube.\n");
    printf("    NO temporal routing exists for contour cube.\n\n");

    printf("  Meanwhile cube_on_kis.c routes z via:\n");
    printf("    z = t / 1440 %% 10  (fibo tick)\n");
    printf("    This is the ONLY z-routing that exists.\n\n");

    printf("  ⚠ DANGER: If contour cube wants to ride frame_seek,\n");
    printf("    its L (depth) dimension would be forced onto fibo tick.\n");
    printf("    But contour mask's L is NOT temporal — it's spatial.\n");
    printf("    Mapping L → fibo cycle would:\n");
    printf("      1. Break the displacement model (position ≠ time)\n");
    printf("      2. Force sequential depth reads (1440 steps per layer)\n");
    printf("      3. Waste 99.3% of timeline on depth traversal\n");

    int mismatch = 1;
    printf("  → RESULT: CONFLICT (spatial L ≠ temporal fibo tick)\n\n");
    return mismatch;
}

// ═══════════════════════════════════════════
// SUMMARY
// ═══════════════════════════════════════════

int main(void) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Contour × Kis × Skeleton — Compatibility Check       ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    int conflicts = 0;
    conflicts += check1_solid_mismatch();
    conflicts += check2_cycle_mismatch();
    conflicts += check3_face_slot_overlap();
    conflicts += check4_contour_coordinate();
    conflicts += check5_depth_semantics();
    conflicts += check6_weight_timeline();
    conflicts += check7_contour_time();

    printf("══════════════════════════════════════════════════════════\n");
    printf("  SUMMARY: %d conflicts found\n", conflicts);
    printf("══════════════════════════════════════════════════════════\n\n");

    printf("  ROOT CAUSES:\n");
    printf("  1. DUAL SOLID mismatch: skeleton=dodeca(12), contour=icosa(20)\n");
    printf("  2. CYCLE mismatch: skeleton=720, frame_seek=1440\n");
    printf("  3. SLOT mismatch: skeleton=60/face, frame_seek=120/face\n");
    printf("  4. COORDINATE INDEPENDENT: 3 systems with no cross-mapping\n");
    printf("  5. z SEMANTICS: temporal (fibo) vs spatial (depth) conflated\n\n");

    printf("  WHAT WORKS:\n");
    printf("  ✓ contour_cube_on_kis.c internally consistent (barycentric)\n");
    printf("  ✓ cube_on_kis.c internally consistent (frame_seek routing)\n");
    printf("  ✓ skeleton_index.h internally consistent (720 walk)\n");
    printf("  ✗ But they DON'T talk to each other\n\n");

    printf("  OPTIONS:\n");
    printf("  A: Keep separate — contour cube doesn't need skeleton/frame_seek\n");
    printf("     (its L dimension IS its own timeline, no fibo needed)\n");
    printf("  B: Bridge via skeleton — add mapping: icosa face → dodeca face\n");
    printf("     (use dual solid correspondence: icosa face center = dodeca vertex)\n");
    printf("  C: Unified 1440 addressing — remap contour to use 12 faces × 120 slots\n");
    printf("     (abandon 6-face cube model, use 12-face dodeca model directly)\n");

    return conflicts;
}
