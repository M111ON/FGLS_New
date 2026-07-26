/*
 * beam_field.c — Geometric Field Storage Prototype (v3)
 * ═══════════════════════════════════════════════════════════════════
 *
 * Bijection ผ่าน stride-37 permutation บน icosahedron grid (2304 slots)
 * bake(weight) → coord   ↔   decode(coord) → weight  :: lossless
 * ═══════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ── Icosahedron grid ───────────────────────────────────────── */
#define BF_FACES      12u   /* 12 pentagonal faces */
#define BF_SLOTS      12u   /* 12 slots per face */
#define BF_ICO_IDX    16u   /* 16 icosphere sub-positions per slot */
#define BF_GRID       (BF_FACES * BF_SLOTS * BF_ICO_IDX)  /* 2304 total positions */
#define BF_GRID_STRIDE 37u  /* stride coprime with 2304 (gcd=1) */
#define BF_GRID_INV   685u  /* modinv(37, 2304) = 685 (685*37 ≡ 1 mod 2304) */

typedef struct {
    uint8_t face;     /* 0..11 */
    uint8_t slot;     /* 0..11 */
    uint8_t ixo;      /* 0..15 */
} Coord;              /* 3 bytes */

/* ── Coordinate ↔ Index ─────────────────────────────────────── */
static inline uint32_t coord_to_idx(Coord c) {
    return (uint32_t)c.face * (BF_SLOTS * BF_ICO_IDX)
         + (uint32_t)c.slot * BF_ICO_IDX
         + (uint32_t)c.ixo;
}

static inline Coord idx_to_coord(uint32_t idx) {
    Coord c;
    c.face = (uint8_t)(idx / (BF_SLOTS * BF_ICO_IDX));
    c.slot = (uint8_t)((idx / BF_ICO_IDX) % BF_SLOTS);
    c.ixo  = (uint8_t)(idx % BF_ICO_IDX);
    return c;
}

/* ── BEAM encode/decode (bijection through stride-37) ──────────
 *
 * bake:  weight(-128..+127) → u8(0..255) → stride-37 × 2304 → coord
 * decode: coord → idx → stride-37⁻¹ → u8 → weight
 *
 * "Coordinate IS the data" — the icosahedron position IS the value,
 *   not just an index. The geometry of the icosahedron (face rotation,
 *   slot adjacency, icosphere subdivision) gives each position a
 *   UNIQUE geometric identity that IS the weight.
 */

/* weight → Coord (bijection) */
static inline Coord beam_bake(int32_t weight) {
    uint32_t u = (uint32_t)(uint8_t)((weight + 128) & 0xFF);
    uint32_t idx = (u * BF_GRID_STRIDE) % BF_GRID;
    return idx_to_coord(idx);
}

/* Coord → weight (inverse bijection) */
static inline int32_t beam_decode(Coord c) {
    uint32_t idx = coord_to_idx(c);
    uint32_t u = (idx * BF_GRID_INV) % BF_GRID;
    /* แค่ 256/2304 indices ใช้สำหรับ Q8 — decode จากตรงนี้ */
    return (int32_t)(int8_t)((u + 128) & 0xFF);
}

/* ── Alternative: XOR beam (faster, no modulo) ────────────────
 * weight = coord bits XOR'd with geometric key
 * แบบนี้ไม่ต้องใช้ modulo arithmetic — แค่ XOR + shift
 * แต่ต้อง calibrate key ให้ produce bijection */

static Coord bake_xor(int32_t weight) {
    uint8_t u = (uint8_t)(weight + 128);  /* 0..255 */
    Coord c;
    /* Scatter bits across face/slot/ixo */
    c.face = (uint8_t)(((u ^ 0xB4) + (u >> 3)) & 0x0F);
    if (c.face >= BF_FACES) c.face = (uint8_t)(BF_FACES - 1 - (c.face - BF_FACES));
    c.slot = (uint8_t)(((u >> 1) ^ (u << 2)) & 0x0F);
    if (c.slot >= BF_SLOTS) c.slot = (uint8_t)(BF_SLOTS - 1 - (c.slot - BF_SLOTS));
    c.ixo  = (uint8_t)((u ^ (u >> 2)) & 0x0F);
    return c;
}

static int32_t decode_xor(Coord c) {
    uint8_t face = c.face;
    uint8_t slot = c.slot;
    uint8_t ixo  = c.ixo;
    /* Invert the bake_xor transformations... 
     * Complex inverse. Skip for now — use stride-37 approach instead. */
    (void)face; (void)slot; (void)ixo;
    return 0;  /* TODO: proper XOR inverse */
}

/* ── Field (sparse storage) ─────────────────────────────────── */

typedef struct {
    Coord   pos;
    uint8_t flags;
} FieldSlot;

#define FIELD_SIZE    BF_GRID

typedef struct {
    FieldSlot slots[FIELD_SIZE];
    uint32_t occupied;
} Field;

static void field_init(Field *f) {
    memset(f, 0, sizeof(Field));
    f->occupied = 0;
}

static int field_store(Field *f, int32_t weight) {
    Coord c = beam_bake(weight);
    uint32_t idx = coord_to_idx(c);
    if (!f->slots[idx].flags) f->occupied++;
    f->slots[idx].pos = c;
    f->slots[idx].flags = 1;
    return 0;
}

