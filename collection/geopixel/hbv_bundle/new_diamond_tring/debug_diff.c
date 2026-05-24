#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK_SZ 64

int main(int argc, char **argv) {
    if (argc < 3) return 1;
    FILE *fa = fopen(argv[1], "rb"); if (!fa) return 1;
    FILE *fb = fopen(argv[2], "rb"); if (!fb) return 1;
    fseek(fa, 0, SEEK_END); size_t sa = ftell(fa); rewind(fa);
    fseek(fb, 0, SEEK_END); size_t sb = ftell(fb); rewind(fb);
    size_t n = sa < sb ? sa : sb;
    uint8_t *a = malloc(n); fread(a, 1, n, fa); fclose(fa);
    uint8_t *b = malloc(n); fread(b, 1, n, fb); fclose(fb);
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            size_t ci = i / CHUNK_SZ;
            size_t off = i % CHUNK_SZ;
            printf("FIRST DIFF at offset=%zu (chunk=%zu, byte=%zu): orig=0x%02X dec=0x%02X\n", i, ci, off, a[i], b[i]);
            /* Dump surrounding bytes */
            size_t start = ci * CHUNK_SZ;
            size_t end = start + CHUNK_SZ; if (end > n) end = n;
            printf("  original chunk %zu:", ci);
            for (size_t j = start; j < end; j++) printf(" %02X", a[j]);
            printf("\n  decoded  chunk %zu:", ci);
            for (size_t j = start; j < end; j++) printf(" %02X", b[j]);
            printf("\n");
            free(a); free(b);
            return (int)(i+1);
        }
    }
    printf("MATCH: %zu bytes identical\n", n);
    free(a); free(b);
    return 0;
}
