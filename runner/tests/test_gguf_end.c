#include <stdio.h>
#include <stdint.h>
int main() {
    FILE *f = fopen("I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf", "rb");
    if (!f) return 1;

    /* Read header */
    uint32_t magic; fread(&magic, 4, 1, f);
    uint32_t version; fread(&version, 4, 1, f);
    uint64_t n_tensors; fread(&n_tensors, 8, 1, f);
    uint64_t n_kv; fread(&n_kv, 8, 1, f);
    printf("header: n_tensors=%llu\n", (unsigned long long)n_tensors);

    /* Dump ALL remaining bytes after header to find tensor_info_count */
    /* Check pos before KV */
    long pos = 24; /* header size */
    printf("header end at %ld\n", pos);

    /* Quick check: what's at the end of KV section? */
    /* Actually let's just read the end of KV directly */
    /* From the earlier test: KV ends at 5931189 */ 
    long kv_end = 5931189;
    fseek(f, kv_end, SEEK_SET);

    printf("\n=== At pos %ld (end of KV) ===\n", kv_end);
    for (int i = 0; i < 64; i++) {
        int b = fgetc(f);
        if (b == EOF) break;
        printf("%02x ", b);
        if ((i % 16) == 15) printf("\n");
    }
    printf("\n");

    /* Read as uint64_t for possible tensor_info_count */
    fseek(f, kv_end, SEEK_SET);
    uint64_t val;
    fread(&val, 8, 1, f);
    printf("as u64 at %ld: %llu (n_tensors=%llu, matches=%d)\n", kv_end,
           (unsigned long long)val, (unsigned long long)n_tensors, val == n_tensors);

    fclose(f);
    return 0;
}
