#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
int main() {
    FILE *f = fopen("test_qwen.pogls","rb");
    if (!f) return 1;
    fseek(f,0,SEEK_END); long sz = ftell(f); rewind(f);
    uint8_t *buf = (uint8_t*)malloc(sz); fread(buf,1,sz,f); fclose(f);
    uint32_t magic = *(uint32_t*)(buf+0);
    uint32_t version = *(uint32_t*)(buf+4);
    uint32_t flags = *(uint32_t*)(buf+8);
    uint32_t n_tensors = *(uint32_t*)(buf+12);
    uint64_t model_meta_off = *(uint64_t*)(buf+104);
    uint32_t model_meta_sz = *(uint32_t*)(buf+112);
    printf("magic=0x%08x ver=%u flags=0x%04x n_tensors=%u\n", magic, version, flags, n_tensors);
    printf("model_meta_off=%llu sz=%u HAS_MMETA=%d\n", (unsigned long long)model_meta_off, model_meta_sz, (flags & 0x2) != 0);
    if (model_meta_sz > 0) {
        uint32_t gguf_magic = *(uint32_t*)(buf + model_meta_off);
        printf("GGUF magic at meta_off: 0x%08x ", gguf_magic);
        if (gguf_magic == 0x46554747) printf("(GGUF!)");
        printf("\n");
    }
    free(buf);
    return 0;
}
