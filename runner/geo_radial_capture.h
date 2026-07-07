/*
 * geo_radial_capture.h — 3D radial capture on dodeca+rotated-dodeca (24 faces)
 *
 * Design:
 *   - Dodecahedron A (12 pentagon faces) + Dodecahedron B (rotate X:36 Y:36)
 *   - 24 vertices on unit sphere = 24 face centers
 *   - 7-node radial mesh per vertex (54,54,60,60,60,60,12 = 360)
 *   - Parallel CAM: all 24 vertices check input simultaneously
 *   - Zero-copy twin swap: addr ^ TWIN_BIT
 *
 * Flow:
 *   input (content hash) → find nearest vertex → expand 7-node mesh → address
 *   No input → return 0
 *
 * Angle pattern (from goldberg pentagon-hexagon seam):
 *   54 = pentagon interior / 2 = 108/2
 *   60 = hexagon interior / 2 = 120/2
 *   12 = hexagon - pentagon = 120 - 108 (seam gap)
 */

#ifndef GEO_RADIAL_CAPTURE_H
#define GEO_RADIAL_CAPTURE_H

#include <stdint.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════ */

#define RC_N_VERTICES      24    /* 12 + 12 (dodeca A + dodeca B) */
#define RC_N_NODES          7    /* radial mesh nodes per vertex   */
#define RC_TWIN_BIT  0x4000000000000000ULL  /* bit62: face A ↔ face B */
#define RC_SPHERE_R  1.0     /* unit sphere                     */

/* ═══════════════════════════════════════════════════════════════
   ANGULAR GAPS (degrees)
   Pattern: 54, 54, 60, 60, 60, 60, gap 12 = 360
   ═══════════════════════════════════════════════════════════════ */

#define RC_GAP_0   54
#define RC_GAP_1   54
#define RC_GAP_2   60
#define RC_GAP_3   60
#define RC_GAP_4   60
#define RC_GAP_5   60
#define RC_GAP_6   12    /* pentagon-hexagon seam */

/* Cumulative angles (positions of each radial line) */
static const double RC_ANGLES_DEG[RC_N_NODES] = {
     0.0,   /* node 0 */
    54.0,   /* node 1 */
   108.0,   /* node 2 */
   168.0,   /* node 3 */
   228.0,   /* node 4 */
   288.0,   /* node 5 */
   348.0    /* node 6 */
};

/* ═══════════════════════════════════════════════════════════════
   24 VERTEX POSITIONS (unit sphere)

   Dodeca A: 12 face centers of regular dodecahedron
   Dodeca B: Dodeca A rotated 36° around X then Y
   All on unit sphere (r = 1.0)
   ═══════════════════════════════════════════════════════════════ */

typedef struct { double x, y, z; } rc_vec3;

/* Dodecahedron A — 12 pentagon face centers (= icosahedron vertices) */
static const rc_vec3 RC_VERTS_A[12] = {
    { +0.00000000, +0.52573111, +0.85065081 },  /*  0 */
    { +0.00000000, -0.52573111, +0.85065081 },  /*  1 */
    { +0.00000000, +0.52573111, -0.85065081 },  /*  2 */
    { +0.00000000, -0.52573111, -0.85065081 },  /*  3 */
    { +0.52573111, +0.85065081, +0.00000000 },  /*  4 */
    { -0.52573111, +0.85065081, +0.00000000 },  /*  5 */
    { +0.52573111, -0.85065081, +0.00000000 },  /*  6 */
    { -0.52573111, -0.85065081, +0.00000000 },  /*  7 */
    { +0.85065081, +0.00000000, +0.52573111 },  /*  8 */
    { -0.85065081, +0.00000000, +0.52573111 },  /*  9 */
    { +0.85065081, +0.00000000, -0.52573111 },  /* 10 */
    { -0.85065081, +0.00000000, -0.52573111 },  /* 11 */
};

