#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "capture_pipeline.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <tensor.bin>\n", argv[0]);
        return 1;
    }

    /* Read raw tensor file */
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *raw = (uint8_t *)malloc(sz);
    fread(raw, 1, sz, f);
    fclose(f);

    printf("Input: %ld bytes\n", sz);

    /* Split into 64B chunks and capture each */
    CaptureResult cr;
    capture_init(&cr);

    uint32_t n_chunks = (uint32_t)(sz / 64);
    uint32_t tick = 0;
    uint8_t layer = 0;

    for (uint32_t i = 0; i < n_chunks; i++) {
        capture_tensor(&cr, raw + i * 64, 64, 0, tick++, layer);
    }

    printf("Chunks captured: %u\n", cr.n_tensors_captured);
    printf("Freeze entries: %u\n", cr.n_freeze_entries);
    printf("Rewind occupied: %u/%u (%.1f%%)\n",
           cr.total_tring_occupied, TW_REWIND_SLOTS,
           100.0 * cr.total_tring_occupied / TW_REWIND_SLOTS);

    /* Show some capture details */
    printf("\nCapture pipeline uses:\n");
    printf("  - triwheel zone/slot/residual (integer-only, POGLS rule)\n");
    printf("  - dodecahedron 12-face capo routing\n");
    printf("  - rewind ring buffer (TW_REWIND_SLOTS=%u)\n", TW_REWIND_SLOTS);
    printf("  - freeze tracking for drain/bundle\n");

    free(raw);
    return 0;
}
