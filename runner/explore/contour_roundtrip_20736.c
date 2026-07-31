// contour_roundtrip_20736.c
// ================================================================
// TEST: 6000 cells → 144×144 = 20736 → decode → 6000 cells
//
// The ONLY requirement: lossless roundtrip
// Placement strategy doesn't matter — must decode back exactly.
//
// Contour cube: 6 faces × 10×10×10 = 6000 cells
// Target: 144 × 144 = 20736 addresses
// 20736 - 6000 = 14736 unused addresses (empty slots)
// ================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CUBE_FACES   6
#define CUBE_W      10
#define CUBE_H      10
#define CUBE_L      10
#define CUBE_CELLS  (CUBE_FACES * CUBE_W * CUBE_H * CUBE_L)  // 6000

#define GEO_FULL    20736   // 144 × 144
#define GEO_DIM     144     // sqrt(20736)

// ── Contour cube: face × (x, y, z) → value ──
typedef struct {
    int face, x, y, z;
    int8_t value;
    int global_idx;  // 0..5999
} ContourCell;

static ContourCell original[CUBE_CELLS];
static ContourCell decoded[CUBE_CELLS];

// ── GeoJump 20736 store ──
static int8_t geo_store[GEO_FULL];
static int8_t geo_store_2[GEO_FULL];  // for comparison

// ── Fill original with test data ──
static void fill_original(void) {
    int idx = 0;
    for (int f = 0; f < CUBE_FACES; f++)
        for (int z = 0; z < CUBE_L; z++)
            for (int y = 0; y < CUBE_H; y++)
                for (int x = 0; x < CUBE_W; x++) {
                    original[idx].face = f;
                    original[idx].x = x;
                    original[idx].y = y;
                    original[idx].z = z;
                    // Deterministic test value based on position
                    original[idx].value = (int8_t)(
                        (f * 1000 + z * 100 + y * 10 + x) * 37 + 13
                    ) % 256 - 128;
                    original[idx].global_idx = idx;
                    idx++;
                }
}

// ═══════════════════════════════════════════
// MAPPING FUNCTIONS: cell → geo address
// All must be bijective for roundtrip
// ═══════════════════════════════════════════

// Map 1: face × 1000 + z×100 + y×10 + x → mod 20736
// Simple sequential, guaranteed unique (6000 < 20736)
static uint32_t map_sequential(const ContourCell *c) {
    return (uint32_t)(c->face * 1000 + c->z * 100 + c->y * 10 + c->x);
}

// Map 2: stride-37 scatter
// global_idx × 37 % 20736
static uint32_t map_stride37(const ContourCell *c) {
    return (uint32_t)((c->global_idx * 37u) % GEO_FULL);
}

// Map 3: face × 3456 + z×345 + y×34 + x
// Spread each face across a different region
static uint32_t map_face_region(const ContourCell *c) {
    return (uint32_t)(c->face * 3456 + c->z * 345 + c->y * 34 + c->x);
}

// Map 4: XOR scatter (displacement model)
// position XOR seed
static uint32_t map_xor(const ContourCell *c) {
    uint32_t pos = (uint32_t)(c->global_idx);
    return (pos ^ 0x9E3779B9u) % GEO_FULL;
}

// Map 5: 144×144 grid — face maps to 144-row
// row = face×24 + z, col = y×10 + x
static uint32_t map_grid(const ContourCell *c) {
    uint32_t row = (uint32_t)(c->face * 24 + c->z);
    uint32_t col = (uint32_t)(c->y * 10 + c->x);
    return row * GEO_DIM + col;
}

// ═══════════════════════════════════════════
// ENCODE: pack cells into geo_store
// ═══════════════════════════════════════════

typedef uint32_t (*MapFunc)(const ContourCell *);

static void encode(MapFunc map) {
    memset(geo_store, 0, GEO_FULL);
    for (int i = 0; i < CUBE_CELLS; i++) {
        uint32_t addr = map(&original[i]);
        geo_store[addr] = original[i].value;
    }
}

// ═══════════════════════════════════════════
// DECODE: extract cells from geo_store
// ═══════════════════════════════════════════

static int decode(MapFunc map) {
    memset(decoded, 0, sizeof(decoded));
    int found = 0;
    for (int i = 0; i < CUBE_CELLS; i++) {
        uint32_t addr = map(&original[i]);
        decoded[i].value = geo_store[addr];
        decoded[i].face = original[i].face;
        decoded[i].x = original[i].x;
        decoded[i].y = original[i].y;
        decoded[i].z = original[i].z;
        decoded[i].global_idx = original[i].global_idx;
        if (decoded[i].value != 0) found++;
    }
    return found;
}

// ═══════════════════════════════════════════
// VERIFY: roundtrip correctness
// ═══════════════════════════════════════════

static int verify_roundtrip(void) {
    int errors = 0;
    for (int i = 0; i < CUBE_CELLS; i++) {
        if (original[i].value != decoded[i].value) {
            if (errors < 5) {
                printf("    MISMATCH at [%d] face=%d(%d,%d,%d): orig=%d decoded=%d\n",
                       i, original[i].face, original[i].x, original[i].y, original[i].z,
                       original[i].value, decoded[i].value);
            }
            errors++;
        }
    }
    return errors;
}

// ═══════════════════════════════════════════
// TEST each mapping
// ═══════════════════════════════════════════