/* Dodecahedron B — rotated 36° X then 36° Y */
static const rc_vec3 RC_VERTS_B[12] = {
    { +0.58614413, -0.07467460, +0.80675818 },  /*  0 */
    { +0.22287287, -0.92532540, +0.30675818 },  /*  1 */
    { -0.22287287, +0.92532540, -0.30675818 },  /*  2 */
    { -0.58614413, +0.07467460, -0.80675818 },  /*  3 */
    { +0.71921803, +0.68819096, +0.09549150 },  /*  4 */
    { -0.13143278, +0.68819096, +0.71352549 },  /*  5 */
    { +0.13143278, -0.68819096, -0.71352549 },  /*  6 */
    { -0.71921803, -0.68819096, -0.09549150 },  /*  7 */
    { +0.93819096, -0.30901699, -0.15590452 },  /*  8 */
    { -0.43819096, -0.30901699, +0.84409548 },  /*  9 */
    { +0.43819096, +0.30901699, -0.84409548 },  /* 10 */
    { -0.93819096, +0.30901699, +0.15590452 },  /* 11 */
};

/* Combined 24-vertex table: [0..11] = A, [12..23] = B */
static const rc_vec3 RC_VERTS[RC_N_VERTICES] = {
    /* A: dodecahedron A */
    { +0.00000000, +0.52573111, +0.85065081 },
    { +0.00000000, -0.52573111, +0.85065081 },
    { +0.00000000, +0.52573111, -0.85065081 },
    { +0.00000000, -0.52573111, -0.85065081 },
    { +0.52573111, +0.85065081, +0.00000000 },
    { -0.52573111, +0.85065081, +0.00000000 },
    { +0.52573111, -0.85065081, +0.00000000 },
    { -0.52573111, -0.85065081, +0.00000000 },
    { +0.85065081, +0.00000000, +0.52573111 },
    { -0.85065081, +0.00000000, +0.52573111 },
    { +0.85065081, +0.00000000, -0.52573111 },
    { -0.85065081, +0.00000000, -0.52573111 },
    /* B: dodecahedron B (rotate 36 X then Y) */
    { +0.58614413, -0.07467460, +0.80675818 },
    { +0.22287287, -0.92532540, +0.30675818 },
    { -0.22287287, +0.92532540, -0.30675818 },
    { -0.58614413, +0.07467460, -0.80675818 },
    { +0.71921803, +0.68819096, +0.09549150 },
    { -0.13143278, +0.68819096, +0.71352549 },
    { +0.13143278, -0.68819096, -0.71352549 },
    { -0.71921803, -0.68819096, -0.09549150 },
    { +0.93819096, -0.30901699, -0.15590452 },
    { -0.43819096, -0.30901699, +0.84409548 },
    { +0.43819096, +0.30901699, -0.84409548 },
    { -0.93819096, +0.30901699, +0.15590452 },
};

/* ═══════════════════════════════════════════════════════════════
   RADIAL MESH — 7-node offsets in tangent plane

   For a given vertex, the 7 nodes are placed at angles
   RC_ANGLES_DEG[0..6] in the tangent plane, at angular distance
   RC_RADIUS radians from the vertex.

   These are pre-computed as (cos, sin) pairs for each angle.
   ═══════════════════════════════════════════════════════════════ */

typedef struct { double dx, dy; } rc_offset2;

/* Offsets in tangent plane (unit radius, to be scaled by RC_RADIUS) */
static const rc_offset2 RC_TANGENT_OFFSETS[RC_N_NODES] = {
    { +1.00000000, +0.00000000 },  /* node 0:   0° */
    { +0.58778525, +0.80901699 },  /* node 1:  54° */
    { -0.30901699, +0.95105652 },  /* node 2: 108° */
    { -0.97814760, +0.20791169 },  /* node 3: 168° */
    { -0.66913061, -0.74314483 },  /* node 4: 228° */
    { +0.30901699, -0.95105652 },  /* node 5: 288° */
    { +0.97814760, -0.20791169 },  /* node 6: 348° */
};

