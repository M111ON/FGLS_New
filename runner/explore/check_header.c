#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "runner/explore/fgls_archive.h"

int main(void) {
    FILE *f = fopen("I:/model/Qwen3-0.6B-Q8_0.gguf.fgls", "rb");
    if (!f) return 1;
    
    FGLS_Header hdr;
    fread(&hdr, 1, FGLS_HEADER_SZ, f);
    
    printf("=== FGLS Archive Header ===\n");
    printf("  magic:          0x%08X (%s)\n", hdr.magic, hdr.magic == FGLS_MAGIC ? "OK" : "BAD");
    printf("  version:        %u\n", hdr.version);
    printf("  n_tensors:      %u\n", hdr.n_tensors);
    printf("  n_baked:        %u\n", hdr.n_baked);
    printf("  orig_size:      %" PRIu64 " (%.1f MB)\n", hdr.orig_size, hdr.orig_size/1048576.0);
    printf("  orig_data_start:%" PRIu64 "\n", hdr.orig_data_start);
    printf("  kept_weights:   %" PRIu64 "\n", hdr.kept_weights);
    printf("  total_weights:  %" PRIu64 "\n", hdr.total_weights);
    
    fclose(f);
    return 0;
}
