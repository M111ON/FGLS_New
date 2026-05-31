#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define GEO_STORE_READER_IMPL
#include "geo_store_reader.h"

int n_pass = 0, n_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { n_pass++; printf("  OK  %s\n", label); } \
    else { n_fail++; printf("  FAIL %s (%s:%d)\n", label, __FILE__, __LINE__); } \
} while(0)

typedef struct { uint32_t count; } ListCtx;
static void list_cb(uint32_t i, int ns, uint32_t zone, int shape,
                     uint32_t rows, uint32_t cols, void *udata) {
    (void)i; (void)ns; (void)zone; (void)shape; (void)rows; (void)cols;
    ((ListCtx*)udata)->count++;
}

int main(void) {
    printf("=== GeoStore C Reader Test ===\n\n");

    /* T1: open store */
    GeoStore gs;
    int rc = geo_store_open(&gs, "build/test_geom");
    CHECK("T1 open store", rc == GEO_OK);
    if (rc != GEO_OK) { printf("  open failed: %d\n", rc); return 1; }
    CHECK("T1b is_open", gs.is_open == 1);
    CHECK("T1c n_entries", gs.n_entries == 84);
    CHECK("T1d data_size", gs.data_size > 0);
    CHECK("T1e data_buf != NULL", gs.data_buf != NULL);

    /* T2: list all entries */
    ListCtx ctx = {0};
    geo_store_list(&gs, list_cb, &ctx);
    CHECK("T2 list count", ctx.count == 84);

    /* T3: query ns=K, zone=0, shape=I */
    GeoEntry e;
    rc = geo_store_query(&gs, 'K', 0, 'I', &e);
    CHECK("T3 query K-0-I", rc == GEO_OK);
    if (rc == GEO_OK) {
        CHECK("T3b zone", e.zone == 0);
        CHECK("T3c shape", e.shape == 'I');
        CHECK("T3d n_rows", e.n_rows == 8);
        CHECK("T3e n_cols", e.n_cols == 64);
        CHECK("T3f data != NULL", e.data != NULL);
        CHECK("T3g ns_shift", e.ns_shift == GEO_NS_K);
    }

    /* T4: query ns=Q, zone=3, shape=O */
    rc = geo_store_query(&gs, 'Q', 3, 'O', &e);
    CHECK("T4 query Q-3-O", rc == GEO_OK);
    if (rc == GEO_OK) {
        CHECK("T4b zone", e.zone == 3);
        CHECK("T4c ns_shift", e.ns_shift == GEO_NS_Q);
    }

    /* T5: query ns=D, zone=3, shape=S */
    rc = geo_store_query(&gs, 'D', 3, 'S', &e);
    CHECK("T5 query D-3-S", rc == GEO_OK);
    if (rc == GEO_OK) {
        CHECK("T5b zone", e.zone == 3);
        CHECK("T5c ns_shift", e.ns_shift == GEO_NS_D);
    }

    /* T6: non-existent zone=99 */
    rc = geo_store_query(&gs, 'K', 99, 'I', &e);
    CHECK("T6 non-existent zone", rc == GEO_ERR_NOT_FOUND);

    /* T7: non-existent shape=X */
    rc = geo_store_query(&gs, 'K', 0, 'X', &e);
    CHECK("T7 non-existent shape", rc == GEO_ERR_NOT_FOUND);

    /* T8: has() */
    CHECK("T8 has K-0-I", geo_store_has(&gs, 'K', 0, 'I') == 1);
    CHECK("T8b has not K-99-I", geo_store_has(&gs, 'K', 99, 'I') == 0);

    /* T9: data integrity - ns=D, zone=0, shape=I */
    rc = geo_store_query(&gs, 'D', 0, 'I', &e);
    CHECK("T9 query D-0-I", rc == GEO_OK);
    if (rc == GEO_OK) {
        uint32_t total = e.n_rows * e.n_cols;
        CHECK("T9b rows*cols = 512", total == 512);
        CHECK("T9c first val in range [80,100]",
              e.data[0] > 80.0f && e.data[0] < 100.0f);
        CHECK("T9d 64th val in range [80,100]",
              e.data[64] > 80.0f && e.data[64] < 100.0f);
        CHECK("T9e last val in range [70,110]",
              e.data[total-1] > 70.0f && e.data[total-1] < 110.0f);
    }

    /* T10: all 7 namespaces present, each with 12 keys */
    const char *nss = "QKVOGUD";
    for (int ni = 0; ni < 7; ni++) {
        int found = 0;
        for (uint32_t z = 0; z < 4; z++) {
            if (geo_store_has(&gs, nss[ni], z, 'I')) found++;
            if (geo_store_has(&gs, nss[ni], z, 'O')) found++;
            if (geo_store_has(&gs, nss[ni], z, 'S')) found++;
        }
        CHECK("T10 ns present", found == 12);
    }

    /* T11: no base (non-namespaced) entries */
    CHECK("T11 no base zone 0 I", geo_store_has(&gs, 0, 0, 'I') == 0);

    /* T12: open_mem from buffer */
    {
        FILE *f = fopen("build/test_geom.gsidx", "rb");
        CHECK("T12a open gsidx", f != NULL);
        if (f) {
            fseek(f, 0, SEEK_END);
            long idx_len = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint8_t *idx_buf = (uint8_t*)malloc((size_t)idx_len);
            fread(idx_buf, 1, (size_t)idx_len, f);
            fclose(f);

            GeoStore gs2;
            rc = geo_store_open_mem(&gs2, idx_buf, (size_t)idx_len,
                                     gs.data_buf, gs.data_size);
            CHECK("T12c open_mem", rc == GEO_OK);
            if (rc == GEO_OK) {
                CHECK("T12d mem n_entries", gs2.n_entries == 84);
                GeoEntry e2;
                rc = geo_store_query(&gs2, 'V', 2, 'O', &e2);
                CHECK("T12e mem query V-2-O", rc == GEO_OK);
                if (rc == GEO_OK) {
                    CHECK("T12f mem zone", e2.zone == 2);
                    CHECK("T12g mem ns_shift", e2.ns_shift == GEO_NS_V);
                }
                geo_store_close(&gs2);
            }
            free(idx_buf);
        }
    }

    /* T13: data consistency: file vs mem */
    {
        GeoEntry ef;
        rc = geo_store_query(&gs, 'U', 1, 'S', &ef);
        CHECK("T13a file query U-1-S", rc == GEO_OK);

        FILE *f = fopen("build/test_geom.gsidx", "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long idx_len = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint8_t *idx_buf = (uint8_t*)malloc((size_t)idx_len);
            fread(idx_buf, 1, (size_t)idx_len, f);
            fclose(f);

            GeoStore gs2;
            geo_store_open_mem(&gs2, idx_buf, (size_t)idx_len,
                               gs.data_buf, gs.data_size);
            GeoEntry em;
            rc = geo_store_query(&gs2, 'U', 1, 'S', &em);
            CHECK("T13b mem query", rc == GEO_OK);

            if (ef.data && em.data) {
                uint32_t nf = ef.n_rows * ef.n_cols;
                uint32_t nm = em.n_rows * em.n_cols;
                if (nf == nm) {
                    int match = 1;
                    for (uint32_t i = 0; i < nf; i++)
                        if (ef.data[i] != em.data[i]) { match = 0; break; }
                    CHECK("T13c data match", match);
                }
            }
            geo_store_close(&gs2);
            free(idx_buf);
        }
    }

    /* T14: close + reopen */
    geo_store_close(&gs);
    CHECK("T14a closed", gs.is_open == 0);
    rc = geo_store_open(&gs, "build/test_geom");
    CHECK("T14b reopen", rc == GEO_OK);
    rc = geo_store_query(&gs, 'G', 2, 'I', &e);
    CHECK("T14c query G-2-I", rc == GEO_OK);
    if (rc == GEO_OK)
        CHECK("T14d ns_shift G", e.ns_shift == GEO_NS_G);

    geo_store_close(&gs);

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
