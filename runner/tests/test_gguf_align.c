#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

int main() {
    FILE *f = fopen("I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf", "rb");
    if (!f) { printf("FAIL: open\n"); return 1; }

    uint32_t magic, version;
    uint64_t n_tensors, n_kv;
    fread(&magic, 4, 1, f);
    fread(&version, 4, 1, f);
    fread(&n_tensors, 8, 1, f);
    fread(&n_kv, 8, 1, f);

    printf("magic=0x%X version=%u tensors=%llu kv=%llu\n",
           magic, version, (unsigned long long)n_tensors, (unsigned long long)n_kv);

    /* Look for alignment metadata - search through KV for "general.alignment" */
    /* First find which KV pair contains alignment */
    for (uint64_t k = 0; k < n_kv; k++) {
        long kv_start = ftell(f);
        uint64_t klen;
        fread(&klen, 8, 1, f);
        char key[256] = {0};
        fread(key, (size_t)klen, 1, f);
        key[klen] = '\0';
        uint32_t vtype;
        fread(&vtype, 4, 1, f);

        if (1 || strcmp(key, "general.alignment") == 0) {
            printf("KV[%llu] at %ld: key='%s' vtype=%u\n",
                   (unsigned long long)k, kv_start, key, vtype);
        }

        if (strcmp(key, "general.alignment") == 0 && vtype == 4) {
            uint32_t align_val;
            fread(&align_val, 4, 1, f);
            printf("  ALIGNMENT = %u\n", align_val);
        } else {
            switch (vtype) {
                case 0: case 1: case 7: fseek(f, 1, SEEK_CUR); break;
                case 2: case 3: fseek(f, 2, SEEK_CUR); break;
                case 4: case 5: case 6: fseek(f, 4, SEEK_CUR); break;
                case 10: case 11: case 12: fseek(f, 8, SEEK_CUR); break;
                case 8: {
                    uint64_t slen; fread(&slen, 8, 1, f); fseek(f, (long)slen, SEEK_CUR);
                    break;
                }
                case 9: {
                    uint32_t arr_type; fread(&arr_type, 4, 1, f);
                    uint64_t narr; fread(&narr, 8, 1, f);
                    printf("  [array type=%u count=%llu]\n", arr_type, (unsigned long long)narr);
                    static const uint8_t esz[] = {1,1,2,2,4,4,4,1,0,0,8,8,8};
                    if (arr_type == 8) {
                        for (uint64_t a = 0; a < narr; a++) {
                            uint64_t slen; fread(&slen, 8, 1, f); fseek(f, (long)slen, SEEK_CUR);
                        }
                    } else if (arr_type < 13) {
                        fseek(f, (long)(esz[arr_type] * narr), SEEK_CUR);
                    }
                    break;
                }
            }
        }
    }

    long pos = ftell(f);
    printf("After KV: pos=%ld\n", pos);

    /* Try different alignments and look for tensor info */
    uint32_t align_vals[] = {32, 64, 128, 256, 512, 1024, 4096, 0};
    for (int a = 0; align_vals[a] != 0; a++) {
        uint32_t align = align_vals[a];
        uint32_t pad = (align - (pos % align)) % align;
        long aligned_pos = pos + pad;
        fseek(f, aligned_pos, SEEK_SET);
        uint64_t val;
        fread(&val, 8, 1, f);
        printf("  align=%u: pad=%u pos=%ld val=%llu", align, pad, aligned_pos, (unsigned long long)val);
        if (val == n_tensors) {
            printf(" ← MATCHES tensor_count=%llu!", (unsigned long long)n_tensors);
        }
        printf("\n");
    }

    fclose(f);
    return 0;
}
