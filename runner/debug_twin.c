#include <stdio.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    FILE *f = fopen("I:/FGLS_new/test_model.dramtile", "rb");
    if (!f) { perror("open"); return 1; }

    /* Read last 4 bytes = dir_off */
    fseek(f, -4, SEEK_END);
    uint32_t dir_off = 0;
    fread(&dir_off, 4, 1, f);
    printf("dir_off = %u (0x%x)\n", dir_off, dir_off);

    /* Read file size */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    printf("file size = %ld bytes\n", fsize);

    if (dir_off >= (uint32_t)fsize - 16) {
        printf("dir_off >= fsize-16, invalid\n");
        fclose(f);
        return 1;
    }

    /* Read TDI2 header at dir_off */
    fseek(f, dir_off, SEEK_SET);
    char magic[5] = {0};
    fread(magic, 4, 1, f);
    printf("magic = '%s'\n", magic);

    uint32_t n = 0;
    fread(&n, 4, 1, f);
    printf("n = %u\n", n);

    uint64_t used = 0;
    fread(&used, 8, 1, f);
    printf("used = %llu\n", (unsigned long long)used);

    /* Read first entry */
    if (n > 0) {
        uint32_t addr; uint64_t off, sz;
        uint32_t dtype; int32_t ndim;
        uint32_t shape[4] = {0};
        uint32_t namelen;
        fread(&addr, 4, 1, f);
        fread(&off, 8, 1, f);
        fread(&sz, 8, 1, f);
        fread(&dtype, 4, 1, f);
        fread(&ndim, 4, 1, f);
        fread(shape, 4, 4, f);
        fread(&namelen, 4, 1, f);
        printf("  [0] addr=0x%08x off=%llu sz=%llu dtype=%u ndim=%d namelen=%u\n",
               addr, (unsigned long long)off, (unsigned long long)sz, dtype, ndim, namelen);
        if (namelen > 0 && namelen < 256) {
            char name[256] = {0};
            fread(name, 1, namelen, f);
            printf("  name='%s'\n", name);
        }
    }

    fclose(f);
    return 0;
}
