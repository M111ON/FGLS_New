#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <direct.h>
#else
  #include <sys/stat.h>
#endif
#include "gguf_index.h"
#include "capture_pipeline.h"

#define BAKE_MAX_TENSORS 4096

static int read_tensor_data(const char *path, GGUFTensorIndex *idx,
                             uint64_t ti, uint8_t **out, uint64_t *out_nbytes)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint64_t off = gguf_idx_tensor_abs_offset(idx, ti);
    uint64_t sz = idx->sizes[ti];
    if (sz == 0) { fclose(f); *out = NULL; *out_nbytes = 0; return 0; }
    uint8_t *buf = (uint8_t*)malloc(sz);
    if (!buf) { fclose(f); return -1; }
    int ok = (fseek(f, (long)off, SEEK_SET) == 0 &&
              fread(buf, 1, sz, f) == sz);
    fclose(f);
    if (!ok) { free(buf); return -1; }
    *out = buf;
    *out_nbytes = sz;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: bake_tool <model.gguf> [output_dir]\n");
        fprintf(stderr, "  output_dir defaults to ./bake_output/\n");
        return 1;
    }

    const char *gguf_path = argv[1];
    const char *out_dir   = (argc >= 3) ? argv[2] : "./bake_output";

    /* ── Open GGUF index ── */
    GGUFTensorIndex idx;
    if (gguf_idx_open(gguf_path, &idx) != 0) {
        fprintf(stderr, "ERROR: cannot open GGUF: %s\n", gguf_path);
        return 1;
    }
    uint64_t n = idx.n_tensors;
    if (n > BAKE_MAX_TENSORS) n = BAKE_MAX_TENSORS;
    fprintf(stderr, "[bake] GGUF: %llu tensors\n", (unsigned long long)idx.n_tensors);

    /* ── Prepare output directory ── */
#ifdef _WIN32
    _mkdir(out_dir);
#else
    mkdir(out_dir, 0755);
#endif

    /* ── Allocate CaptureTensor array ── */
    CaptureTensor *ctens = (CaptureTensor*)calloc(n, sizeof(CaptureTensor));
    char **names = (char**)calloc(n, sizeof(char*));
    if (!ctens || !names) {
        fprintf(stderr, "ERROR: out of memory\n");
        return 1;
    }

    /* ── Read each tensor from disk → capture ── */
    CaptureResult cr;
    capture_init(&cr);

    for (uint64_t i = 0; i < n; i++) {
        uint8_t *data = NULL;
        uint64_t nbytes = 0;
        if (read_tensor_data(gguf_path, &idx, i, &data, &nbytes) != 0 || !data) {
            fprintf(stderr, "[bake] WARN: cannot read tensor %llu (%s), skipping\n",
                    (unsigned long long)i, idx.names[i] ? idx.names[i] : "?");
            continue;
        }

        names[i] = (char*)malloc(strlen(idx.names[i]) + 1);
        if (names[i]) strcpy(names[i], idx.names[i]);
        ctens[i].name   = names[i];
        ctens[i].data   = data;
        ctens[i].nbytes = nbytes;
        ctens[i].dtype  = (int)idx.dtypes[i];

        capture_tensor(&cr, data, nbytes, (int)idx.dtypes[i],
                       (uint32_t)i, (uint8_t)(i % 64));

        /* free tensor data after capture (we only need it in cr for freeze wallet;
         * capture_write_store will re-read ctens which still has pointers) */
        /* Actually keep it — capture_write_store needs the data pointer later */
    }

    fprintf(stderr, "[bake] Captured %u/%llu tensors\n",
            cr.n_tensors_captured, (unsigned long long)idx.n_tensors);

    /* ── Write freeze wallet ── */
    size_t tw_bytes = capture_write_freeze_wallet(&cr, out_dir);
    fprintf(stderr, "[bake] Freeze wallet: %zu bytes\n", tw_bytes);

    /* ── Write tensor store ── */
    size_t gsten_bytes = capture_write_store(&cr, out_dir,
                                              ctens, (int)cr.n_tensors_captured);
    fprintf(stderr, "[bake] Tensor store: %zu bytes\n", gsten_bytes);

    /* ── Verify ── */
    capture_verify(&cr, ctens, (int)cr.n_tensors_captured);
    capture_summary(&cr, stderr);

    /* ── Cleanup ── */
    for (uint64_t i = 0; i < n; i++) {
        free((void*)ctens[i].data);
        free(names[i]);
    }
    free(ctens);
    free(names);
    gguf_idx_close(&idx);

    fprintf(stderr, "[bake] Done. Output in %s/\n", out_dir);
    return 0;
}
