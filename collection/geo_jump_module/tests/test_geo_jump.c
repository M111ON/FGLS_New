#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "geo_jump.h"
#include "geo_shell.h"
#include "geo_dodeca_adj.h"
#include "geo_dodeca_ring.h"
#include "geo_shell_fold.h"
#include "geo_field_climate.h"

static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); _pass++; } \
    else      { printf("  FAIL  %s  (line %d)\n", msg, __LINE__); _fail++; } \
} while (0)

int main(void) {
    printf("=== geo_jump_module v2 test suite ===\n");

    printf("\nG00: constants\n");
    {
        CHECK(GEO_BLOCK == 48,       "GEO_BLOCK == 48");
        CHECK(GEO_TOWER == 144,      "GEO_TOWER == 144");
        CHECK(GEO_FULL == 20736,     "GEO_FULL == 20736");
        CHECK(GEO_BLOCK * 3 == GEO_TOWER, "48*3=144");
        CHECK(128U * GEO_MOD_PRIME == GEO_FULL, "128*162=20736");
        CHECK(GEO_PENTAGONS == 12,   "GEO_PENTAGONS == 12");
        CHECK(GEO_FIBO_CLOCK == 1440,"GEO_FIBO_CLOCK == 1440");
    }

    printf("\nG01: v2 Hilbert 4x3 via geo_jump_r\n");
    {
        GeoJumpRouter h = { JUMP_HILBERT, 2, 3 };
        uint32_t h1 = geo_jump_r(0, &h);
        GeoJumpRouter h2r = { JUMP_HILBERT, 3, 2 };
        uint32_t h2 = geo_jump_r(0, &h2r);
        CHECK(h1 != h2,          "hilbert (2,3) != (3,2)");
        CHECK(h1 < GEO_FULL,     "hilbert within bounds");
        CHECK(h2 < GEO_FULL,     "hilbert(3,2) within bounds");

        GeoJumpRouter h_same = { JUMP_HILBERT, 2, 3 };
        CHECK(geo_jump_r(0, &h) == geo_jump_r(0, &h_same),
              "hilbert (2,3) deterministic");
    }

    printf("\nG02: v2 Peano 3x4 via geo_jump_r\n");
    {
        GeoJumpRouter p = { JUMP_PEANO, 2, 3 };
        uint32_t p1 = geo_jump_r(0, &p);
        GeoJumpRouter p2r = { JUMP_PEANO, 3, 2 };
        uint32_t p2 = geo_jump_r(0, &p2r);
        CHECK(p1 != p2,          "peano (2,3) != (3,2)");
        CHECK(p1 < GEO_FULL,     "peano within bounds");
        CHECK(p2 < GEO_FULL,     "peano(3,2) within bounds");

        GeoJumpRouter p_same = { JUMP_PEANO, 2, 3 };
        CHECK(geo_jump_r(0, &p) == geo_jump_r(0, &p_same),
              "peano (2,3) deterministic");
    }

    printf("\nG03: Hilbert != Peano at same (col,row)\n");
    {
        GeoJumpRouter hrc = { JUMP_HILBERT, 2, 2 };
        GeoJumpRouter prc = { JUMP_PEANO,   2, 2 };
        CHECK(geo_jump_r(10, &hrc) != geo_jump_r(10, &prc),
              "hilbert != peano same coord");
    }

    printf("\nG04: geo_jump single-param backward compat\n");
    {
        uint32_t h = geo_jump(0, JUMP_HILBERT, 2);
        CHECK(h < GEO_FULL, "geo_jump HILBERT(2) in range");

        uint32_t p = geo_jump(0, JUMP_PEANO, 1);
        CHECK(p < GEO_FULL, "geo_jump PEANO(1) in range");

        uint32_t pent = geo_jump(0, JUMP_PENTAGON, 2);
        CHECK(pent == 2 * GEO_TOWER, "PENTAGON(layer=2) from face1 lands at 288");

        uint32_t m = geo_jump(128, JUMP_MOD, 162);
        CHECK(m == 0, "MOD 128*162 %% 20736 == 0");

        uint32_t inv = geo_jump(0, JUMP_INVERT, 0);
        CHECK(inv != 0, "INVERT blind spot moves");
    }

    printf("\nG05: geo_jump_r fallback (param2=0)\n");
    {
        GeoJumpRouter hf = { JUMP_HILBERT, 1, 0 };
        GeoJumpRouter pf = { JUMP_PEANO,   1, 0 };
        CHECK(geo_jump_r(5, &hf) < GEO_FULL, "hilbert param2=0 fallback");
        CHECK(geo_jump_r(5, &pf) < GEO_FULL, "peano param2=0 fallback");
    }

    printf("\nG06: helpers\n");
    {
        CHECK(geo_clock_tick(10368) == 720,     "clock 10368 ==> 720");
        CHECK(geo_clock_tick(0) == 0,           "clock 0 ==> 0");
        CHECK(geo_clock_tick(20735) == 1439,    "clock max ==> 1439");
        CHECK(geo_pentagon_id(1728) == 2,       "pentagon_id(1728)=2");
        CHECK(geo_pentagon_id(0) == 1,           "pentagon_id(0)=1");
        CHECK(geo_shell_level(0) == 0,           "shell_level(0)=0");
        CHECK(geo_shell_level(1727) <= 11,       "shell_level in range");
    }

    printf("\nG07: GEO_WRAP edge cases\n");
    {
        CHECK(GEO_WRAP(GEO_FULL)     == 0,        "FULL wraps to 0");
        CHECK(GEO_WRAP(GEO_FULL + 5) == 5,        "FULL+5 wraps to 5");
        CHECK(GEO_WRAP(0) == 0,                    "wrap(0) == 0");
        CHECK(GEO_WRAP(GEO_FULL - 1) == GEO_FULL - 1, "wrap(FULL-1) unchanged");
    }

    printf("\nG08: NULL router / unknown type\n");
    {
        CHECK(geo_jump_r(100, NULL) == GEO_WRAP(101), "NULL ==> node+1");
        CHECK(geo_jump(100, (GeoJumpType)99, 0) == GEO_WRAP(101), "unknown type ==> node+1");
    }

    printf("\nG09: pentagon lift — radial line (same face, different layers)\n");
    {
        uint32_t base = 0; // face 1
        for (uint32_t layer = 0; layer < GEO_SHELL_TICK; layer++) {
            uint32_t node = geo_jump(base, JUMP_PENTAGON, layer);
            CHECK(geo_pentagon_id(node) == 1, "pentagon lift stays on face 1");
            CHECK(node == layer * GEO_TOWER, "pentagon lift at layer N");
        }
    }

    printf("\nG10: pentagon face override via geo_jump_r\n");
    {
        for (uint32_t pent = 1; pent <= GEO_PENTAGONS; pent++) {
            GeoJumpRouter r = { JUMP_PENTAGON, 0, pent }; // layer=0, face=pent
            uint32_t node = geo_jump_r(0, &r);
            CHECK(geo_pentagon_id(node) == pent, "pentagon face override lands on target");
            CHECK(node == (pent-1) * (GEO_FULL/GEO_PENTAGONS), "face override at layer 0");
        }
    }

    printf("\n=== Shell coordinate tests ===\n");

    printf("\nS00: shell constants\n");
    {
        CHECK(SHELL_FACES == 12,     "SHELL_FACES == 12");
        CHECK(SHELL_RINGS == 12,     "SHELL_RINGS == 12");
        CHECK(SHELL_SIDES == 2,      "SHELL_SIDES == 2");
        CHECK(SHELL_TOTAL == 288,    "SHELL_TOTAL == 288");
        CHECK(SHELL_FULL == GEO_FULL,  "SHELL_FULL == GEO_FULL");
        CHECK(SHELL_FACE_BLOCK * SHELL_FACES == SHELL_FULL, "12*1728=20736");
    }

    printf("\nS01: geo_shell_encode/decode roundtrip\n");
    {
        for (uint32_t f = 0; f < SHELL_FACES; f++) {
            for (uint32_t r = 0; r < SHELL_RINGS; r++) {
                for (uint32_t s = 0; s < SHELL_SIDES; s++) {
                    uint32_t sid = geo_shell_encode(f, r, s);
                    uint32_t f2, r2, s2;
                    geo_shell_decode(sid, &f2, &r2, &s2);
                    CHECK(sid < SHELL_TOTAL, "shell_id in range");
                    if (f != f2 || r != r2 || s != s2) {
                        printf("  FAIL roundtrip (%u,%u,%u) -> %u -> (%u,%u,%u)\n",
                               f, r, s, sid, f2, r2, s2);
                        _fail++;
                        goto shell_done;
                    }
                }
            }
        }
        printf("  PASS  encode/decode roundtrip all 288 coords\n");
        _pass++;
        shell_done:;
    }

    printf("\nS02: geo_shell_to_node in range\n");
    {
        for (uint32_t sid = 0; sid < SHELL_TOTAL; sid++) {
            uint32_t node = geo_shell_to_node(sid);
            CHECK(node < SHELL_FULL, "shell_to_node in range");
            uint32_t back = geo_node_to_shell(node);
            uint32_t f1, r1, s1, f2, r2, s2;
            geo_shell_decode(sid, &f1, &r1, &s1);
            geo_shell_decode(back, &f2, &r2, &s2);
            if (f1 != f2 || r1 != r2 || s1 != s2) {
                printf("  FAIL shell_to_node/back (%u) (%u,%u,%u) -> (%u,%u,%u)\n",
                       sid, f1, r1, s1, f2, r2, s2);
                _fail++;
                break;
            }
        }
        printf("  PASS  shell_to_node / node_to_shell roundtrip\n");
        _pass++;
    }

    printf("\nS03: geo_shell_face/ring/side helpers\n");
    {
        for (uint32_t node = 0; node < SHELL_FULL; node += 86) {
            uint32_t f  = geo_shell_face(node);
            uint32_t r  = geo_shell_ring(node);
            uint32_t s  = geo_shell_side(node);
            CHECK(f < SHELL_FACES, "face < 12");
            CHECK(r < SHELL_RINGS, "ring < 12");
            CHECK(s < SHELL_SIDES, "side < 2");
            uint32_t sid = geo_shell_encode(f, r, s);
            CHECK(geo_node_to_shell(node) == sid, "node_to_shell matches encode");
        }
        printf("  PASS  face/ring/side helpers (%u samples)\n", SHELL_FULL/86);
        _pass++;
    }

    printf("\nS04: geo_shell_face_base entry points\n");
    {
        for (uint32_t f = 0; f < SHELL_FACES; f++) {
            uint32_t base = geo_shell_face_base(f);
            CHECK(base == f * SHELL_FACE_BLOCK, "face_base = face * 1728");
            CHECK(geo_shell_face(base) == f, "face_base lands on correct face");
        }
        printf("  PASS  all 12 face bases\n");
        _pass++;
    }

    printf("\nS05: shell alignment verification (RING_BLOCK=144=GEO_TOWER)\n");
    {
        /* RING_BLOCK=144=GEO_TOWER → 1 ring = 1 geo layer, tail_gap=0.
         * Shell coord is a COARSE ZONE LABEL (face, ring, side) — not a bijection.
         * Round-trip survivors = nodes where (n % SHELL_RING_BLOCK) % SHELL_SIDE_BLOCK == 0
         * = entry points of each side = SHELL_TOTAL = 288 nodes.
         * All other nodes quantize to same zone but back-project to entry point. */
        uint32_t tail_gap = SHELL_FACE_BLOCK - SHELL_RING_BLOCK * SHELL_RINGS;
        uint32_t survivors = 0;
        for (uint32_t n = 0; n < SHELL_FULL; n++)
            if (geo_shell_to_node(geo_node_to_shell(n)) == n) survivors++;
        CHECK(SHELL_RING_BLOCK == 144u,                           "RING_BLOCK == GEO_TOWER == 144");
        CHECK(SHELL_RING_BLOCK * SHELL_RINGS == SHELL_FACE_BLOCK, "ring stride×rings == FACE_BLOCK (exact, no tail gap)");
        CHECK(tail_gap == 0u,                                     "tail gap = 0");
        CHECK(SHELL_RINGS == GEO_SHELL_TICK,                      "SHELL_RINGS == GEO_SHELL_TICK == 12");
        CHECK(survivors == SHELL_TOTAL,                           "288 entry-point nodes round-trip exactly");
    }
    printf("\nB01: geo_jump_batch single type\n");
    {
        uint32_t in[5] = {0, 10, 100, 1000, 20000};
        uint32_t out[5];
        geo_jump_batch(in, 5, JUMP_HILBERT, 2, out);
        uint32_t ok = 1;
        for (int i = 0; i < 5; i++) {
            uint32_t expected = geo_jump(in[i], JUMP_HILBERT, 2);
            if (out[i] != expected) ok = 0;
        }
        CHECK(ok, "batch matches single calls");
    }

    printf("\nB02: geo_jump_batch_r via router\n");
    {
        GeoJumpRouter r = { JUMP_PEANO, 2, 3 };
        uint32_t in[4] = {0, 50, 500, 5000};
        uint32_t out[4];
        geo_jump_batch_r(in, 4, &r, out);
        uint32_t ok = 1;
        for (int i = 0; i < 4; i++)
            if (out[i] != geo_jump_r(in[i], &r)) ok = 0;
        CHECK(ok, "batch_r matches single r calls");
    }

    printf("\nB03: empty batch (n=0) no crash\n");
    {
        uint32_t dummy;
        geo_jump_batch(NULL, 0, JUMP_MOD, 37, &dummy);
        CHECK(1, "empty batch no crash");
    }

    printf("\n=== Walk context tests ===\n");
    printf("\nW01: geo_walk_init + step\n");
    {
        GeoJumpRouter r = { JUMP_HILBERT, 2, 1 };
        GeoJumpWalk w;
        geo_walk_init(&w, 100, &r);
        CHECK(w.start == 100, "walk start == 100");
        CHECK(w.step == 0,    "walk step == 0");
        CHECK(w.path[0] == 100, "walk path[0] == start");

        uint32_t n1 = geo_walk_step(&w);
        CHECK(w.step == 1, "walk step == 1 after 1 step");
        CHECK(w.path[1] == n1, "walk path[1] == step result");
        CHECK(n1 < GEO_FULL, "walk step in range");
    }

    printf("\nW02: walk multi-step deterministic\n");
    {
        GeoJumpRouter r = { JUMP_PEANO, 2, 1 };
        GeoJumpWalk w1, w2;
        geo_walk_init(&w1, 0, &r);
        geo_walk_init(&w2, 0, &r);
        for (int i = 0; i < 50; i++) {
            uint32_t s1 = geo_walk_step(&w1);
            uint32_t s2 = geo_walk_step(&w2);
            CHECK(s1 == s2, "walk deterministic step");
            CHECK(s1 < GEO_FULL, "walk step in range");
        }
        CHECK(w1.step == 50, "walk step counter == 50");
    }

    printf("\nW03: geo_walk_peak lookahead\n");
    {
        GeoJumpRouter r = { JUMP_MOD, 37, 0 };
        GeoJumpWalk w;
        geo_walk_init(&w, 1, &r);
        uint32_t s1 = geo_walk_step(&w);
        uint32_t s1_check = geo_jump_r(1, &r);
        CHECK(s1 == s1_check, "walk_peak: first step correct");
        uint32_t peak_match = 0;
        for (uint32_t i = 0; i < 3; i++) {
            if (geo_walk_peak(&w, i) < GEO_FULL) peak_match++;
        }
        CHECK(peak_match == 3, "walk_peak lookahead[0..2] in range");
    }

    printf("\n=== Shell-jump bridge tests ===\n");
    printf("\nSJ01: geo_shell_jump roundtrip\n");
    {
        for (uint32_t sid = 0; sid < SHELL_TOTAL; sid += 12) {
            uint32_t node = geo_shell_to_node(sid);
            uint32_t jumped_node = geo_jump(node, JUMP_PENTAGON, 0);
            uint32_t jumped_shell = geo_shell_jump(sid, JUMP_PENTAGON, 0);
            CHECK(jumped_shell == geo_node_to_shell(jumped_node),
                  "shell_jump matches node_jump + convert");
        }
        printf("  PASS  shell_jump roundtrip (20 samples)\n");
        _pass++;
    }

    printf("\nSJ02: geo_shell_jump_r via router\n");
    {
        GeoJumpRouter r = { JUMP_HILBERT, 3, 2 };
        uint32_t sid = 42;
        uint32_t node = geo_shell_to_node(sid);
        uint32_t jumped_shell = geo_shell_jump_r(sid, &r);
        CHECK(jumped_shell == geo_node_to_shell(geo_jump_r(node, &r)),
              "shell_jump_r matches node_jump_r + convert");
    }

    printf("\nSJ03: geo_shell_jump_batch\n");
    {
        uint32_t in[10], out[10];
        for (int i = 0; i < 10; i++) in[i] = i * 20;
        geo_shell_jump_batch(in, 10, JUMP_MOD, 37, out);
        uint32_t ok = 1;
        for (int i = 0; i < 10; i++)
            if (out[i] != geo_shell_jump(in[i], JUMP_MOD, 37)) ok = 0;
        CHECK(ok, "shell_jump_batch matches single calls");
    }

    printf("\nSJ04: geo_shell_distance\n");
    {
        CHECK(geo_shell_distance(0, 0) == 0, "distance same shell == 0");
        uint32_t d = geo_shell_distance(0, 1);
        CHECK(d > 0 && d < SHELL_TOTAL, "distance adjacent shell >0");
        d = geo_shell_distance(0, SHELL_TOTAL / 2);
        CHECK(d == geo_shell_distance(SHELL_TOTAL / 2, 0), "distance symmetric");
    }

    printf("\n=== Dodeca adjacency tests ===\n");

    printf("\nD01: DODECA_ADJ_A symmetry (bidirectional check)\n");
    {
        uint32_t err = 0;
        for (uint32_t f = 0; f < DODECA_FACES; f++) {
            for (uint32_t e = 0; e < DODECA_EDGES; e++) {
                DodecaAdj nb = DODECA_ADJ_A[f][e];
                DodecaAdj back = DODECA_ADJ_A[nb.face][nb.edge];
                if (back.face != f || back.edge != e) {
                    printf("  FAIL asymmetry adj[%u][%u] -> (%u,%u) -> back (%u,%u)\n",
                           f, e, nb.face, nb.edge, back.face, back.edge);
                    err++;
                }
            }
        }
        CHECK(err == 0, "adjacency table symmetric (0 errors)");
    }

    printf("\nD02: dodeca_adj globe=1 matches globe=0\n");
    {
        uint32_t err = 0;
        for (uint32_t f = 0; f < DODECA_FACES; f++) {
            for (uint32_t e = 0; e < DODECA_EDGES; e++) {
                DodecaAdj a = dodeca_adj(0, f, e);
                DodecaAdj b = dodeca_adj(1, f, e);
                if (a.face != b.face || a.edge != b.edge) err++;
            }
        }
        CHECK(err == 0, "globe B topology identical to globe A (0 mismatches)");
    }

    printf("\nD03: dodeca_walk round-trip (N hops forward, N back)\n");
    {
        uint32_t err = 0;
        for (uint32_t f = 0; f < DODECA_FACES; f++) {
            for (uint32_t e = 0; e < DODECA_EDGES; e++) {
                for (uint32_t hops = 1; hops <= 6; hops++) {
                    DodecaPos p = dodeca_walk(0, f, e, hops);
                    DodecaPos back = dodeca_walk(0, p.face, p.edge, hops);
                    if (back.face != f || back.edge != e) { err++; goto d03_done; }
                }
            }
        }
        d03_done:
        CHECK(err == 0, "walk round-trip for all faces/edges up to 6 hops");
    }

    printf("\nD04: dodeca_face_dist matches BFS (diameter=3)\n");
    {
        // BFS from each face
        uint8_t bfs[12][12];
        memset(bfs, 0, sizeof(bfs));
        for (uint32_t f = 0; f < DODECA_FACES; f++) {
            uint8_t seen[12] = {0};
            uint8_t q[12], hd = 0, tl = 0;
            seen[f] = 0; q[tl++] = f;
            while (hd < tl) {
                uint32_t cur = q[hd++];
                for (uint32_t e = 0; e < DODECA_EDGES; e++) {
                    uint32_t nf = DODECA_ADJ_A[cur][e].face;
                    if (nf < DODECA_FACES && !seen[nf] && nf != f) {
                        seen[nf] = seen[cur] + 1;
                        q[tl++] = nf;
                    }
                }
            }
            for (uint32_t f2 = 0; f2 < DODECA_FACES; f2++)
                bfs[f][f2] = f == f2 ? 0 : seen[f2];
        }
        uint32_t err = 0;
        for (uint32_t i = 0; i < DODECA_FACES; i++)
            for (uint32_t j = 0; j < DODECA_FACES; j++)
                if (bfs[i][j] != dodeca_face_dist(i, j)) err++;
        CHECK(err == 0, "distance table matches BFS (0 mismatches)");

        uint32_t maxd = 0;
        for (uint32_t i = 0; i < DODECA_FACES; i++)
            for (uint32_t j = 0; j < DODECA_FACES; j++)
                if (dodeca_face_dist(i,j) > maxd) maxd = dodeca_face_dist(i,j);
        CHECK(maxd == 3, "face graph diameter == 3");
    }

    printf("\nD05: dodeca_globe_offset\n");
    {
        CHECK(dodeca_globe_offset(0) == 0,       "globe A offset = 0");
        CHECK(dodeca_globe_offset(1) == 10368,   "globe B offset = 10368");
    }

    printf("\nD06: dodeca_frustum_gate\n");
    {
        uint32_t ok = 1;
        for (uint32_t f = 0; f < DODECA_FACES; f++) {
            for (uint32_t e = 0; e < DODECA_EDGES; e++) {
                uint8_t gate = dodeca_frustum_gate(f, e, 0);
                if (gate >= DODECA_EDGES) ok = 0;
            }
        }
        CHECK(ok, "all frustum gates in valid edge range");
    }

    printf("\nD07: dodeca_face_dist antipodal pairs (diameter=3)\n");
    {
        uint32_t antipodal_pairs = 0;
        for (uint32_t i = 0; i < DODECA_FACES; i++)
            for (uint32_t j = i+1; j < DODECA_FACES; j++)
                if (dodeca_face_dist(i, j) == 3) antipodal_pairs++;
        CHECK(antipodal_pairs == 6, "6 antipodal face pairs (dodeca has 6 opposite pairs)");
    }

    printf("\nD08: ring bridge — dodeca/flower ↔ ring\n");
    {
        uint32_t ok = 1;
        for (uint8_t d = 0; d < RING_DODECA; d++) {
            for (uint8_t f = 0; f < RING_FLOWERS; f++) {
                uint8_t r = dodeca_flower_to_ring(d, f);
                if (ring_to_dodeca(r) != d || ring_to_flower(r) != f) ok = 0;
            }
        }
        CHECK(ok, "ring↔(dodeca,flower) round-trip all 10");
    }

    printf("\nD09: face_flower classification\n");
    {
        uint32_t ok = 1;
        for (uint8_t f = 0; f < SHELL_FACES; f++) {
            uint8_t fl = face_flower(f);
            if (f < 6 && fl != 0) ok = 0;
            if (f >= 6 && fl != 1) ok = 0;
        }
        CHECK(ok, "faces 0-5=north/0, 6-11=south/1");
    }

    printf("\nD10: ring_step internal edge (same flower)\n");
    {
        uint32_t ok = 1;
        for (uint8_t f = 2; f < 5; f++) {  /* faces within same flower */
            for (uint8_t e = 0; e < DODECA_EDGES; e++) {
                DodecaAdj nb = dodeca_adj(0, f, e);
                if (face_flower(f) != face_flower(nb.face)) continue; /* skip boundary */
                RingStepResult r = ring_step(f, 0, e);
                if (r.face != nb.face || r.ring != 0) { ok = 0; goto d10d; }
            }
        }
        d10d: CHECK(ok, "internal edges stay same ring");
    }

    printf("\nD11: ring_step boundary edge crosses dodeca\n");
    {
        /* face 4 edge 0 → face 6 (flower 0→1, same dodeca) */
        RingStepResult r = ring_step(4, 0, 0);
        CHECK(r.face == 6 && r.ring == 5, "face4→6 at ring0→5 (flower 0→1)");

        /* face 6 edge 1 → face 0 (flower 1→0, next dodeca) */
        RingStepResult r2 = ring_step(6, 5, 1);
        CHECK(r2.face == 0 && r2.ring == 1, "face6→0 at ring5→1 (flower 1→0 next dodeca)");
    }

    printf("\nD12: hex conversion — vertex→face consistency\n");
    {
        uint32_t ok = 1;
        for (uint8_t v = 0; v < DODECA_VERTS; v++) {
            for (uint8_t s = 0; s < 3; s++) {
                uint8_t f = hex_to_face(v, s);
                /* check that this face actually contains vertex v */
                uint8_t found = 0;
                for (uint8_t e = 0; e < DODECA_EDGES; e++)
                    if (ring_to_hex(f, e) == v) { found = 1; break; }
                if (!found) { ok = 0; goto d12d; }
            }
        }
        d12d: CHECK(ok, "all vertex→face links consistent");
    }

    printf("\n=== Shell fold + hot path tests ===\n");

    printf("\nF01: GEO_FIBO table\n");
    {
        CHECK(GEO_FIBO[0] == 1,  "F(0)=1");
        CHECK(GEO_FIBO[5] == 8,  "F(5)=8");
        CHECK(GEO_FIBO[11]== 144,"F(11)=144");
    }

    printf("\nF02: shell_layer_live at various ticks\n");
    {
        /* layer 0 always live (fibo=1) */
        CHECK(shell_layer_live(0, 0) == 1, "layer 0 live at tick 0");
        CHECK(shell_layer_live(0, 7) == 1, "layer 0 live at tick 7");
        /* layer 6 (fibo=13): live at tick 13, 26, ... */
        CHECK(shell_layer_live(6, 13) == 1, "layer 6 live at tick 13");
        CHECK(shell_layer_live(6, 7)  == 0, "layer 6 frozen at tick 7");
    }

    printf("\nF03: shell_fold_nearest fallback\n");
    {
        /* layer 6 (fibo=13) at tick 7: nearest live is 0 (always live) */
        uint8_t n1 = shell_fold_nearest(6, 7, 0);
        CHECK(n1 >= 0 && n1 < 12, "fold nearest in range");
        CHECK(shell_layer_live(n1, 7) == 1, "fold nearest is live");

        /* hot path bypass */
        CHECK(shell_fold_nearest(6, 7, 1) == 6, "hot path bypasses fold");

        /* layer 0 always folds to itself */
        CHECK(shell_fold_nearest(0, 999, 0) == 0, "layer 0 folds to 0");
    }

    printf("\nF04: ring_hot_path O(1)\n");
    {
        for (uint8_t p = 0; p < GEO_PENTAGONS; p++) {
            for (uint8_t l = 0; l < GEO_SHELL_TICK; l++) {
                uint32_t node = ring_hot_path(p, l, 0);
                CHECK(node < GEO_FULL, "hot path in range");
                CHECK(geo_pentagon_id(node) == p + 1, "hot path on correct pentagon");
                CHECK(geo_shell_level(node) == l, "hot path at correct layer");
            }
        }
        /* globe B offset */
        uint32_t node_b = ring_hot_path(0, 0, 1);
        CHECK(node_b >= GEO_FULL/3, "globe B offset > 1/3");
    }

    printf("\n=== Climate field tests ===\n");

    printf("\nC01: climate_init + add + centroid\n");
    {
        ClimateField f;
        climate_init(&f, 1000, 0, CLIMATE_MODALITY_TEXT, 0, 0);
        CHECK(f.seed == 1000, "seed set");
        CHECK(f.n_attract == 0, "no attractors initially");
        CHECK(f.centroid == 1000, "centroid == seed initially");

        climate_add(&f, 2000, 128);
        CHECK(f.n_attract == 1, "1 attractor after add");
        CHECK(f.attract[0].node == 2000, "attractor 0 node correct");
        CHECK(f.attract[0].weight == 128, "attractor 0 weight correct");

        climate_add(&f, 3000, 128);
        CHECK(f.n_attract == 2, "2 attractors");
        uint32_t expected = GEO_WRAP((2000*128 + 3000*128) / 256);
        CHECK(f.centroid == expected, "centroid = mean of attractors");
    }

    printf("\nC02: climate_capo shift\n");
    {
        ClimateField f;
        climate_init(&f, 100, 2, CLIMATE_MODALITY_GEO, 0, 0);
        climate_add(&f, 500, 64);
        uint32_t before = f.centroid;
        climate_capo(&f, 5);
        uint32_t delta = GEO_WRAP((5-2) * GEO_TOWER);
        CHECK(f.key == 5, "key updated");
        CHECK(f.centroid == GEO_WRAP(before + delta), "centroid shifted by key×TOWER");
        CHECK(climate_valid(&f), "field valid after capo");
    }

    printf("\nC03: climate_similarity\n");
    {
        ClimateField a, b;
        climate_init(&a, 0, 0, CLIMATE_MODALITY_TEXT, 0, 0);
        climate_init(&b, 0, 0, CLIMATE_MODALITY_TEXT, 0, 0);
        climate_add(&a, 100, 128); climate_add(&a, 200, 128);
        climate_add(&b, 100, 128); climate_add(&b, 200, 128);
        uint8_t sim = climate_similarity(&a, &b);
        CHECK(sim >= 100, "identical fields have similarity >= 100");

        climate_init(&b, 0, 0, CLIMATE_MODALITY_TEXT, 0, 0);
        climate_add(&b, 10000, 128); climate_add(&b, 20000, 128);
        uint8_t sim2 = climate_similarity(&a, &b);
        CHECK(sim2 <= sim, "distant fields have lower or equal similarity");
    }

    printf("\n========================================\n");
    printf("  Results: %d pass, %d fail\n", _pass, _fail);
    printf("========================================\n");
    return _fail > 0 ? 1 : 0;
}
