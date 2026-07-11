/*
 * pogls_verify.c — Verify POGLS file integrity
 *
 * Usage: pogls_verify <file.pogls>
 *
 * Checks: magic, header, tensor meta validity, data section alignment.
 * Reports PASS/FAIL per check.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pogls_core.h"

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <file.pogls>\n", prog);
    fprintf(stderr, "  Verifies POGLS file integrity.\n");
    fprintf(stderr, "  Returns 0 on all-pass, 1 on any failure.\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    const char *path = argv[1];

    if (strcmp(path, "--help") == 0 || strcmp(path, "-h") == 0) {
        usage(argv[0]); return 0;
    }

    int pass = 1;
    int checks = 0, fails = 0;

    #define CHECK(name, cond, ...) do { \
        checks++; \
        if (cond) { \
            printf("  PASS  %s\n", name); \
        } else { \
            printf("  FAIL  %s — ", name); \
            printf(__VA_ARGS__); \
            printf("\n"); \
            fails++; pass = 0; \
        } \
    } while(0)

    printf("═══ POGLS Verify: %s ═══\n\n", path);

    /* Read raw header */
    FILE *f = pogls_fopen(path, "rb");
    if (!f) { fprintf(stderr, "Error: cannot open %s\n", path); return 1; }

    pogls_fseek(f, 0, SEEK_END);
    uint64_t file_sz = (uint64_t)pogls_ftell(f);
    pogls_fseek(f, 0, SEEK_SET);

    CHECK("file_not_empty", file_sz > 0, "file is empty (%llu bytes)", (unsigned long long)file_sz);

    if (file_sz < POGLS_HEADER_SZ) {
        printf("  FAIL  header_too_small (%llu < %u)\n",
               (unsigned long long)file_sz, POGLS_HEADER_SZ);
        fclose(f);
        return 1;
    }

    /* Read header */
    PoglsStoreHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        printf("  FAIL  header_read\n");
        fclose(f);
        return 1;
    }

    CHECK("magic", hdr.magic == POGLS_META_MAGIC || hdr.magic == 0x504F474Cu,
          "got 0x%08X (expected 0x%08X or 0x%08X)",
          hdr.magic, POGLS_META_MAGIC, 0x504F474Cu);
    CHECK("version", hdr.version >= 1 && hdr.version <= 3,
          "got %u", hdr.version);
    CHECK("n_tensors", hdr.n_tensors > 0,
          "got %u", hdr.n_tensors);
    CHECK("header_flags", (hdr.flags & ~0x07u) == 0,
          "unknown flags 0x%04X", hdr.flags & ~0x07u);

    /* Check data offset alignment */
    uint64_t data_off = POGLS_HEADER_SZ + (uint64_t)POGLS_INDEX_SZ;
    CHECK("data_alignment", (data_off % 32) == 0,
          "data at %llu not 32-byte aligned", (unsigned long long)data_off);

    CHECK("data_fits", data_off <= file_sz,
          "data_offset %llu > file_size %llu",
          (unsigned long long)data_off, (unsigned long long)file_sz);

    /* Tensor meta validation */
    if (hdr.tensor_meta_off > 0 && hdr.tensor_meta_count > 0) {
        CHECK("tmeta_offset", hdr.tensor_meta_off + (uint64_t)hdr.tensor_meta_count * sizeof(PoglsTensorMeta) <= file_sz,
              "tmeta overflows file");

        /* Read and validate each tensor meta */
        uint32_t bad_meta = 0;
        fseek(f, (long)hdr.tensor_meta_off, SEEK_SET);
        for (uint32_t i = 0; i < hdr.tensor_meta_count; i++) {
            PoglsTensorMeta tm;
            if (fread(&tm, sizeof(tm), 1, f) != 1) { bad_meta++; continue; }
            if (tm.comp_nbytes > 0 && tm.comp_nbytes < tm.nbytes_orig)
                bad_meta++; /* comp smaller than orig is ok */
        }
        CHECK("tensor_meta_valid", bad_meta == 0,
              "%u tensors have invalid meta", bad_meta);
    } else if (hdr.flags & POGLS_FLAG_HAS_TMETA) {
        printf("  WARN  HAS_TMETA flag set but no tensor_meta_off\n");
    }

    /* Model meta validation */
    if (hdr.model_meta_off > 0 && hdr.model_meta_sz > 0) {
        CHECK("mmeta_fits", hdr.model_meta_off + hdr.model_meta_sz <= file_sz,
              "mmeta overflows file");
    }

    printf("\n═══ Result: %s (%d/%d checks passed) ═══\n",
           pass ? "ALL PASS" : "SOME FAILED",
           checks - fails, checks);

    fclose(f);
    return pass ? 0 : 1;
}
