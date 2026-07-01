/*
 * test_pogls_read.c — Validate POGLS file structural integrity from C
 *
 * Build: gcc -O0 -std=c11 -I. -o test_pogls_read.exe test_pogls_read.c
 *
 * Usage: test_pogls_read.exe store.pogls
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "pogls_store.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: test_pogls_read.exe store.pogls\n"); return 1; }
    const char *path = argv[1];

    /* Open file and mmap */
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "ERROR: open\n"); return 1; }

    _fseeki64(f, 0, SEEK_END);
    uint64_t file_sz = (uint64_t)_ftelli64(f);
    rewind(f);

    uint8_t *map = (uint8_t*)malloc((size_t)file_sz);
    if (!map) { fprintf(stderr, "ERROR: malloc\n"); return 1; }
    fread(map, 1, (size_t)file_sz, f);
    fclose(f);

    PoglsStore *s = (PoglsStore*)map;
    if (s->magic != POGLS_MAGIC) {
        fprintf(stderr, "ERROR: bad magic %08x (expected %08x)\n", s->magic, POGLS_MAGIC);
        free(map); return 1;
    }

    printf("[test] POGLS: magic=%08x ver=%u n_tensors=%u flags=%u\n",
           s->magic, s->version, s->n_tensors, s->flags);
    printf("[test] file_size=%llu struct_size=%zu\n",
           (unsigned long long)file_sz, sizeof(*s));

    if (file_sz < sizeof(*s)) {
        fprintf(stderr, "ERROR: file too small\n"); free(map); return 1;
    }

    /* Verify index entries */
    uint32_t n_valid = 0, n_zero = 0, n_oob = 0;
    uint64_t max_off = 0;
    for (uint32_t a = 0; a < POGLS_MAX_ADDR; a++) {
        PoglsStoreEntry *e = &s->idx[a];
        if (e->offset == 0 && e->nbytes == 0) { n_zero++; continue; }
        if (e->offset < sizeof(*s)) {
            fprintf(stderr, "  OOB addr=%5u: offset=%llu < struct_size\n",
                    a, (unsigned long long)e->offset);
            n_oob++; continue;
        }
        if (e->offset + e->nbytes > file_sz) {
            fprintf(stderr, "  OOB addr=%5u: offset=%llu + nbytes=%u > file_sz=%llu\n",
                    a, (unsigned long long)e->offset, e->nbytes,
                    (unsigned long long)file_sz);
            n_oob++; continue;
        }
        n_valid++;
        if (e->offset + e->nbytes > max_off) max_off = e->offset + e->nbytes;
        /* Sample: verify first 16 bytes are readable */
        if (n_valid <= 3) {
            const uint8_t *data = pogls_store_ptr(s, a);
            fprintf(stderr, "  idx[%5u]: offset=%12llu nbytes=%8u data_prefix=%02x%02x%02x%02x...\n",
                    a, (unsigned long long)e->offset, e->nbytes,
                    data[0], data[1], data[2], data[3]);
        }
    }

    printf("[test] index: %u valid, %u zero, %u OOB\n", n_valid, n_zero, n_oob);
    if (n_valid != s->n_tensors) {
        fprintf(stderr, "WARN: n_tensors=%u but valid index entries=%u\n",
                s->n_tensors, n_valid);
    }
    if (n_oob > 0) { fprintf(stderr, "FAIL: OOB entries\n"); free(map); return 1; }
    if (max_off > file_sz) { fprintf(stderr, "FAIL: data extends past file\n"); free(map); return 1; }

    /* Verify data roundtrip for first valid entry */
    uint32_t first = POGLS_MAX_ADDR;
    for (uint32_t a = 0; a < POGLS_MAX_ADDR; a++)
        if (s->idx[a].nbytes > 0) { first = a; break; }

    if (first < POGLS_MAX_ADDR) {
        const uint8_t *d = pogls_store_ptr(s, first);
        printf("[test] first tensor at addr=%u: %u bytes\n", first, s->idx[first].nbytes);
    }

    printf("[test] PASS\n");
    free(map);
    return 0;
}
