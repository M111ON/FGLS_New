#include <stdio.h>
#include <stdint.h>
#include <string.h>

int main() {
    FILE *f = fopen("I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf", "rb");
    if (!f) { fprintf(stderr, "FAIL: open\n"); return 1; }

    /* skip header */
    fseek(f, 24, SEEK_SET);
    uint64_t n_kv;
    fread(&n_kv, 8, 1, f);
    printf("n_kv=%llu\n", (unsigned long long)n_kv);
    fseek(f, 24 + 8, SEEK_SET);

    /* skip KV pairs */
    for (uint64_t k = 0; k < n_kv; k++) {
        uint64_t klen;
        fread(&klen, 8, 1, f);
        fseek(f, (long)klen, SEEK_CUR);

        uint32_t vtype;
        fread(&vtype, 4, 1, f);

        switch (vtype) {
            case 0: case 1: case 7: fseek(f, 1, SEEK_CUR); break;
            case 2: case 3: fseek(f, 2, SEEK_CUR); break;
            case 4: case 5: case 6: fseek(f, 4, SEEK_CUR); break;
            case 10: case 11: case 12: fseek(f, 8, SEEK_CUR); break;
            case 8: {
                uint64_t slen;
                fread(&slen, 8, 1, f);
                fseek(f, (long)slen, SEEK_CUR);
                break;
            }
            case 9: {
                uint32_t arr_type;
                uint64_t narr;
                fread(&arr_type, 4, 1, f);
                fread(&narr, 8, 1, f);
                static const uint8_t elem_sz[] = {1,1,2,2,4,4,4,1,0,0,8,8,8};
                size_t sz = (arr_type < 13) ? elem_sz[arr_type] : 0;
                if (arr_type == 8) {
                    for (uint64_t a = 0; a < narr; a++) {
                        uint64_t slen;
                        fread(&slen, 8, 1, f);
                        fseek(f, (long)slen, SEEK_CUR);
                    }
                } else if (sz > 0) {
                    fseek(f, (long)(sz * narr), SEEK_CUR);
                }
                break;
            }
            default:
                printf("KV[%llu]: unknown type %u at pos=%ld\n", (unsigned long long)k, vtype, ftell(f)-4);
                fclose(f); return 1;
        }
        if (k < 3 || k == n_kv-1) {
            printf("KV[%llu] done, pos=%ld\n", (unsigned long long)k, ftell(f));
        }
    }

    long pos = ftell(f);
    printf("after KV: pos=%ld\n", pos);
    uint32_t pad = (32 - (pos % 32)) % 32;
    if (pad) { fseek(f, pad, SEEK_CUR); printf("padded %u bytes to %ld\n", pad, ftell(f)); }

    printf("tensor info at pos=%ld\n", ftell(f));

    /* read 3 tensor infos */
    for (int i = 0; i < 3; i++) {
        printf("TI[%d] at pos=%ld\n", i, ftell(f));
        uint64_t nlen;
        if (fread(&nlen, 8, 1, f) != 1) {
            printf("  read nlen fail! ferror=%d feof=%d\n", ferror(f), feof(f));
            break;
        }
        printf("  nlen=%llu\n", (unsigned long long)nlen);
        char name[256];
        fread(name, (size_t)nlen, 1, f);
        name[nlen] = '\0';
        uint32_t n_dims;
        fread(&n_dims, 4, 1, f);
        printf("  name='%s' n_dims=%u dims=[", name, n_dims);
        for (uint32_t d = 0; d < n_dims; d++) {
            uint32_t dim;
            fread(&dim, 4, 1, f);
            printf("%u%s", dim, d+1 < n_dims ? "," : "");
        }
        uint32_t dtype;
        fread(&dtype, 4, 1, f);
        uint64_t data_off;
        fread(&data_off, 8, 1, f);
        printf("] dtype=%u data_off=%llu\n", dtype, (unsigned long long)data_off);
    }

    fclose(f);
    printf("OK\n");
    return 0;
}
