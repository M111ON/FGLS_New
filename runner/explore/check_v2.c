#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "runner/explore/fgls_archive.h"
int main(void) {
    FILE *f = fopen("I:/model/Qwen3-0.6B-Q8_0.v2.fgls", "rb");
    if (!f) return 1;
    FGLS_Header hdr;
    fread(&hdr, 1, FGLS_HEADER_SZ, f);
    printf("magic: 0x%08X (%s)\n", hdr.magic, hdr.magic==FGLS_MAGIC?"OK":"BAD");
    printf("version: %u\n", hdr.version);
    printf("n_tensors: %u, n_baked: %u\n", hdr.n_tensors, hdr.n_baked);
    printf("orig_size: %" PRIu64 " (%.1f MB)\n", hdr.orig_size, hdr.orig_size/1048576.0);
    printf("arch_size: %" PRIu64 " (%.1f MB)\n", hdr.arch_size, hdr.arch_size/1048576.0);
    printf("kept: %" PRIu64 " / %" PRIu64 " (%.1f%%)\n", hdr.kept_weights, hdr.total_weights,
           hdr.total_weights ? 100.0*hdr.kept_weights/hdr.total_weights : 0);
    fclose(f);
    return 0;
}
