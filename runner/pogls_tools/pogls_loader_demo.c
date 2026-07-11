/*
 * pogls_loader_demo.c — Integration example for pogls_loader.h
 *
 * Shows how to use the POGLS loader from any C/C++ runner.
 * Demonstrates: open, iterate tensors, lookup by name, face rotation.
 *
 * Build:
 *   gcc -O2 -std=c11 -o pogls_loader_demo.exe pogls_loader_demo.c -lm
 */

#ifndef POGLS_LOADER_IMPLEMENTATION
#define POGLS_LOADER_IMPLEMENTATION
#endif
#include "pogls_core/pogls_loader.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <model.pogls>\n", argv[0]);
        return 1;
    }

    printf("═══ POGLS Loader Demo ═══\n\n");

    /* Open POGLS file */
    PoglsLoader *L = pogls_open(argv[1]);
    if (!L) {
        fprintf(stderr, "Error: %s\n", pogls_last_error());
        return 1;
    }

    printf("File:     %s\n", argv[1]);
    printf("Version:  %u\n", pogls_version(L));
    printf("Tensors:  %u\n", pogls_tensor_count(L));
    printf("Size:     %llu bytes (%.2f MB)\n",
           (unsigned long long)pogls_file_size(L),
           pogls_file_size(L) / 1048576.0);

    const char *gguf = pogls_gguf_path(L);
    if (gguf && gguf[0]) printf("GGUF:     %s\n", gguf);

    /* List all tensors */
    printf("\n── Tensor List ──\n");
    uint32_t n = pogls_tensor_count(L);
    uint32_t total_bytes = 0;
    for (uint32_t i = 0; i < n && i < 20; i++) {
        const char *name = pogls_tensor_name(L, i);
        uint32_t addr = pogls_tensor_addr(L, i);
        uint32_t ndim = pogls_tensor_ndim(L, i);
        size_t sz = pogls_tensor_nbytes(L, i);
        uint32_t dims[4];
        pogls_tensor_shape(L, i, dims);

        printf("  [%u] %-30s  addr=%5u  %uD  %zu bytes  dims=[",
               i, name ? name : "?", addr, ndim, sz);
        for (uint32_t d = 0; d < ndim && d < 4; d++) {
            if (d > 0) printf("x");
            printf("%u", dims[d]);
        }
        printf("]\n");
        total_bytes += (uint32_t)sz;
    }
    if (n > 20) printf("  ... (%u more tensors)\n", n - 20);
    printf("  Total: %u bytes (%.2f KB)\n", total_bytes, total_bytes / 1024.0);

    /* Lookup by name */
    if (n > 0) {
        const char *test_name = pogls_tensor_name(L, 0);
        printf("\n── Lookup by Name ──\n");
        printf("  Find:    \"%s\"\n", test_name);
        int idx = pogls_find_tensor(L, test_name);
        printf("  Index:   %d\n", idx);
        void *data = pogls_tensor_data(L, test_name);
        printf("  Data:    %s\n", data ? "OK" : "NULL");
        printf("  Size:    %zu bytes\n", pogls_tensor_size(L, test_name));
    }

    /* Face rotation */
    if (n > 0) {
        printf("\n── Face Rotation ──\n");
        uint32_t base_addr = pogls_tensor_addr(L, 0);
        printf("  Base address: %u\n", base_addr);
        for (int face = 0; face < 6; face++) {
            uint32_t faddr = pogls_face_addr(base_addr, face);
            printf("  Face %d:       %u\n", face, faddr);
        }
    }

    /* Direct mmap access (advanced) */
    printf("\n── Direct MMAP ──\n");
    printf("  ptr:  %p\n", pogls_mmap_ptr(L));
    printf("  size: %zu bytes\n", pogls_mmap_size(L));

    pogls_close(L);
    printf("\nDone.\n");
    return 0;
}