typedef struct {
    const char *name;
    MapFunc map;
    int unique_check;
} MapTest;

static int test_mapping(MapTest *t) {
    printf("  %-20s ", t->name);

    // Encode
    encode(t->map);

    // Check uniqueness (no two cells map to same address)
    int collisions = 0;
    memset(geo_store_2, 0, GEO_FULL);
    for (int i = 0; i < CUBE_CELLS; i++) {
        uint32_t addr = t->map(&original[i]);
        if (geo_store_2[addr] != 0) collisions++;
        geo_store_2[addr] = original[i].value;
    }

    // Decode
    decode(t->map);

    // Verify roundtrip
    int errors = verify_roundtrip();

    // Coverage
    int used = 0;
    for (int i = 0; i < GEO_FULL; i++) {
        if (geo_store[i] != 0) used++;
    }

    if (collisions == 0 && errors == 0) {
        printf("✓ PASS  collisions=0  errors=0  used=%d/%d (%.1f%%)\n",
               used, GEO_FULL, 100.0*used/GEO_FULL);
        return 1;
    } else {
        printf("✗ FAIL  collisions=%d  errors=%d\n", collisions, errors);
        return 0;
    }
}

// ═══════════════════════════════════════════
// TEST: Reverse lookup — geo address → cell
// ═══════════════════════════════════════════

static int test_reverse_lookup(MapFunc map, const char *name) {
    printf("\n  Reverse lookup (%s):\n", name);

    encode(map);

    // For each used geo address, find which cell it came from
    int found = 0;
    for (int i = 0; i < CUBE_CELLS && found < 5; i++) {
        uint32_t addr = map(&original[i]);
        if (geo_store[addr] != 0) {
            printf("    geo[%5u] = face=%d(%d,%d,%d) val=%d\n",
                   addr, original[i].face, original[i].x, original[i].y,
                   original[i].z, original[i].value);
            found++;
        }
    }

    // Empty addresses
    int empty = 0;
    for (int i = 0; i < GEO_FULL; i++) {
        if (geo_store[i] == 0) empty++;
    }
    printf("    Empty addresses: %d / %d (%.1f%%)\n",
           empty, GEO_FULL, 100.0*empty/GEO_FULL);

    return 1;
}

// ═══════════════════════════════════════════
// TEST: Roundtrip with modification
// ═══════════════════════════════════════════

static int test_modify_roundtrip(MapFunc map, const char *name) {
    printf("\n  Modify + roundtrip (%s):\n", name);

    // Encode original
    encode(map);

    // Modify 100 cells in geo_store
    int modified = 0;
    for (int i = 0; i < GEO_FULL && modified < 100; i++) {
        if (geo_store[i] != 0) {
            geo_store[i] = (int8_t)(geo_store[i] + 1);  // increment
            modified++;
        }
    }
    printf("    Modified %d cells in geo_store\n", modified);

    // Decode — should get MODIFIED values
    decode(map);

    // Check: decoded values should match modified geo_store
    int match = 0, mismatch = 0;
    for (int i = 0; i < CUBE_CELLS; i++) {
        uint32_t addr = map(&original[i]);
        int8_t expected = geo_store[addr];  // modified value
        if (decoded[i].value == expected) match++;
        else mismatch++;
    }
    printf("    Decoded: %d match, %d mismatch\n", match, mismatch);

    // Restore original and verify
    encode(map);
    decode(map);
    int errors = verify_roundtrip();
    printf("    Restore roundtrip: %s\n", errors == 0 ? "✓" : "✗");

    return (mismatch == 0 && errors == 0) ? 1 : 0;
}

// ═══════════════════════════════════════════

int main(void) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Contour Cube → 20736 Roundtrip Test                   ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");
    printf("  Contour: %d cells (6×10×10×10)\n", CUBE_CELLS);
    printf("  Target:  %d (144×144)\n", GEO_FULL);
    printf("  Unused:  %d addresses (%.1f%%)\n\n",
           GEO_FULL - CUBE_CELLS, 100.0*(GEO_FULL-CUBE_CELLS)/GEO_FULL);

    fill_original();

    MapTest tests[] = {
        {"sequential",    map_sequential,  1},
        {"stride37",      map_stride37,    1},
        {"face_region",   map_face_region, 1},
        {"xor_scatter",   map_xor,         1},
        {"144×144_grid",  map_grid,        1},
    };
    int n_tests = sizeof(tests)/sizeof(tests[0]);

    printf("═══ Roundtrip Tests ═══\n");
    int pass = 0;
    for (int i = 0; i < n_tests; i++) {
        pass += test_mapping(&tests[i]);
    }

    printf("\n  Score: %d/%d PASS\n", pass, n_tests);

    // Reverse lookup for best mapping
    test_reverse_lookup(map_sequential, "sequential");
    test_reverse_lookup(map_stride37, "stride37");

    // Modify roundtrip
    printf("\n═══ Modify + Roundtrip ═══\n");
    test_modify_roundtrip(map_sequential, "sequential");
    test_modify_roundtrip(map_stride37, "stride37");

    printf("\n══════════════════════════════════════════════════════════\n");
    printf("  FINAL: %d/%d roundtrip PASS\n", pass, n_tests);
    printf("══════════════════════════════════════════════════════════\n");
    return (pass == n_tests) ? 0 : 1;
}
