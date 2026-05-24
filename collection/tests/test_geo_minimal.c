#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#define GEO_STORE_READER_IMPL
#include "geo_store_reader.h"

int main(void) {
    printf("GeoStore minimal test starting...\n");
    fflush(stdout);

    GeoStore gs;
    printf("Calling geo_store_open...\n");
    fflush(stdout);

    int rc = geo_store_open(&gs, "build/test_geom");
    printf("geo_store_open returned: %d\n", rc);
    fflush(stdout);

    if (rc != 0) return 1;

    printf("n_entries=%u data_size=%llu\n",
           (unsigned)gs.n_entries, (unsigned long long)gs.data_size);
    fflush(stdout);

    GeoEntry e;
    rc = geo_store_query(&gs, 'K', 0, 'I', &e);
    printf("query K-0-I: %d (zone=%u shape=%c rows=%u cols=%u ns=%u)\n",
           rc, (unsigned)e.zone, (char)e.shape,
           (unsigned)e.n_rows, (unsigned)e.n_cols, (unsigned)e.ns_shift);
    fflush(stdout);

    if (rc == 0) {
        printf("first 3 floats: %f %f %f\n", e.data[0], e.data[1], e.data[2]);
        fflush(stdout);
    }

    geo_store_close(&gs);
    printf("Done\n");
    return 0;
}
