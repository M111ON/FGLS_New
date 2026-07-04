/*
 * gguf_to_pogls_v3.c — Convert GGUF → POGLS v3 (header-only geopixel store)
 *
 * Build:
 *   gcc -O2 -std=c11 -I. -o gguf_to_pogls_v3.exe gguf_to_pogls_v3.c -lm
 *
 * Usage:
 *   .\gguf_to_pogls_v3.exe model.gguf output.pogls [--relative] [--verify]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "pogls_v3_geopixel.h"
#include "gguf_index.h"
#include "addr_space.h"

int main(int argc, char **argv) {
    const char *in_path = NULL, *out_path = NULL;
    int use_relative = 0, do_verify = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--relative") == 0) use_relative = 1;
        else if (strcmp(argv[i], "--verify") == 0) do_verify = 1;
        else if (!in_path) in_path = argv[i];
        else if (!out_path) out_path = argv[i];
    }
    if (!in_path || !out_path) {
        fprintf(stderr, "Usage: gguf_to_pogls_v3 input.gguf output.pogls [--relative] [--verify]\n");
        return 1;
    }

    GGUFTensorIndex idx;
    memset(&idx, 0, sizeof(idx));
    if (gguf_idx_open(in_path, &idx) != 0) {
        fprintf(stderr, "ERROR: can't open %s\n", in_path); return 1;
    }
    fprintf(stderr, "[v3] GGUF has %llu tensors\n", (unsigned long long)idx.n_tensors);

    PoglsV3Entry *entries = (PoglsV3Entry *)calloc(idx.n_tensors, sizeof(PoglsV3Entry));
    uint64_t total_tensor_bytes = 0;

    for (uint64_t i = 0; i < idx.n_tensors; i++) {
        PoglsV3Entry *e = &entries[i];
        e->addr = addr_from_tensor_name(idx.names[i], 0);
        e->dtype = idx.dtypes[i];
        e->nbytes = (uint32_t)idx.sizes[i];
        e->gguf_offset = gguf_idx_tensor_abs_offset(&idx, i);
        strncpy(e->name, idx.names[i], sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
        total_tensor_bytes += idx.sizes[i];
    }

    qsort(entries, idx.n_tensors, sizeof(PoglsV3Entry), pogls_v3_cmp_name);
    fprintf(stderr, "[v3] Total tensor data: %.2f GB (stays in GGUF)\n",
            (double)total_tensor_bytes / (1024.0 * 1024.0 * 1024.0));

    char gguf_path_buf[1024];
    if (use_relative) {
        const char *slash = strrchr(in_path, '\\');
        const char *fslash = strrchr(in_path, '/');
        const char *fname = (slash > fslash) ? slash + 1 : (fslash ? fslash + 1 : in_path);
        strncpy(gguf_path_buf, fname, sizeof(gguf_path_buf) - 1);
    } else {
        strncpy(gguf_path_buf, in_path, sizeof(gguf_path_buf) - 1);
    }
    gguf_path_buf[sizeof(gguf_path_buf) - 1] = '\0';
    uint32_t path_sz = (uint32_t)strlen(gguf_path_buf) + 1;

    PoglsV3Header hdr;
    pogls_v3_header_init(&hdr);
    hdr.n_tensors = idx.n_tensors;
    hdr.flags = POGLS_V3_FLAG_HAS_GGUF | (use_relative ? POGLS_V3_FLAG_RELATIVE : 0);
    hdr.entries_off = sizeof(PoglsV3Header);
    hdr.gguf_path_off = sizeof(PoglsV3Header) + (uint32_t)idx.n_tensors * POGLS_V3_ENTRY_SZ;
    hdr.gguf_path_sz = path_sz;

    FILE *fout = fopen(out_path, "wb");
    if (!fout) { fprintf(stderr, "ERROR: can't create %s\n", out_path); free(entries); gguf_idx_close(&idx); return 1; }
    fwrite(&hdr, sizeof(hdr), 1, fout);
    fwrite(entries, POGLS_V3_ENTRY_SZ, idx.n_tensors, fout);
    fwrite(gguf_path_buf, path_sz, 1, fout);
    fclose(fout);

    uint64_t file_sz = pogls_v3_total_size(&hdr);
    fprintf(stderr, "[v3] Written %s (%.2f KB)\n", out_path, (double)file_sz / 1024.0);

    if (do_verify) {
        FILE *fin = fopen(out_path, "rb");
        PoglsV3Header hdr2; fread(&hdr2, sizeof(hdr2), 1, fin);
        PoglsV3Entry *entries2 = malloc((size_t)hdr2.n_tensors * POGLS_V3_ENTRY_SZ);
        fseek(fin, hdr2.entries_off, SEEK_SET);
        fread(entries2, POGLS_V3_ENTRY_SZ, hdr2.n_tensors, fin);
        fclose(fin);

        uint32_t found = 0;
        for (uint64_t i = 0; i < idx.n_tensors; i++) {
            const PoglsV3Entry *e = pogls_v3_seek(entries2, hdr2.n_tensors, idx.names[i]);
            if (e && e->gguf_offset == gguf_idx_tensor_abs_offset(&idx, i)) found++;
            else fprintf(stderr, "  MISS/MISMATCH: '%s'\n", idx.names[i]);
        }
        fprintf(stderr, "[v3] VERIFY: %u/%llu tensors OK\n", found, (unsigned long long)idx.n_tensors);
        free(entries2);
    }

    free(entries);
    gguf_idx_close(&idx);
    fprintf(stderr, "[v3] Compression vs v2: %.0fx smaller\n",
            total_tensor_bytes > 0 ? (double)total_tensor_bytes / (double)file_sz : 0);
    return 0;
}
