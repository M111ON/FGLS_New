/*
 * test_geometry_store_reader.c
 * ============================
 * Minimal self-test for the C .gsidx/.gsdat reader.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "geometry_store_reader.h"

static int write_fixture_store(const char *base)
{
    char idx_path[256];
    char dat_path[256];
    FILE *idx = NULL;
    FILE *dat = NULL;

    snprintf(idx_path, sizeof(idx_path), "%s.gsidx", base);
    snprintf(dat_path, sizeof(dat_path), "%s.gsdat", base);

    const float rows_a[] = { 1.f, 2.f, 3.f, 4.f };
    const float rows_b[] = { 10.f, 11.f, 12.f, 13.f, 14.f, 15.f };

    dat = fopen(dat_path, "wb");
    if (!dat) return -1;
    if (fwrite(rows_a, sizeof(float), 4, dat) != 4) return -1;
    if (fwrite(rows_b, sizeof(float), 6, dat) != 6) return -1;
    fclose(dat);
    dat = NULL;

    idx = fopen(idx_path, "wb");
    if (!idx) return -1;

    fwrite(GSIDX_MAGIC, 1, 8, idx);
    uint32_t n_entries = 2;
    uint64_t data_size = (uint64_t)(sizeof(rows_a) + sizeof(rows_b));
    unsigned char pad[12] = {0};
    fwrite(&n_entries, sizeof(n_entries), 1, idx);
    fwrite(&data_size, sizeof(data_size), 1, idx);
    fwrite(pad, 1, sizeof(pad), idx);

    uint16_t zone = 1, shape = 0;
    int64_t off = 0;
    uint32_t n_rows = 1, n_cols = 4;
    fwrite(&zone, sizeof(zone), 1, idx);
    fwrite(&shape, sizeof(shape), 1, idx);
    fwrite(&off, sizeof(off), 1, idx);
    fwrite(&n_rows, sizeof(n_rows), 1, idx);
    fwrite(&n_cols, sizeof(n_cols), 1, idx);

    zone = 12 + 3;
    shape = 3;
    off = (int64_t)sizeof(rows_a);
    n_rows = 2;
    n_cols = 3;
    fwrite(&zone, sizeof(zone), 1, idx);
    fwrite(&shape, sizeof(shape), 1, idx);
    fwrite(&off, sizeof(off), 1, idx);
    fwrite(&n_rows, sizeof(n_rows), 1, idx);
    fwrite(&n_cols, sizeof(n_cols), 1, idx);

    fclose(idx);
    return 0;
}

static void expect_floats(const float *ptr, const float *want, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        assert(ptr[i] == want[i]);
    }
}

int main(void)
{
    const char *base = "test_geometry_store_tmp";
    GeometryStoreReader store;
    GeometryStoreView view;
    const float want_a[] = { 1.f, 2.f, 3.f, 4.f };
    const float want_b[] = { 10.f, 11.f, 12.f, 13.f, 14.f, 15.f };

    assert(write_fixture_store(base) == 0);
    assert(geometry_store_open(&store, base) == 0);
    assert(geometry_store_count(&store) == 2);

    assert(geometry_store_query(&store, 1, 'I', NULL, &view) == 0);
    assert(view.n_rows == 1 && view.n_cols == 4);
    expect_floats(view.ptr, want_a, 4);

    assert(geometry_store_query(&store, 3, 'S', "Q", &view) == 0);
    assert(view.n_rows == 2 && view.n_cols == 3);
    expect_floats(view.ptr, want_b, 6);

    assert(geometry_store_query_key(&store, 15, 'S', &view) == 0);
    assert(view.n_rows == 2 && view.n_cols == 3);

    geometry_store_close(&store);
    remove("test_geometry_store_tmp.gsidx");
    remove("test_geometry_store_tmp.gsdat");
    puts("geometry_store_reader: PASS");
    return 0;
}
