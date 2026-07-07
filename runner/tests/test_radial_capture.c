/*
 * test_radial_capture.c — Build + verify geo_radial_capture.h
 */

#include <stdio.h>
#include <math.h>
#include "geo_radial_capture.h"

#define PASS(name) printf("  [PASS] %s\n", name)
#define FAIL(name, msg) do { printf("  [FAIL] %s: %s\n", name, msg); fail++; } while(0)

int main(void) {
    int fail = 0;
    int pass = 0;

    printf("=== geo_radial_capture tests ===\n\n");

    /* Test 1: compile-time gap sum */
    {
        int sum = RC_GAP_0 + RC_GAP_1 + RC_GAP_2 + RC_GAP_3 +
                  RC_GAP_4 + RC_GAP_5 + RC_GAP_6;
        if (sum == 360) { PASS("gap sum = 360"); pass++; }
        else { char buf[32]; snprintf(buf, 32, "got %d", sum);
               FAIL("gap sum = 360", buf); }
    }

    /* Test 2: all vertices on unit sphere */
    {
        if (rc_verify_vertices()) { PASS("24 vertices on unit sphere"); pass++; }
        else { FAIL("24 vertices on unit sphere", "radius mismatch"); }
    }

    /* Test 3: no duplicate vertices */
    {
        if (rc_verify_unique()) { PASS("24 unique vertices (no overlap)"); pass++; }
        else { FAIL("24 unique vertices", "duplicates found"); }
    }

    /* Test 4: twin swap is involutive */
    {
        if (rc_verify_twin()) { PASS("twin swap involutive (x^b^b = x)"); pass++; }
        else { FAIL("twin swap involutive", "failed"); }
    }

    /* Test 5: pre-computed tangent frames orthonormal */
    {
        int ok = 1;
        for (int i = 0; i < RC_N_VERTICES; i++) {
            double d = rc_dot(&RC_TANGENT_U[i], &RC_TANGENT_V[i]);
            if (fabs(d) > 1e-6) { ok = 0; break; }
            double u_len = sqrt(rc_dot(&RC_TANGENT_U[i], &RC_TANGENT_U[i]));
            double v_len = sqrt(rc_dot(&RC_TANGENT_V[i], &RC_TANGENT_V[i]));
            if (fabs(u_len - 1.0) > 1e-6 || fabs(v_len - 1.0) > 1e-6) {
                ok = 0; break;
            }
        }
        if (ok) { PASS("pre-computed tangent frames orthonormal"); pass++; }
        else { FAIL("pre-computed tangent frames orthonormal", "bad frame"); }
    }

    /* Test 6: NULL input → 0 */
    {
        uint64_t r = rc_capture(NULL);
        if (r == 0) { PASS("NULL input → 0"); pass++; }
        else { FAIL("NULL input → 0", "non-zero"); }
    }

    /* Test 7: zero input → 0 */
    {
        rc_vec3 zero = {0, 0, 0};
        uint64_t r = rc_capture(&zero);
        if (r == 0) { PASS("zero vector → 0"); pass++; }
        else { FAIL("zero vector → 0", "non-zero"); }
    }

    /* Test 8: each vertex is its own nearest */
    {
        int ok = 1;
        for (int i = 0; i < RC_N_VERTICES; i++) {
            int vi = rc_nearest_vertex(&RC_VERTS[i]);
            if (vi != i) { ok = 0; printf("    vertex %d → nearest %d\n", i, vi); break; }
        }
        if (ok) { PASS("each vertex is its own nearest"); pass++; }
        else { FAIL("each vertex is its own nearest", ""); }
    }

    /* Test 9: capture on each vertex → address in expected range */
    {
        int ok = 1;
        for (int i = 0; i < RC_N_VERTICES; i++) {
            uint64_t addr = rc_capture(&RC_VERTS[i]);
            int vi = rc_vertex_of(addr);
            if (vi != i) { ok = 0; printf("    vertex %d → addr vertex %d\n", i, vi); break; }
        }
        if (ok) { PASS("capture on vertex → correct vertex index"); pass++; }
        else { FAIL("capture on vertex → correct vertex index", ""); }
    }

    /* Test 10: address space = 24 * 7 = 168 unique addresses */
    {
        int seen[168] = {0};
        int unique = 0;
        for (int i = 0; i < RC_N_VERTICES; i++) {
            uint64_t addr = rc_capture(&RC_VERTS[i]);
            uint64_t idx = addr & ~RC_TWIN_BIT;
            if (idx < 168 && !seen[idx]) {
                seen[idx] = 1;
                unique++;
            }
        }
        if (unique == 24) { PASS("24 unique base addresses (one per vertex)"); pass++; }
        else { char buf[32]; snprintf(buf, 32, "got %d", unique);
               FAIL("24 unique base addresses", buf); }
    }

    /* Test 11: twin swap changes twin bit, preserves vertex+node */
    {
        int ok = 1;
        for (int i = 0; i < RC_N_VERTICES; i++) {
            uint64_t addr = rc_capture(&RC_VERTS[i]);
            uint64_t twin = rc_twin_swap(addr);
            /* Twin should flip bit62 */
            if ((twin & RC_TWIN_BIT) == (addr & RC_TWIN_BIT)) { ok = 0; break; }
            /* Vertex and node indices should stay the same */
            if (rc_vertex_of(twin) != rc_vertex_of(addr)) { ok = 0; break; }
            if (rc_node_of(twin) != rc_node_of(addr)) { ok = 0; break; }
        }
        if (ok) { PASS("twin swap flips bit62, preserves vertex+node"); pass++; }
        else { FAIL("twin swap flips bit62", ""); }
    }

    /* Test 12: geometric twin — each A vertex's nearest B vertex */
    {
        int ok = 1;
        for (int i = 0; i < 12; i++) {
            /* Find nearest B vertex to A[i] */
            int best_j = 0;
            double best_d2 = rc_dist2(&RC_VERTS_A[i], &RC_VERTS_B[0]);
            for (int j = 1; j < 12; j++) {
                double d2 = rc_dist2(&RC_VERTS_A[i], &RC_VERTS_B[j]);
                if (d2 < best_d2) { best_d2 = d2; best_j = j; }
            }
            /* Each A vertex should map to a unique B vertex */
            /* (We check the distance is reasonable — less than 2 units) */
            if (best_d2 > 4.0) { ok = 0; break; }
        }
        if (ok) { PASS("geometric twin A→B (nearest B vertex exists)"); pass++; }
        else { FAIL("geometric twin A→B", "no reasonable twin"); }
    }

    /* Test 13: total addresses = 168 */
    {
        uint64_t max_addr = 0;
        for (int i = 0; i < RC_N_VERTICES; i++) {
            for (int n = 0; n < RC_N_NODES; n++) {
                uint64_t addr = (uint64_t)i * RC_N_NODES + (uint64_t)n;
                if (i >= 12) addr |= RC_TWIN_BIT;
                if (addr > max_addr) max_addr = addr;
            }
        }
        if (max_addr < (1ULL << 63)) { PASS("all 168 addresses fit in 64-bit"); pass++; }
        else { FAIL("168 addresses fit in 64-bit", "overflow"); }
    }

    /* Test 14: rc_hash_str deterministic */
    {
        uint64_t a = rc_hash_str("blk.0.attn_q.weight");
        uint64_t b = rc_hash_str("blk.0.attn_q.weight");
        uint64_t c = rc_hash_str("blk.1.attn_q.weight");
        if (a == b && a != c) { PASS("rc_hash_str deterministic"); pass++; }
        else { FAIL("rc_hash_str deterministic", ""); }
    }

    /* Test 15: rc_sphere_from_hash on unit sphere */
    {
        uint64_t h = 0x1234567890ABCDEFULL;
        rc_vec3 p = rc_sphere_from_hash(h);
        double r2 = p.x*p.x + p.y*p.y + p.z*p.z;
        if (fabs(r2 - 1.0) < 1e-10) { PASS("rc_sphere_from_hash on unit sphere"); pass++; }
        else { FAIL("rc_sphere_from_hash on unit sphere", ""); }
    }

    /* Test 16: rc_sphere_from_hash symmetric (same hash → same point) */
    {
        uint64_t h = 0xDEADBEEFCAFEULL;
        rc_vec3 p1 = rc_sphere_from_hash(h);
        rc_vec3 p2 = rc_sphere_from_hash(h);
        if (p1.x == p2.x && p1.y == p2.y && p1.z == p2.z) {
            PASS("rc_sphere_from_hash symmetric"); pass++;
        } else { FAIL("rc_sphere_from_hash symmetric", ""); }
    }

    /* Test 17: rc_capture_name produces valid address range */
    {
        const char *names[] = {
            "blk.0.attn_q.weight",
            "blk.0.attn_k.weight",
            "blk.0.attn_v.weight",
            "token_embd.weight",
            "output.weight",
            "output_norm.weight"
        };
        int ok = 1;
        for (int i = 0; i < 6; i++) {
            uint64_t addr = rc_capture_name(names[i]);
            uint64_t base = addr & ~RC_TWIN_BIT;
            int vi = (int)(base / RC_N_NODES);
            int ni = (int)(base % RC_N_NODES);
            if (vi < 0 || vi >= 24 || ni < 0 || ni >= 7) {
                ok = 0; printf("    %s → addr 0x%llx invalid\n", names[i], (unsigned long long)addr);
                break;
            }
        }
        if (ok) { PASS("rc_capture_name produces valid addresses"); pass++; }
        else { FAIL("rc_capture_name produces valid addresses", ""); }
    }

    /* Test 18: rc_capture_name_twin flips twin bit */
    {
        uint64_t a = rc_capture_name("blk.0.attn_q.weight");
        uint64_t b = rc_capture_name_twin("blk.0.attn_q.weight");
        if ((a ^ b) == RC_TWIN_BIT) { PASS("rc_capture_name_twin flips twin bit"); pass++; }
        else { FAIL("rc_capture_name_twin flips twin bit", ""); }
    }

    /* Test 19: different tensor names → different addresses (collision check) */
    {
        const char *names[] = {
            "blk.0.attn_q.weight", "blk.0.attn_k.weight",
            "blk.0.attn_v.weight", "blk.0.attn_o.weight",
            "blk.0.ffn_gate.weight", "blk.0.ffn_up.weight",
            "blk.0.ffn_down.weight", "blk.1.attn_q.weight"
        };
        int collisions = 0;
        for (int i = 0; i < 8; i++) {
            for (int j = i+1; j < 8; j++) {
                if (rc_capture_name(names[i]) == rc_capture_name(names[j]))
                    collisions++;
            }
        }
        if (collisions == 0) { PASS("no collisions across 8 tensor names"); pass++; }
        else { char buf[32]; snprintf(buf, 32, "%d collisions", collisions);
               FAIL("no collisions across 8 tensor names", buf); }
    }

    printf("\n%d/%d PASS\n", pass, pass + fail);
    return fail;
}
