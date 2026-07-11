/*
 * pogls_inspect.c — Dump .pogls file metadata to console
 *
 * Usage: pogls_inspect <file.pogls>
 *
 * Reads header, index, tensor meta, model meta.
 * Prints human-readable summary.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pogls_core.h"

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <file.pogls>\n", prog);
    fprintf(stderr, "  Dumps POGLS file header, tensor count, metadata.\n");
}

static const char* dtype_name(uint32_t dt) {
    switch (dt) {
        case 0:  return "F32";
        case 1:  return "F16";
        case 8:  return "Q8_0";
        case 9:  return "Q4_0";
        case 10: return "Q4_1";
        case 11: return "Q5_0";
        case 12: return "Q5_1";
        case 13: return "Q8_K";
        case 14: return "Q6_K";
        case 15: return "Q4_K";
        case 16: return "Q5_K";
        case 17: return "Q6_K";
        case 18: return "Q4_0_4_4";
        case 19: return "Q4_0_4_8";
        case 20: return "Q4_0_8_8";
        default: return "???";
    }
}

static const char* comp_name(uint32_t c) {
    switch (c) {
        case POGLS_COMP_RAW:      return "RAW";
        case POGLS_COMP_ZSTD:     return "ZSTD";
        case POGLS_COMP_SHELL:    return "SHELL";
        case POGLS_COMP_DELTA:    return "DELTA";
        case POGLS_COMP_GEOPIXEL: return "GEOPIXEL";
        default: return "???";
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    const char *path = argv[1];

    if (strcmp(path, "--help") == 0 || strcmp(path, "-h") == 0) {
        usage(argv[0]); return 0;
    }

    PoglsStoreHeader hdr;
    uint8_t *idx = NULL;
    PoglsTensorMeta *meta = NULL;
    uint64_t data_off = 0;

    /* Read file — try v2 first, fallback to raw read */
    int rc = pogls_meta_read(path, &hdr, NULL, NULL, &data_off);
    if (rc != 0) {
        /* Maybe v1 or unknown format — read magic manually */
        FILE *f = pogls_fopen(path, "rb");
        if (!f) { fprintf(stderr, "Error: cannot open %s\n", path); return 1; }
        uint32_t magic = 0;
        if (fread(&magic, 4, 1, f) != 1) { fclose(f); return 1; }
        /* Accept both POGLS (0x53474F50) and legacy POGL (0x504F474C) */
        if (magic != 0x53474F50u && magic != 0x504F474Cu) {
            fprintf(stderr, "Error: not a POGLS file (magic=0x%08X)\n", magic);
            fclose(f);
            return 1;
        }
        /* It's a valid POGLS file — read header manually */
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic = magic;
        fseek(f, 0, SEEK_SET);
        fread(&hdr, sizeof(hdr), 1, f);
        hdr.magic = magic; /* restore */
        pogls_fseek(f, 0, SEEK_END);
        data_off = (uint64_t)pogls_ftell(f); /* data goes to EOF */
        fclose(f);
    }

    /* Print header */
    printf("═══ POGLS File Inspector ═══\n");
    printf("File:       %s\n", path);
    printf("Magic:      0x%08X (%s)\n", hdr.magic,
           hdr.magic == POGLS_META_MAGIC ? "POGS" : "???");
    printf("Version:    %u\n", hdr.version);
    printf("Tensors:    %u\n", hdr.n_tensors);
    printf("Flags:      0x%04X", hdr.flags);
    if (hdr.flags & POGLS_FLAG_HAS_TMETA) printf(" HAS_TMETA");
    if (hdr.flags & POGLS_FLAG_HAS_MMETA) printf(" HAS_MMETA");
    if (hdr.flags & POGLS_FLAG_MCOMPRESS) printf(" MCOMPRESS");
    printf("\n");

    /* Re-read with full data */
    if (hdr.tensor_meta_count > 0) {
        meta = (PoglsTensorMeta*)calloc(hdr.tensor_meta_count, sizeof(PoglsTensorMeta));
        if (meta) {
            FILE *f = pogls_fopen(path, "rb");
            if (f) {
                pogls_fseek(f, (int64_t)hdr.tensor_meta_off, SEEK_SET);
                fread(meta, sizeof(PoglsTensorMeta), hdr.tensor_meta_count, f);
                fclose(f);
            }
        }
    }

    printf("Header sz:  %u\n", POGLS_HEADER_SZ);
    printf("Index sz:   %llu\n", (unsigned long long)POGLS_INDEX_SZ);
    printf("Data off:   %llu\n", (unsigned long long)data_off);

    if (hdr.tensor_meta_off > 0)
        printf("TMeta off:  %llu (%u entries)\n",
               (unsigned long long)hdr.tensor_meta_off, hdr.tensor_meta_count);
    if (hdr.model_meta_off > 0)
        printf("MMeta off:  %llu (%u bytes)\n",
               (unsigned long long)hdr.model_meta_off, hdr.model_meta_sz);

    /* Print tensor meta */
    if (meta && hdr.tensor_meta_count > 0) {
        printf("\n═══ Tensor Metadata (%u) ═══\n", hdr.tensor_meta_count);
        size_t total_orig = 0, total_comp = 0;
        uint32_t n_compressed = 0;
        for (uint32_t i = 0; i < hdr.tensor_meta_count && i < 20; i++) {
            printf("  [%4u] addr=%5u  dtype=%-5s  dims=%ux%ux%ux%u  "
                   "orig=%u  comp=%s(%u)\n",
                   i, meta[i].addr, dtype_name(meta[i].dtype),
                   meta[i].dims[0], meta[i].dims[1],
                   meta[i].dims[2], meta[i].dims[3],
                   meta[i].nbytes_orig,
                   comp_name(meta[i].comp_type), meta[i].comp_nbytes);
            total_orig += meta[i].nbytes_orig;
            total_comp += meta[i].comp_nbytes > 0 ? meta[i].comp_nbytes : meta[i].nbytes_orig;
            if (meta[i].comp_type != POGLS_COMP_RAW) n_compressed++;
        }
        if (hdr.tensor_meta_count > 20)
            printf("  ... (%u more)\n", hdr.tensor_meta_count - 20);

        printf("\n═══ Summary ═══\n");
        printf("Total orig: %zu bytes (%.2f MB)\n", total_orig, total_orig / 1048576.0);
        printf("Total comp: %zu bytes (%.2f MB)\n", total_comp, total_comp / 1048576.0);
        printf("Compressed: %u / %u tensors\n", n_compressed, hdr.tensor_meta_count);
        if (total_orig > 0)
            printf("Overall:    %.2fx\n", (double)total_orig / (double)total_comp);
    }

    free(meta);
    return 0;
}