/* ═══════════════════════════════════════════════════════════════
   PRE-COMPUTED TANGENT FRAMES

   For each vertex, (U, V) = orthonormal basis in the tangent plane.
   Pre-computed at compile time — no runtime trig needed.
   ═══════════════════════════════════════════════════════════════ */

static const rc_vec3 RC_TANGENT_U[RC_N_VERTICES] = {
    { +0.0000000000, -0.8506508102, +0.5257311091 },
    { +0.0000000000, +0.8506508102, +0.5257311091 },
    { +0.0000000000, +0.8506508102, +0.5257311091 },
    { +0.0000000000, -0.8506508102, +0.5257311091 },
    { +0.0000000000, +0.0000000000, +1.0000000000 },
    { +0.0000000000, +0.0000000000, +1.0000000000 },
    { +0.0000000000, +0.0000000000, +1.0000000000 },
    { +0.0000000000, +0.0000000000, +1.0000000000 },
    { -0.5257311102, +0.0000000000, +0.8506508096 },
    { +0.5257311102, +0.0000000000, +0.8506508096 },
    { +0.5257311102, +0.0000000000, +0.8506508096 },
    { -0.5257311102, +0.0000000000, +0.8506508096 },
    { -0.8002896938, +0.1019566856, +0.5908817481 },
    { -0.0718312407, +0.2982295311, +0.9517874866 },
    { -0.0718312407, +0.2982295311, +0.9517874866 },
    { -0.8002896938, +0.1019566856, +0.5908817481 },
    { -0.0689944964, -0.0660180735, +0.9954302454 },
    { +0.1338520138, -0.7008582326, +0.7006292716 },
    { +0.1338520138, -0.7008582326, +0.7006292716 },
    { -0.0689944964, -0.0660180735, +0.9954302454 },
    { +0.1480789009, -0.0487735420, +0.9877721300 },
    { +0.6898170669, +0.4864664339, +0.5361928971 },
    { +0.6898170669, +0.4864664339, +0.5361928971 },
    { +0.1480789009, -0.0487735420, +0.9877721300 },
};

static const rc_vec3 RC_TANGENT_V[RC_N_VERTICES] = {
    { +1.0000000000, +0.0000000000, -0.0000000000 },
    { -1.0000000000, +0.0000000000, +0.0000000000 },
    { +1.0000000000, -0.0000000000, +0.0000000000 },
    { -1.0000000000, -0.0000000000, +0.0000000000 },
    { +0.8506508098, -0.5257311098, +0.0000000000 },
    { +0.8506508098, +0.5257311098, -0.0000000000 },
    { -0.8506508098, -0.5257311098, +0.0000000000 },
    { -0.8506508098, +0.5257311098, +0.0000000000 },
    { +0.0000000000, -1.0000000000, +0.0000000000 },
    { +0.0000000000, +1.0000000000, -0.0000000000 },
    { +0.0000000000, -1.0000000000, +0.0000000000 },
    { +0.0000000000, +1.0000000000, +0.0000000000 },
    { -0.1263782484, -0.9919821260, +0.0000000000 },
    { -0.9721974883, -0.2341624302, +0.0000000000 },
    { +0.9721974883, +0.2341624302, +0.0000000000 },
    { +0.1263782484, +0.9919821260, +0.0000000000 },
    { +0.6913502615, -0.7225197685, +0.0000000000 },
    { +0.9822469458, +0.1875924769, -0.0000000000 },
    { -0.9822469458, -0.1875924769, +0.0000000000 },
    { -0.6913502615, +0.7225197685, +0.0000000000 },
    { -0.3128423866, -0.9498050543, -0.0000000000 },
    { -0.5763168340, +0.8172263498, -0.0000000000 },
    { +0.5763168340, -0.8172263498, +0.0000000000 },
    { +0.3128423866, +0.9498050543, +0.0000000000 },
};

/* ═══════════════════════════════════════════════════════════════
   GEOMETRY HELPERS
   ═══════════════════════════════════════════════════════════════ */

