#include "pipeline_glue.h"
#include <stdio.h>
#include <string.h>
int main(void) {
    const char *data = "Hello GeoField pipeline integration test! This is 64 bytes of data for chunking and processing through the full pipeline stages.";
    PGContext pg;
    pg_init(&pg, data, (uint32_t)strlen(data), 42, PG_FLAG_VERBOSE);
    pg_run_full(&pg);
    PGStats s = pg_stats(&pg);
    printf("Input: %u bytes, Output: %u bytes, Ratio: %.2fx\n", s.input_size, s.output_size, (double)s.input_size / (double)s.output_size);
    printf("Chunks: %u, Bonded: %u, Shelled: %u, Pixelated: %u, Hamburger: %u\n",
           s.n_chunks, s.n_bonded, s.n_shelled, s.n_pixelated, s.n_hamburger);
    printf("File: %s\n", pg.output_path);
    pg_free(&pg);
    return 0;
}
