#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "runner/explore/fgls_archive.h"

int main(void) {
    FILE *f = fopen("I:/model/Qwen3-0.6B-Q8_0.gguf.fgls", "rb");
    if (!f) return 1;
    
    FGLS_Header hdr;
    fread(&hdr, 1, FGLS_HEADER_SZ, f);
    
    /* Skip tensor table to find body start */
    uint64_t body_start = FGLS_HEADER_SZ;
    for (uint32_t t = 0; t < hdr.n_tensors; t++) {
        FGLS_TensorEntry entry;
        fread(&entry, 1, sizeof(entry), f);
        fseek(f, entry.name_len, SEEK_CUR);
        body_start += sizeof(FGLS_TensorEntry) + entry.name_len;
    }
    
    printf("Body starts at: %" PRIu64 "\n", body_start);
    
    /* Read first tensor's data (token_embd.weight) */
    fseek(f, body_start + 4096, SEEK_SET);  /* arch_offset=4096 */
    
    /* Read scales (first 2 bytes of first block) */
    uint8_t scale[2];
    fread(scale, 1, 2, f);
    printf("First block scale: %02X %02X\n", scale[0], scale[1]);
    
    /* Read bitmap (next 4 bytes) */
    uint32_t bitmap;
    fread(&bitmap, 4, 1, f);
    printf("First block bitmap: 0x%08X\n", bitmap);
    
    /* Count set bits */
    int bits = 0;
    for (int i = 0; i < 32; i++)
        if (bitmap & (1u << i)) bits++;
    printf("Weights in first block: %d/32\n", bits);
    
    fclose(f);
    return 0;
}
