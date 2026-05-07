/*
 * test_fgls_twin_store.c — Correctness tests for fgls_twin_store.h
 * compile: gcc -O2 -I.. -o test_fts test_fgls_twin_store.c && ./test_fts
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "fgls_twin_store.h"

#define ASSERT(cond, msg) do { \
    if (!(cond)) { printf("[FAIL] %s\n", msg); fails++; } \
    else         { printf("[PASS] %s\n", msg); } \
} while(0)

int main(void) {
    int fails = 0;

    /* ── T1: trit address determinism ── */
    {
        FtsTritAddr a1 = fts_trit_addr(0xDEAD, 0xBEEF);
        FtsTritAddr a2 = fts_trit_addr(0xDEAD, 0xBEEF);
        ASSERT(a1.trit  == a2.trit  &&
               a1.coset == a2.coset &&
               a1.face  == a2.face  &&
               a1.level == a2.level,
               "T1: trit_addr deterministic");
        ASSERT(a1.trit < 27u,  "T1: trit < 27");
        ASSERT(a1.coset < 12u, "T1: coset < 12");
        ASSERT(a1.face  < 6u,  "T1: face  < 6");
        ASSERT(a1.level < 4u,  "T1: level < 4");
    }

    /* ── T2: write → slot populated ── */
    {
        FtsTwinStore s;
        fts_init(&s, 0xCAFEBABEDEAD0000ULL);

        DodecaEntry e = {
            .merkle_root = 0x1234567890ABCDEFULL,
            .sha256_hi   = 0xAAAABBBBCCCCDDDDULL,
            .sha256_lo   = 0x0000111122223333ULL,
            .offset      = 7,
            .hop_count   = 42,
            .segment     = 3,
            .ref_count   = 1,
        };

        int rc = fts_write(&s, 0x100, 0x200, &e);
        ASSERT(rc == 0, "T2: write returns 0");

        FtsTritAddr ta = fts_trit_addr(0x100, 0x200);
        FrustumSlot64 *slot = &s.ga.cubes[ta.coset].faces[ta.face];

        ASSERT(slot->core[ta.level] == e.merkle_root, "T2: core[level] = merkle_root");
        ASSERT(slot->coset      == ta.coset, "T2: slot.coset correct");
        ASSERT(slot->frustum_id == ta.face,  "T2: slot.frustum_id correct");
        ASSERT(slot->checksum   != 0u,       "T2: checksum computed");
    }

    /* ── T3: delete → coset silenced ── */
    {
        FtsTwinStore s;
        fts_init(&s, 0xDEAD000000000000ULL);

        DodecaEntry e = { .merkle_root=0x999, .ref_count=1 };
        uint64_t addr = 0xABC, val = 0x123;

        /* pre-delete write ok */
        ASSERT(fts_write(&s, addr, val, &e) == 0, "T3: write before delete ok");

        /* delete */
        fts_delete(&s, addr, val);

        /* post-delete write rejected */
        ASSERT(fts_write(&s, addr, val, &e) == -1, "T3: write after delete = -1");

        /* accessibility check */
        ASSERT(fts_accessible(&s, addr, val) == -1, "T3: fts_accessible = -1 after delete");

        /* different addr same coset also blocked */
        FtsTritAddr ta = fts_trit_addr(addr, val);
        /* find another addr that maps to same coset */
        int found_same_coset = 0;
        for (uint64_t a2 = 0; a2 < 1000u; a2++) {
            FtsTritAddr t2 = fts_trit_addr(a2, 0);
            if (t2.coset == ta.coset && a2 != addr) {
                ASSERT(fts_write(&s, a2, 0, &e) == -1,
                       "T3: same-coset addr also blocked");
                found_same_coset = 1;
                break;
            }
        }
        if (!found_same_coset) printf("[SKIP] T3: same-coset search\n");
    }

    /* ── T4: null entry → -2 ── */
    {
        FtsTwinStore s;
        fts_init(&s, 0x1ULL);
        ASSERT(fts_write(&s, 1, 2, NULL) == -2, "T4: null entry = -2");
    }

    /* ── T5: serialize → 4896B correct magic ── */
    {
        FtsTwinStore s;
        fts_init(&s, 0xFEEDFACECAFEBABEULL);

        DodecaEntry e = { .merkle_root=0xBEEF, .sha256_hi=0xDEAD,
                          .offset=1, .hop_count=5, .ref_count=1 };
        fts_write(&s, 0x42, 0x43, &e);
        fts_serialize(&s);

        uint8_t buf[GCFS_TOTAL_BYTES];
        fts_write_buf(&s, buf);

        /* check magic bytes "FGLS" */
        ASSERT(buf[0] == 0x53 && buf[1] == 0x4C &&
               buf[2] == 0x47 && buf[3] == 0x46,
               "T5: GCFS_MAGIC = 'FGLS' in buffer");
        ASSERT(sizeof(buf) == 4896u, "T5: buffer size = 4896B");

        FtsTwinStats st = fts_stats(&s);
        ASSERT(st.writes == 1u, "T5: write_count = 1");
        ASSERT(st.active_cosets == 12u, "T5: all 12 cosets active");
    }

    /* ── T6: delete_coset → serialize zero-fills that coset ── */
    {
        FtsTwinStore s;
        fts_init(&s, 0x5A5A5A5A5A5A5A5AULL);

        DodecaEntry e = { .merkle_root=0x1111111111111111ULL, .ref_count=1 };

        /* write to coset 0 face 0 */
        /* find addr that maps to coset 0 */
        uint64_t target_addr = 0, target_val = 0;
        for (uint64_t a = 0; a < 1000u; a++) {
            FtsTritAddr t = fts_trit_addr(a, 0);
            if (t.coset == 0) { target_addr = a; break; }
        }
        fts_write(&s, target_addr, target_val, &e);
        fts_serialize(&s);

        uint8_t buf_before[GCFS_TOTAL_BYTES];
        fts_write_buf(&s, buf_before);

        /* delete coset 0 */
        fts_delete_coset(&s, 0);
        fts_serialize(&s);

        uint8_t buf_after[GCFS_TOTAL_BYTES];
        fts_write_buf(&s, buf_after);

        /* payload for coset 0 = first 384B of payload section */
        uint32_t payload_off = GCFS_HEADER_BYTES + GCFS_META_BYTES;
        int coset0_zeroed = 1;
        for (uint32_t i = 0; i < 384u; i++) {
            if (buf_after[payload_off + i] != 0) { coset0_zeroed = 0; break; }
        }
        ASSERT(coset0_zeroed, "T6: deleted coset payload = zero-filled");

        FtsTwinStats st = fts_stats(&s);
        ASSERT(st.active_cosets == 11u, "T6: 11 active cosets after 1 delete");
    }

    printf("\n%s — %d failure(s)\n",
           fails == 0 ? "ALL PASS" : "SOME FAILED", fails);
    return fails ? 1 : 0;
}