/* Dot product */
static inline double rc_dot(const rc_vec3 *a, const rc_vec3 *b) {
    return a->x * b->x + a->y * b->y + a->z * b->z;
}

/* Distance squared between two points on sphere */
static inline double rc_dist2(const rc_vec3 *a, const rc_vec3 *b) {
    double dx = a->x - b->x;
    double dy = a->y - b->y;
    double dz = a->z - b->z;
    return dx*dx + dy*dy + dz*dz;
}

/* ═══════════════════════════════════════════════════════════════
   NEAREST VERTEX — parallel CAM (all 24 checked)
   ═══════════════════════════════════════════════════════════════ */

/* Find nearest vertex index to point p on unit sphere.
   Returns 0..23. O(24) comparisons. */
static inline int rc_nearest_vertex(const rc_vec3 *p) {
    int best = 0;
    double best_d2 = rc_dist2(p, &RC_VERTS[0]);
    for (int i = 1; i < RC_N_VERTICES; i++) {
        double d2 = rc_dist2(p, &RC_VERTS[i]);
        if (d2 < best_d2) {
            best_d2 = d2;
            best = i;
        }
    }
    return best;
}

/* ═══════════════════════════════════════════════════════════════
   RADIAL CAPTURE — expand 7-node mesh around nearest vertex

   Input:  (px, py, pz) = point on unit sphere
   Output: uint64_t address = vertex_index * RC_N_NODES + node_index
           with twin bit set for dodeca B vertices

   If point is NULL or zero vector, returns 0.
   ═══════════════════════════════════════════════════════════════ */

static inline uint64_t rc_capture(const rc_vec3 *p) {
    if (!p) return 0;
    double len = sqrt(p->x * p->x + p->y * p->y + p->z * p->z);
    if (len < 1e-12) return 0;

    /* Normalize input to unit sphere */
    rc_vec3 unit = { p->x / len, p->y / len, p->z / len };

    /* Find nearest vertex (parallel CAM) */
    int vi = rc_nearest_vertex(&unit);

    /* Look up pre-computed tangent frame (no trig) */
    const rc_vec3 *tu = &RC_TANGENT_U[vi];
    const rc_vec3 *tv = &RC_TANGENT_V[vi];

    /* Project input onto tangent plane */
    rc_vec3 diff = { unit.x - RC_VERTS[vi].x,
                     unit.y - RC_VERTS[vi].y,
                     unit.z - RC_VERTS[vi].z };
    double proj_u = rc_dot(&diff, tu);
    double proj_v = rc_dot(&diff, tv);

    /* Find closest node in the 7-node mesh */
    int best_node = 0;
    double best_d2 = 1e30;
    for (int n = 0; n < RC_N_NODES; n++) {
        double dx = proj_u - RC_TANGENT_OFFSETS[n].dx;
        double dy = proj_v - RC_TANGENT_OFFSETS[n].dy;
        double d2 = dx*dx + dy*dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best_node = n;
        }
    }

    /* Build address: vertex_index * nodes_per_vertex + node_index */
    uint64_t addr = (uint64_t)vi * RC_N_NODES + (uint64_t)best_node;

    /* Set twin bit for dodeca B vertices (index >= 12) */
    if (vi >= 12) {
        addr |= RC_TWIN_BIT;
    }

    return addr;
}

/* ═══════════════════════════════════════════════════════════════
   TWIN SWAP — O(1) face A ↔ face B

   Flips bit62 to swap between dodeca A and dodeca B.
   ═══════════════════════════════════════════════════════════════ */

static inline uint64_t rc_twin_swap(uint64_t addr) {
    return addr ^ RC_TWIN_BIT;
}

/* Check if address is on dodeca B */
static inline int rc_is_twin(uint64_t addr) {
    return (addr & RC_TWIN_BIT) != 0;
}

/* Get vertex index from address */
static inline int rc_vertex_of(uint64_t addr) {
    return (int)((addr & ~RC_TWIN_BIT) / RC_N_NODES);
}