static int32_t field_load(Field *f, Coord c) {
    uint32_t idx = coord_to_idx(c);
    if (!f->slots[idx].flags) return 0;
    return beam_decode(f->slots[idx].pos);
}

/* ── Verify ─────────────────────────────────────────────────── */

static int verify(void) {
    int pass = 0, fail = 0;
#define T(expr, msg) do { \
    if (expr) { pass++; printf("  PASS  %s\n", msg); } \
    else { fail++; printf("  FAIL  %s (line %d)\n", msg, __LINE__); } \
} while(0)

    printf("=== Verify (v3) ===\n");

    /* [T1] Full Q8 lossless roundtrip */
    {
        int ok = 1;
        for (int32_t w = -128; w <= 127; w++) {
            Coord c = beam_bake(w);
            int32_t r = beam_decode(c);
            if (r != w) { ok = 0; break; }
        }
        T(ok, "full Q8 lossless roundtrip: bake→decode = weight");
    }

    /* [T2] Deterministic */
    {
        Coord c1 = beam_bake(42);
        Coord c2 = beam_bake(42);
        T(c1.face == c2.face && c1.slot == c2.slot && c1.ixo == c2.ixo,
          "deterministic: same weight → same coord");
    }

    /* [T3] Zero collisions — 256 unique coords for 256 values */
    {
        uint8_t seen[BF_GRID] = {0};
        int collisions = 0;
        for (int32_t w = -128; w <= 127; w++) {
            Coord c = beam_bake(w);
            uint32_t idx = coord_to_idx(c);
            if (seen[idx]) collisions++;
            seen[idx] = 1;
        }
        T(collisions == 0, "zero collisions: 256 values → 256 unique coords");
    }

    /* [T4] All 256 values reachable by decode */
    {
        int vals[256] = {0};
        for (uint32_t i = 0; i < BF_GRID; i++) {
            Coord c = idx_to_coord(i);
            int32_t v = beam_decode(c);
            if (v >= -128 && v <= 127) {
                uint8_t u = (uint8_t)(v + 128);
                vals[u]++;
            }
        }
        int non_zero = 0;
        for (int i = 0; i < 256; i++) if (vals[i]) non_zero++;
        T(non_zero == 256, "all 256 Q8 values reachable");
    }

    /* [T5] field store/load */
    {
        Field f;
        field_init(&f);
        int ok = 1;
        for (int32_t w = -128; w <= 127; w++) field_store(&f, w);
        for (int32_t w = -128; w <= 127; w++) {
            Coord c = beam_bake(w);
            int32_t r = field_load(&f, c);
            if (r != w) { ok = 0; break; }
        }
        T(ok, "field store/load roundtrip: 256 values stored and loaded");
    }

    /* [T6] Storage size check */
    {
        T(sizeof(Coord) == 3, "Coord = 3 bytes (face+slot+ixo)");
        T(sizeof(FieldSlot) == 4, "FieldSlot = 4 bytes");
    }

    return fail;
}

/* ── Demo ───────────────────────────────────────────────────── */

static void demo(void) {
    printf("\n═══ Geometric Field Demo (v3) ═══\n");
    printf("  stride-37 bijection on 2304-slot icosahedron grid\n");
    printf("  bake: weight → coord :: decode: coord → weight  ✓\n\n");
    
    printf("  %6s | %4s %4s %4s | %6s : %s\n",
           "weight", "face", "slot", "ixo", "decode", "match");
    printf("  " "------" "-" "----" "-" "----" "-" "----" "-" "------" "---" "-----" "\n");
    
    int32_t tests[] = {0, 1, -1, 42, -42, 127, -128, 64, -64, 100};
    int n = sizeof(tests) / sizeof(tests[0]);
    for (int i = 0; i < n; i++) {
        int32_t w = tests[i];
        Coord c = beam_bake(w);
        int32_t d = beam_decode(c);
        printf("  %6d | %4d %4d %4d | %6d : %s\n",
               w, c.face, c.slot, c.ixo, d, d == w ? "✓" : "✗");
    }
    
    /* Compression comparison */
    printf("\n  Per-weight storage:\n");
    printf("    Q8 raw:         1 byte  (8 bits fixed)\n");
    printf("    BeamCode (Step1): 1 byte  (zone+position=8bit)\n");
    printf("    Geometric field:  3 bytes but flexible\n");
    printf("\n  Why 3 bytes when BeamCode is 1 byte?\n");
    printf("    BeamCode uses 1 byte because it's an INDEX.\n");
    printf("    Geometric field uses 3 bytes because it's a POSITION\n");
    printf("    on the icosahedron — it carries GEOMETRIC meaning:\n");
    printf("    face adjacency, slot distance, icosphere proximity.\n");
    printf("    For compression: pack 3 bytes into slot index → 8 bits.\n");
    printf("    For Field-as-Model: the 3 bytes ARE the model.\n");
}

int main(void) {
    printf("Beam Field v3 — stride-37 Bijection\n");
    printf("====================================\n\n");
    
    int r = verify();
    demo();
    
    printf("\n%s\n", r ? "FAIL" : "✓ ALL PASS — stride-37 bijection is lossless.");
    return r;
}
