/*
 * test_capture_radial.c — Test capture_radial.h pipeline
 */

#include <stdio.h>
#include <string.h>
#include "capture_radial.h"

#define PASS(name) printf("  [PASS] %s\n", name)
#define FAIL(name, msg) do { printf("  [FAIL] %s: %s\n", name, msg); fail++; } while(0)

int main(void) {
    int fail = 0, pass = 0;

    printf("=== capture_radial tests ===\n\n");

    const char *names[] = {
        "token_embd.weight",
        "output_norm.weight",
        "output.weight",
        "blk.0.attn_q.weight",
        "blk.0.attn_k.weight",
        "blk.0.attn_v.weight",
        "blk.0.attn_o.weight",
        "blk.0.ffn_gate.weight",
        "blk.0.ffn_up.weight",
        "blk.0.ffn_down.weight",
        "blk.1.attn_q.weight",
        "blk.1.attn_k.weight",
    };
    int n_names = 12;

    /* Test 1: crad_init + crad_capture + crad_summary */
    {
        CradResult cr;
        crad_init(&cr);
        for (int i = 0; i < n_names; i++) {
            int r = crad_capture(&cr, names[i]);
            if (r != 0) { FAIL("crad_capture", "non-zero return"); goto done; }
        }
        if (cr.n_captured == (uint32_t)n_names) {
            PASS("crad_capture all 12 tensor names");
            pass++;
        } else {
            FAIL("crad_capture all 12 tensor names", "count mismatch");
            goto done;
        }
    }

    /* Test 2: deterministic — same names → same addresses */
    {
        CradResult cr;
        crad_init(&cr);
        for (int i = 0; i < n_names; i++) {
            crad_capture(&cr, names[i]);
        }
        /* Re-capture and compare */
        for (int i = 0; i < n_names; i++) {
            uint64_t a = rc_capture_name(names[i]);
            if (a != cr.entries[i].addr) {
                FAIL("deterministic", "address mismatch");
                goto done2;
            }
        }
        PASS("deterministic: same names → same addresses");
        pass++;
    }
    done2:

    /* Test 3: twin address is valid */
    {
        CradResult cr;
        crad_init(&cr);
        for (int i = 0; i < n_names; i++) {
            crad_capture(&cr, names[i]);
        }
        int ok = 1;
        for (uint32_t i = 0; i < cr.n_captured; i++) {
            uint64_t t = cr.entries[i].twin;
            if ((t & ~RC_TWIN_BIT) != (cr.entries[i].addr & ~RC_TWIN_BIT)) {
                ok = 0; break;
            }
            if ((t & RC_TWIN_BIT) == (cr.entries[i].addr & RC_TWIN_BIT)) {
                ok = 0; break;
            }
        }
        if (ok) { PASS("twin address: same vertex+node, flipped twin bit"); pass++; }
        else { FAIL("twin address", "invalid"); }
    }

    /* Test 4: crad_find_addr */
    {
        CradResult cr;
        crad_init(&cr);
        for (int i = 0; i < n_names; i++) {
            crad_capture(&cr, names[i]);
        }
        uint64_t a = crad_find_addr(&cr, "blk.0.attn_q.weight");
        if (a != 0) {
            PASS("crad_find_addr found blk.0.attn_q.weight");
            pass++;
        } else {
            FAIL("crad_find_addr", "not found");
        }
        uint64_t missing = crad_find_addr(&cr, "nonexistent.tensor");
        if (missing == 0) {
            PASS("crad_find_addr returns 0 for missing name");
            pass++;
        } else {
            FAIL("crad_find_addr missing", "non-zero");
        }
    }

    /* Test 5: crad_get_all */
    {
        CradResult cr;
        crad_init(&cr);
        for (int i = 0; i < n_names; i++) {
            crad_capture(&cr, names[i]);
        }
        const char *names_out[32];
        uint64_t addrs_out[32], twins_out[32];
        uint32_t n = crad_get_all(&cr, names_out, addrs_out, twins_out, 32);
        if (n == (uint32_t)n_names) {
            int ok = 1;
            for (uint32_t i = 0; i < n; i++) {
                if (strcmp(names_out[i], names[i]) != 0) { ok = 0; break; }
                if (addrs_out[i] != cr.entries[i].addr) { ok = 0; break; }
                if (twins_out[i] != cr.entries[i].twin) { ok = 0; break; }
            }
            if (ok) { PASS("crad_get_all returns all entries correctly"); pass++; }
            else { FAIL("crad_get_all", "data mismatch"); }
        } else {
            FAIL("crad_get_all", "count mismatch");
        }
    }

    /* Test 6: capture by data (pass raw name as data — useful for testing) */
    {
        CradResult cr;
        crad_init(&cr);
        /* Direct name-based capture */
        uint64_t a1 = rc_capture_name("test_name");
        uint64_t a2 = rc_capture_name("test_name");
        if (a1 == a2 && a1 != 0) {
            PASS("rc_capture_name deterministic");
            pass++;
        } else {
            FAIL("rc_capture_name", "non-deterministic or zero");
        }
    }

    /* Test 7: crad_verify */
    {
        CradResult cr;
        crad_init(&cr);
        for (int i = 0; i < n_names; i++) {
            crad_capture(&cr, names[i]);
        }
        crad_verify(&cr, names, n_names);
        if (cr.lossless_ok && cr.n_verified == (uint32_t)n_names) {
            PASS("crad_verify: all names deterministic");
            pass++;
        } else {
            FAIL("crad_verify", "verification failed");
        }
    }

done:
    printf("\n%d/%d PASS\n", pass, pass + fail);
    return fail;
}
