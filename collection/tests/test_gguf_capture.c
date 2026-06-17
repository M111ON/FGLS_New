/* test_gguf_capture.c — Stream SID capture directly from GGUF
 * Reads GGUF header, for each tensor reads 68 bytes, captures SID.
 * No full tensor load needed — true streaming capture.
 *
 * Build:
 *   gcc -DGEO_JUMP_INLINE -I. -I../src -I../geo_jump_module/include
 *       -I../../src -o tests/test_gguf_capture.exe tests/test_gguf_capture.c -lm
 * Run:
 *   test_gguf_capture.exe model.gguf
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SID_IMPLEMENTATION
#include "sid.h"

/* ── GGUF v3 constants ── */
#define GGUF_MAGIC   0x46554747u  /* "GGUF" */
#define GGUF_F32     0u
#define GGUF_Q8_0    8u

/* Read a GGUF file header + tensor info without loading weights */
typedef struct {
    uint64_t n_tensors;
    char   **names;
    uint32_t *dtypes;       /* ggml type per tensor */
    uint64_t *offsets;      /* byte offset in file */
    uint64_t *sizes;        /* byte size of tensor data */
} GGUFTensorIndex;

static int gguf_read_index(const char *path, GGUFTensorIndex *idx) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return -1; }

    /* Header */
    uint32_t magic; uint32_t version; uint64_t n_tensors, n_kv;
    if (fread(&magic, 4, 1, f) != 1) { fclose(f); return -1; }
    if (fread(&version, 4, 1, f) != 1) { fclose(f); return -1; }
    if (fread(&n_tensors, 8, 1, f) != 1) { fclose(f); return -1; }
    if (fread(&n_kv, 8, 1, f) != 1) { fclose(f); return -1; }

    if (magic != GGUF_MAGIC) {
        fprintf(stderr, "Bad magic: 0x%08x\n", magic); fclose(f); return -1;
    }
    fprintf(stderr, "Header: ver=%u tensors=%llu kv=%llu\n",
            version, (unsigned long long)n_tensors, (unsigned long long)n_kv);
    fflush(stderr);

    /* Skip metadata KV pairs */
    for (uint64_t i = 0; i < n_kv; i++) {
        /* key: string */
        uint64_t klen;
        if (fread(&klen, 8, 1, f) != 1) { fclose(f); return -1; }
        fseek(f, klen, SEEK_CUR);
        /* value type (GGUF v3) */
        uint32_t vtype;
        if (fread(&vtype, 4, 1, f) != 1) { fclose(f); return -1; }
        switch (vtype) {
            case 0: case 1: fseek(f, 1, SEEK_CUR); break;   /* uint8, int8 */
            case 2: case 3: fseek(f, 2, SEEK_CUR); break;   /* uint16, int16 */
            case 4: case 5: case 6: fseek(f, 4, SEEK_CUR); break; /* uint32, int32, float32 */
            case 7: fseek(f, 1, SEEK_CUR); break;           /* bool */
            case 8: { /* string */
                uint64_t slen;
                if (fread(&slen, 8, 1, f) != 1) { fclose(f); return -1; }
                fseek(f, slen, SEEK_CUR);
                break;
            }
            case 9: { /* array */
                uint32_t arrtype;
                uint64_t arrlen;
                if (fread(&arrtype, 4, 1, f) != 1) { fclose(f); return -1; }
                if (fread(&arrlen, 8, 1, f) != 1) { fclose(f); return -1; }
                size_t elem_size = 0;
                switch (arrtype) {
                    case 0: case 1: elem_size=1; break;
                    case 2: case 3: elem_size=2; break;
                    case 4: case 5: case 6: elem_size=4; break;
                    case 7: elem_size=1; break;
                    case 8: /* array of strings */
                        for (uint64_t j = 0; j < arrlen; j++) {
                            uint64_t sl;
                            if (fread(&sl, 8, 1, f) != 1) { fclose(f); return -1; }
                            fseek(f, sl, SEEK_CUR);
                        }
                        continue;
                    case 10: case 11: case 12: elem_size=8; break; /* uint64, int64, float64 */
                    default: elem_size=4; break;
                }
                fseek(f, (long)(elem_size * arrlen), SEEK_CUR);
                break;
            }
            case 10: case 11: case 12: fseek(f, 8, SEEK_CUR); break; /* uint64, int64, float64 */
            default:
                fprintf(stderr, "Unknown metadata vtype=%u at KV %llu\n", vtype, (unsigned long long)i);
                fseek(f, 4, SEEK_CUR);
                break;
        }
    }
    fprintf(stderr, "Metadata parsed OK, n_kv=%llu\n", (unsigned long long)n_kv);
    fflush(stderr);

    fprintf(stderr, "KV skip done. Reading tensor info...\n"); fflush(stderr);
    /* Read tensor info */
    idx->n_tensors = n_tensors;
    idx->names = calloc(n_tensors, sizeof(char*));
    idx->dtypes = calloc(n_tensors, sizeof(uint32_t));
    idx->offsets = calloc(n_tensors, sizeof(uint64_t));
    idx->sizes = calloc(n_tensors, sizeof(uint64_t));

    for (uint64_t i = 0; i < n_tensors; i++) {
        /* tensor name */
        uint64_t nlen;
        if (fread(&nlen, 8, 1, f) != 1) { fclose(f); return -1; }
        idx->names[i] = calloc(nlen + 1, 1);
        if (fread(idx->names[i], 1, nlen, f) != nlen) { fclose(f); return -1; }

        /* n_dims */
        uint32_t nd;
        if (fread(&nd, 4, 1, f) != 1) { fclose(f); return -1; }

        /* shape (dims) */
        for (uint32_t d = 0; d < nd; d++) {
            uint64_t dim;
            if (fread(&dim, 8, 1, f) != 1) { fclose(f); return -1; }
        }

        /* dtype */
        uint32_t dtype;
        if (fread(&dtype, 4, 1, f) != 1) { fclose(f); return -1; }
        idx->dtypes[i] = dtype;

        /* offset */
        uint64_t offset;
        if (fread(&offset, 8, 1, f) != 1) { fclose(f); return -1; }
        idx->offsets[i] = offset;
    }

    fprintf(stderr, "Tensor info read OK (%llu tensors)\n",
            (unsigned long long)n_tensors); fflush(stderr);
    /* Compute sizes from offsets (next - current) */
    /* The data section starts after the header. Offsets are relative to data start.
     * We need to convert to file-absolute offsets. */
    long data_start = ftell(f);  /* This is where data begins */
    for (uint64_t i = 0; i < n_tensors; i++) {
        idx->offsets[i] += data_start;  /* Convert to file-absolute */
    }

    /* Size of each = difference between consecutive offsets */
    /* Sort by offset to compute sizes */
    /* Simple: copy and sort by offset */
    typedef struct { uint64_t off; int idx; } OffSort;
    OffSort *sorted = calloc(n_tensors, sizeof(OffSort));
    for (uint64_t i = 0; i < n_tensors; i++) {
        sorted[i].off = idx->offsets[i];
        sorted[i].idx = i;
    }
    /* Bubble sort (small n, n=339 for 1.5B) */
    for (uint64_t i = 0; i < n_tensors; i++) {
        for (uint64_t j = i+1; j < n_tensors; j++) {
            if (sorted[j].off < sorted[i].off) {
                OffSort t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t;
            }
        }
    }
    fprintf(stderr, "Data start=%ld, sorting sizes...\n", data_start); fflush(stderr);
    /* File size for last tensor's end */
    fseek(f, 0, SEEK_END);
    long file_end = ftell(f);
    fprintf(stderr, "File size=%ld\n", file_end); fflush(stderr);

    for (uint64_t i = 0; i < n_tensors; i++) {
        int ti = sorted[i].idx;
        if (i < n_tensors - 1) {
            idx->sizes[ti] = sorted[i+1].off - idx->offsets[ti];
        } else {
            idx->sizes[ti] = file_end - idx->offsets[ti];
        }
    }

    free(sorted);
    fclose(f);
    return 0;
}

