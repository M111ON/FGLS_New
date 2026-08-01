#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "runner/explore/fgls_archive.h"

int main(void) {
    FILE *f = fopen("I:/model/Qwen3-0.6B-Q8_0.gguf.fgls", "rb");
    if (!f) return 1;
    
    FGLS_Header hdr;
    fread(&hdr, 1, FGLS_HEADER_SZ, f);
    
    printf("=== First 5 Q8_0 Tensor Entries ===\n");
    int shown = 0;
    for (uint32_t t = 0; t < hdr.n_tensors && shown < 5; t++) {
        FGLS_TensorEntry entry;
        fread(&entry, 1, sizeof(entry), f);
        
        char name[256];
        fread(name, 1, entry.name_len, f);
        name[entry.name_len] = '\0';
        
        if (entry.flags & 1) {
            printf("\n[%u] %s\n", t, name);
            printf("    type=%u, n_blocks=%u, arch_offset=%" PRIu64 ", arch_size=%" PRIu64 "\n",
                   entry.ggml_type, entry.n_blocks, entry.arch_offset, entry.arch_size);
            shown++;
        }
    }
    
    fclose(f);
    return 0;
}
