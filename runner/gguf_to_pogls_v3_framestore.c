#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
  #include <io.h>
  #define fseeko64 _fseeki64
#else
  #define fseeko64 fseeko
#endif

#include "pogls_v3_framestore.h"
#include "gguf_index.h"
#include "addr_space.h"

typedef struct {
    uint32_t addr;
    uint32_t nbytes;
    uint64_t gguf_off;
} SortEntry;

static int sort_cmp(const void *a, const void *b) {
    uint32_t aa = ((const SortEntry *)a)->addr;
    uint32_t bb = ((const SortEntry *)b)->addr;
    return (aa > bb) - (aa < bb);
}

int main(int argc, char **argv) {
    const char *in_path = NULL, *out_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!in_path) in_path = argv[i];
        else if (!out_path) out_path = argv[i];
    }
    if (!in_path || !out_path) {
        fprintf(stderr, "Usage: gguf_to_pogls_v3_framestore input.gguf output.framestore\n");
        return 1;
    }

    GGUFTensorIndex idx;
    memset(&idx, 0, sizeof(idx));
    if (gguf_idx_open(in_path, &idx) != 0) {
        fprintf(stderr, "ERROR: can't open %s\n", in_path); return 1;
    }
    uint64_t n = idx.n_tensors;
    fprintf(stderr, "[framestore] GGUF has %llu tensors\n", (unsigned long long)n);

    SortEntry *sort_buf = (SortEntry *)calloc(n, sizeof(SortEntry));
    if (!sort_buf) { gguf_idx_close(&idx); return 1; }

    uint64_t total_data = 0;
    for (uint64_t i = 0; i < n; i++) {
        sort_buf[i].addr    = addr_from_tensor_name(idx.names[i], 0);
        sort_buf[i].nbytes  = (uint32_t)idx.sizes[i];
        sort_buf[i].gguf_off = gguf_idx_tensor_abs_offset(&idx, i);
        total_data += idx.sizes[i];
    }

    qsort(sort_buf, n, sizeof(SortEntry), sort_cmp);
    fprintf(stderr, "[framestore] Total tensor data: %.2f GB\n",
            (double)total_data / (1024.0 * 1024.0 * 1024.0));

    FrameStoreHeader hdr;
    framestore_header_init(&hdr);
    hdr.n_tensors   = (uint32_t)n;
    hdr.data_off     = FRAMESTORE_HEADER_SZ + (uint32_t)n * FRAMESTORE_ENTRY_SZ;

    FrameStoreEntry *entries = (FrameStoreEntry *)calloc(n, sizeof(FrameStoreEntry));
    if (!entries) { free(sort_buf); gguf_idx_close(&idx); return 1; }

    uint64_t curr_off = hdr.data_off;
    for (uint64_t i = 0; i < n; i++) {
        entries[i].addr        = sort_buf[i].addr;
        entries[i].nbytes      = sort_buf[i].nbytes;
        entries[i].file_offset = curr_off;
        curr_off += sort_buf[i].nbytes;
    }

    FILE *fout = fopen(out_path, "wb");
    if (!fout) {
        fprintf(stderr, "ERROR: can't create %s\n", out_path);
        free(entries); free(sort_buf); gguf_idx_close(&idx); return 1;
    }

    if (fwrite(&hdr, FRAMESTORE_HEADER_SZ, 1, fout) != 1) {
        fprintf(stderr, "ERROR: header write failed\n");
        fclose(fout); free(entries); free(sort_buf); gguf_idx_close(&idx); return 1;
    }

    if (fwrite(entries, FRAMESTORE_ENTRY_SZ, n, fout) != n) {
        fprintf(stderr, "ERROR: addr map write failed\n");
        fclose(fout); free(entries); free(sort_buf); gguf_idx_close(&idx); return 1;
    }

    FILE *fin = fopen(in_path, "rb");
    if (!fin) {
        fprintf(stderr, "ERROR: can't re-open GGUF for data read\n");
        fclose(fout); free(entries); free(sort_buf); gguf_idx_close(&idx); return 1;
    }

    uint64_t data_written = 0;
    uint8_t *buf = (uint8_t *)malloc(64 * 1024 * 1024);
    if (!buf) { fprintf(stderr, "ERROR: OOM\n"); fclose(fin); fclose(fout); free(entries); free(sort_buf); gguf_idx_close(&idx); return 1; }

    for (uint64_t i = 0; i < n; i++) {
        uint32_t sz = sort_buf[i].nbytes;
        if (sz == 0) continue;

        if (sz > 64 * 1024 * 1024) {
            free(buf);
            buf = (uint8_t *)malloc(sz);
            if (!buf) { fprintf(stderr, "ERROR: OOM at tensor %llu (%u bytes)\n", (unsigned long long)i, sz); break; }
        }

        if (fseeko64(fin, (off64_t)sort_buf[i].gguf_off, SEEK_SET) != 0 ||
            fread(buf, sz, 1, fin) != 1) {
            fprintf(stderr, "ERROR: read tensor %llu failed (GGUF off %llu, sz %u)\n",
                    (unsigned long long)i, (unsigned long long)sort_buf[i].gguf_off, sz);
            break;
        }

        if (fwrite(buf, sz, 1, fout) != 1) {
            fprintf(stderr, "ERROR: write tensor %llu failed\n", (unsigned long long)i);
            break;
        }
        data_written += sz;
    }

    free(buf);
    fclose(fin);
    fclose(fout);

    uint64_t file_size = (uint64_t)hdr.data_off + data_written;
    fprintf(stderr, "[framestore] Written %s (%.2f GB, %llu tensors, addr_map=%.1f KB)\n",
            out_path,
            (double)file_size / (1024.0 * 1024.0 * 1024.0),
            (unsigned long long)n,
            (double)n * FRAMESTORE_ENTRY_SZ / 1024.0);

    fprintf(stderr, "[framestore] Verifying...\n");
    FrameStoreHeader hdr2;
    if (framestore_read_header(out_path, &hdr2) != 0) {
        fprintf(stderr, "  VERIFY FAIL: header read\n"); free(entries); free(sort_buf); gguf_idx_close(&idx); return 1;
    }
    if (hdr2.n_tensors != n) {
        fprintf(stderr, "  VERIFY FAIL: n_tensors mismatch\n"); free(entries); free(sort_buf); gguf_idx_close(&idx); return 1;
    }

    FILE *fver = fopen(out_path, "rb");
    if (!fver) { fprintf(stderr, "  VERIFY FAIL: reopen\n"); free(entries); free(sort_buf); gguf_idx_close(&idx); return 1; }
    fseeko64(fver, (off64_t)hdr2.addr_map_off, SEEK_SET);
    FrameStoreEntry *entries2 = (FrameStoreEntry *)malloc((size_t)n * FRAMESTORE_ENTRY_SZ);
    if (fread(entries2, FRAMESTORE_ENTRY_SZ, n, fver) != n) {
        fprintf(stderr, "  VERIFY FAIL: read map\n"); fclose(fver); free(entries2); free(entries); free(sort_buf); gguf_idx_close(&idx); return 1;
    }
    fclose(fver);

    uint32_t verify_ok = 0, verify_bad = 0;
    for (uint64_t i = 0; i < n; i++) {
        const FrameStoreEntry *e = framestore_seek(entries2, (uint32_t)n, entries[i].addr);
        if (e && e->nbytes == entries[i].nbytes && e->file_offset == entries[i].file_offset)
            verify_ok++;
        else
            verify_bad++;
    }
    fprintf(stderr, "  ADDR MAP: %u/%llu OK, %u FAIL\n", verify_ok, (unsigned long long)n, verify_bad);

    uint32_t data_ok = 0, data_bad = 0;
    fin = fopen(in_path, "rb");
    fver = fopen(out_path, "rb");
    if (fin && fver) {
        uint8_t *dbuf = (uint8_t *)malloc(4 * 1024 * 1024);
        uint8_t *gbuf = (uint8_t *)malloc(4 * 1024 * 1024);
        if (dbuf && gbuf) {
            for (uint64_t i = 0; i < n; i++) {
                uint32_t sz = sort_buf[i].nbytes;
                uint64_t fs_off = entries[i].file_offset;
                uint64_t gguf_off = sort_buf[i].gguf_off;
                uint32_t check_sz = sz < (4*1024*1024) ? sz : (4*1024*1024);

                fseeko64(fver, (off64_t)fs_off, SEEK_SET);
                fread(dbuf, check_sz, 1, fver);
                fseeko64(fin, (off64_t)gguf_off, SEEK_SET);
                fread(gbuf, check_sz, 1, fin);

                if (memcmp(dbuf, gbuf, (size_t)check_sz) == 0) data_ok++;
                else data_bad++;
            }
        }
        fclose(fver); fclose(fin); free(dbuf); free(gbuf);
        fprintf(stderr, "  DATA VERIFY: %u/%llu OK, %u BAD\n", data_ok, (unsigned long long)n, data_bad);
    }

    free(entries2);
    free(entries);
    free(sort_buf);
    gguf_idx_close(&idx);

    if (verify_bad > 0 || data_bad > 0) {
        fprintf(stderr, "[framestore] FAILED\n");
        return 1;
    }
    fprintf(stderr, "[framestore] All checks PASS\n");
    return 0;
}