static void gguf_free_index(GGUFTensorIndex *idx) {
    for (uint64_t i = 0; i < idx->n_tensors; i++)
        free(idx->names[i]);
    free(idx->names);
    free(idx->dtypes);
    free(idx->offsets);
    free(idx->sizes);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: test_gguf_capture.exe <model.gguf>\n");
        return 1;
    }

    const char *path = argv[1];

    /* Read GGUF index (no weights loaded) */
    GGUFTensorIndex idx;
    if (gguf_read_index(path, &idx) != 0) return 1;
    printf("GGUF tensors: %llu\n", (unsigned long long)idx.n_tensors);

    /* Stream capture: open file, for each tensor seek+read 68B → capture */
    FILE *f = fopen(path, "rb");
    if (!f) { gguf_free_index(&idx); return 1; }

    SIDStore store;
    memset(&store, 0, sizeof(store));

    uint8_t buf[256];  /* up to 64 F32 values */
    int n_captured = 0;
    int n_ignored = 0;

    for (uint64_t i = 0; i < idx.n_tensors; i++) {
        int dtype = idx.dtypes[i];

        /* Accept Q8_0 (8) and F32 (0) */
        if (dtype != GGUF_Q8_0 && dtype != GGUF_F32) {
            n_ignored++;
            continue;
        }

        /* Determine read size: 2 Q8_0 blocks (68B) or 64 F32 values (256B) */
        size_t to_read;
        if (dtype == GGUF_Q8_0) {
            to_read = idx.sizes[i] < 68 ? idx.sizes[i] : 68;
        } else {
            to_read = idx.sizes[i] < 256 ? idx.sizes[i] : 256;
        }

        fseek(f, idx.offsets[i], SEEK_SET);
        if (fread(buf, 1, to_read, f) != to_read) continue;

        /* Capture */
        SIDCoord coord;
        if (sid_capture(buf, to_read, dtype, &coord) != 0) continue;

        /* Manually add to store */
        strncpy(store.entries[store.n_entries].name, idx.names[i], SID_NAME_MAX - 1);
        store.entries[store.n_entries].coord = coord;
        store.n_entries++;

        n_captured++;
        if (n_captured % 50 == 0)
            printf("  Captured %d/%llu...\n", n_captured,
                   (unsigned long long)idx.n_tensors);
    }

    /* ── Roundtrip verification for F32 tensors (before fclose) ── */
    printf("\n  Verifying F32 roundtrip...\n");
    uint8_t *full_buf = NULL;
    int n_verify = 0, n_verify_ok = 0;

    for (uint64_t i = 0; i < idx.n_tensors; i++) {
        if (idx.dtypes[i] != GGUF_F32) continue;
        if (n_verify >= 24) break;

        size_t sz = (size_t)idx.sizes[i];
        full_buf = realloc(full_buf, sz);
        fseek(f, idx.offsets[i], SEEK_SET);
        if (fread(full_buf, 1, sz, f) != sz) continue;

        int rc = sid_verify_roundtrip(full_buf, sz, GGUF_F32, idx.names[i]);
        if (rc == 0) n_verify_ok++;
        n_verify++;

        if (n_verify % 12 == 0)
            printf("    Verified %d F32 tensors...\n", n_verify);
    }
    free(full_buf);

    printf("  Roundtrip: %d/%d F32 tensors verified\n", n_verify_ok, n_verify);

    fclose(f);

    printf("\nResults:\n");
    printf("  Tensors in GGUF: %llu\n", (unsigned long long)idx.n_tensors);
    printf("  Captured: %d\n", n_captured);
    printf("  Ignored (non-Q8/F16): %d\n", n_ignored);
    printf("  .twidx size: %zu bytes\n", store.n_entries * sizeof(SIDEntry));

    /* Per-pentagon stats */
    int pent_hist[12] = {0};
    for (uint32_t i = 0; i < store.n_entries; i++) {
        uint32_t pent = geo_pentagon_id(store.entries[i].coord.node_id);
        if (pent >= 1 && pent <= 12) pent_hist[pent-1]++;
    }

    printf("\n  Pentagon distribution:\n");
    for (int p = 0; p < 12; p++)
        printf("    p%d: %d\n", p+1, pent_hist[p]);

    int uniq = 0;
    for (int i = 0; i < 12; i++)
        if (pent_hist[i]) uniq++;
    printf("  Unique pentagons: %d/12 (%.1f%%)\n",
           uniq, 100.0*uniq/12);

    /* Write .twidx */
    char twidx_path[1024];
    snprintf(twidx_path, sizeof(twidx_path), "%s.twidx",
             strrchr(path, '/') ? strrchr(path, '/')+1 : path);
    /* Remove .gguf extension */
    char *dot = strstr(twidx_path, ".gguf");
    if (dot) *dot = 0;
    strcat(twidx_path, ".twidx");

    if (sid_write(twidx_path, &store) > 0)
        printf("  Written: %s\n", twidx_path);

    gguf_free_index(&idx);

    printf("\nDone — stream capture (Q8=68B/tensor, F32=256B/tensor)\n");
    return 0;
}
