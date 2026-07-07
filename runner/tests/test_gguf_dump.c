#include <stdio.h>
#include <stdint.h>
int main() {
    FILE *f = fopen("I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf", "rb");
    long pos = 5931189;
    long align_pos = 5931200;
    fseek(f, align_pos, SEEK_SET);
    printf("=== raw dump at %ld ===\n", align_pos);
    for (int i = 0; i < 128; i++) {
        int b = fgetc(f);
        if (b == EOF) { printf("EOF at %d\n", i); break; }
        printf("%02x ", b);
        if ((i % 16) == 15) printf("\n");
    }
    printf("\n");

    /* Now try to figure out: is there tensor_info_count before TIs? */
    uint64_t ti_count;
    fseek(f, align_pos, SEEK_SET);
    fread(&ti_count, 8, 1, f);
    printf("tensor_info_count (raw u64): %llu (0x%llx)\n",
           (unsigned long long)ti_count, (unsigned long long)ti_count);

    /* If ti_count is 291, try reading first TI */
    if (ti_count == 291) {
        uint64_t nlen;
        fread(&nlen, 8, 1, f);
        printf("TI[0] nlen: %llu\n", (unsigned long long)nlen);
    }

    /* Maybe the count is NOT here. Try reading nlen directly at align_pos */
    uint64_t nlen_direct;
    fseek(f, align_pos, SEEK_SET);
    fread(&nlen_direct, 8, 1, f);
    char name[256] = {0};
    if (nlen_direct > 0 && nlen_direct < 256) {
        fread(name, (size_t)nlen_direct, 1, f);
        printf("Direct read at %ld: nlen=%llu name='%s'\n", align_pos,
               (unsigned long long)nlen_direct, name);
    } else {
        printf("Direct nlen = %llu, not a valid name length. Trying variation...\n",
               (unsigned long long)nlen_direct);
        /* Maybe KV parsing missed a field */
        /* Try reading from pos=5931189 (before alignment) */
        for (long test_pos = 5931180; test_pos < 5931220; test_pos++) {
            uint64_t val;
            fseek(f, test_pos, SEEK_SET);
            fread(&val, 8, 1, f);
            if (val == 291) {
                printf("!! Found tensor_info_count=291 at offset %ld (read as u64)\n", test_pos);
            }
        }

        /* Try backward: maybe tensor info starts later */
        for (long test_pos = 5931200; test_pos < 5931400; test_pos += 1) {
            uint64_t val;
            fseek(f, test_pos, SEEK_SET);
            fread(&val, 8, 1, f);
            if (val > 0 && val < 256) {
                fseek(f, test_pos + 8, SEEK_SET);
                char buf[256];
                fread(buf, (size_t)val, 1, f);
                buf[val] = '\0';
                /* check if it looks like a tensor name (starts with "blk" or "token" etc) */
                if (buf[0] == 'b' || buf[0] == 't' || buf[0] == 'o') {
                    printf("!! Tensor name '%s' at offset %ld (nlen=%llu)\n", buf, test_pos, (unsigned long long)val);
                }
            }
        }
    }

    fclose(f);
    return 0;
}