/* Get node index from address */
static inline int rc_node_of(uint64_t addr) {
    return (int)((addr & ~RC_TWIN_BIT) % RC_N_NODES);
}

/* ═══════════════════════════════════════════════════════════════
   NAME → 3D SPHERE → RADIAL ADDRESS

   Hash tensor name → (x,y,z) on unit sphere → nearest vertex → 
   expand 7-node radial mesh → address 0..167 (with twin bit).

   The hash function (FNV-1a) matches addr_space.h for consistency.
   ═══════════════════════════════════════════════════════════════ */

/* FNV-1a 64-bit hash */
static inline uint64_t rc_hash_str(const char *s) {
    if (!s) return 0;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (const char *p = s; *p; p++) {
        h ^= (uint8_t)(*p);
        h *= 0x00000100000001B3ULL;
    }
    return h;
}

/* Map hash → (x,y,z) on unit sphere.
   Uses two independent halves of the hash for theta and phi,
   producing a deterministic, symmetric, uniform point on the sphere. */
static inline rc_vec3 rc_sphere_from_hash(uint64_t h) {
    double t = 2.0 * 3.14159265358979323846 * (double)(h & 0xFFFF) / 65536.0;
    double p = acos(2.0 * (double)((h >> 16) & 0xFFFF) / 65536.0 - 1.0);
    rc_vec3 pt;
    pt.x = sin(p) * cos(t);
    pt.y = sin(p) * sin(t);
    pt.z = cos(p);
    return pt;
}

/* Capture a tensor name → radial address.
   Hash name → sphere point → rc_capture → address.
   Returns 0 for NULL/empty input. */
static inline uint64_t rc_capture_name(const char *name) {
    if (!name || name[0] == '\0') return 0;
    uint64_t h = rc_hash_str(name);
    rc_vec3 pt = rc_sphere_from_hash(h);
    return rc_capture(&pt);
}

/* Get the twin address of a name's capture (dodeca A ↔ B swap). */
static inline uint64_t rc_capture_name_twin(const char *name) {
    return rc_twin_swap(rc_capture_name(name));
}

/* ═══════════════════════════════════════════════════════════════
   VERIFICATION — compile-time and runtime checks
   ═══════════════════════════════════════════════════════════════ */

/* Compile-time: gap sum = 360 */
#if (RC_GAP_0 + RC_GAP_1 + RC_GAP_2 + RC_GAP_3 + \
     RC_GAP_4 + RC_GAP_5 + RC_GAP_6 != 360)
#  error "GEO_RADIAL_CAPTURE: angular gaps must sum to 360"
#endif

/* Compile-time: vertices * nodes = total addresses */
#if (RC_N_VERTICES * RC_N_NODES != 168)
#  error "GEO_RADIAL_CAPTURE: expected 168 total addresses"
#endif

/* Runtime: verify all 24 vertices on unit sphere */
static inline int rc_verify_vertices(void) {
    for (int i = 0; i < RC_N_VERTICES; i++) {
        double r2 = RC_VERTS[i].x * RC_VERTS[i].x +
                    RC_VERTS[i].y * RC_VERTS[i].y +
                    RC_VERTS[i].z * RC_VERTS[i].z;
        if (fabs(r2 - 1.0) > 1e-6) return 0;
    }
    return 1;
}

/* Runtime: verify no duplicate vertices */
static inline int rc_verify_unique(void) {
    for (int i = 0; i < RC_N_VERTICES; i++) {
        for (int j = i + 1; j < RC_N_VERTICES; j++) {
            if (rc_dist2(&RC_VERTS[i], &RC_VERTS[j]) < 0.01) return 0;
        }
    }
    return 1;
}

/* Runtime: verify twin swap is involutive */
static inline int rc_verify_twin(void) {
    for (uint64_t a = 0; a < 168; a++) {
        if (rc_twin_swap(rc_twin_swap(a)) != a) return 0;
        if (rc_twin_swap(a) == a) return 0;  /* should never be self-twin */
    }
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* GEO_RADIAL_CAPTURE_H */
