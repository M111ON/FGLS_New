#include "pogls_v3_geoframe.h"
#include "gguf_reader.h"
#include "addr_space.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int main(int argc, char **argv) {
    if (argc < 2 || argc > 4) {
        fprintf(stderr, "Usage: gguf_to_geoframe MODEL.gguf [OUTPUT.geo] [--verify]\n");
        return 1;
    }

    const char *model_path = argv[1];
    char outpath[1024];
    int do_verify = 0;

    if (argc >= 3) {
        if (strcmp(argv[2], "--verify") == 0) {
            size_t len = strlen(model_path);
            if (len > 5 && strcmp(model_path + len - 5, ".gguf") == 0) {
                memcpy(outpath, model_path, len - 5);
                memcpy(outpath + len - 5, ".geo", 5);
            } else {
                snprintf(outpath, sizeof(outpath), "%s.geo", model_path);
            }
            do_verify = 1;
        } else {
            strncpy(outpath, argv[2], sizeof(outpath) - 1);
            if (argc >= 4 && strcmp(argv[3], "--verify") == 0) do_verify = 1;
        }
    } else {
        size_t len = strlen(model_path);
        if (len > 5 && strcmp(model_path + len - 5, ".gguf") == 0) {
            memcpy(outpath, model_path, len - 5);
            memcpy(outpath + len - 5, ".geo", 5);
        } else {
            snprintf(outpath, sizeof(outpath), "%s.geo", model_path);
        }
    }

    GgufReader ggr;
    int r = gguf_open(model_path, &ggr);
    if (r != 0) { fprintf(stderr, "FAIL: gguf_open '%s'\n", model_path); return 1; }
    uint32_t n = ggr.n_tensors;

    uint32_t *addrs = (uint32_t *)malloc(n * sizeof(uint32_t));
    uint32_t *sizes = (uint32_t *)malloc(n * sizeof(uint32_t));
    uint8_t **datas = (uint8_t **)malloc(n * sizeof(uint8_t *));
    if (!addrs || !sizes || !datas) { fprintf(stderr, "OOM\n"); return 1; }

    uint64_t total_data = 0;
    for (uint32_t i = 0; i < n; i++) {
        addrs[i] = addr_from_tensor_name(ggr.names[i], 0);
        sizes[i] = ggr.sizes[i];
        if (sizes[i] > (uint32_t)-1 - 16) { fprintf(stderr, "tensor %u too large\n", i); return 1; }
        datas[i] = (uint8_t *)malloc(sizes[i] ? sizes[i] : 1);
        if (!datas[i]) { fprintf(stderr, "OOM tensor %u\n", i); return 1; }
        if (sizes[i] > 0) {
            r = gguf_read_tensor(model_path, &ggr, i, datas[i], sizes[i]);
            if (r != 0) { fprintf(stderr, "FAIL read[%u] err=%d\n", i, r); return 1; }
            total_data += sizes[i];
        }
        if (i < 5 || i >= n-2 || (i % 64) == 0)
            fprintf(stderr, "  [%4u] addr=%5u  size=%7u  %s\n", i, addrs[i], sizes[i], ggr.names[i]);
        if (i == 5) fprintf(stderr, "  ... (%u tensors)\n", n);
    }
    fprintf(stderr, "[gguf] total tensor data: %I64u bytes (%.2f MB)\n", (uint64_t)total_data, total_data / 1048576.0);

    r = geof_write(outpath, addrs, (const uint8_t *const *)datas, sizes, n);
    if (r != 0) { fprintf(stderr, "FAIL: geof_write %d\n", r); return 1; }
    fprintf(stderr, "[geoframe] wrote '%s'\n", outpath);

    if (do_verify) {
        fprintf(stderr, "\n[verify] reading back...\n");
        GeoFHeader hdr;
        uint8_t *bmap;
        uint64_t *offsets;
        uint32_t loaded_n;
        r = geof_load(outpath, &hdr, &bmap, &offsets, &loaded_n);
        if (r != 0) { fprintf(stderr, "  \xe2\x9d\x8c geof_load: %d\n", r); return 1; }
        fprintf(stderr, "[verify] %u unique addrs from %u tensors\n", loaded_n, n);

        int verified = 0, errors = 0;
        FILE *vf = fopen(outpath, "rb");
        for (uint32_t i = 0; vf && i < n; i++) {
            uint8_t *buf = (uint8_t *)malloc(sizes[i] ? sizes[i] : 1);
            GeoFFrame fr;
            r = geof_read_frame(vf, bmap, offsets, addrs[i], loaded_n, buf, sizes[i], &fr);
            if (r == -5) {
                fprintf(stderr, "  \xe2\x9d\x8c [%4u] size cap %u mismatch\n", i, sizes[i]);
                errors++;
            } else if (r != 0) {
                fprintf(stderr, "  \xe2\x9d\x8c [%4u] addr=%u seek err=%d\n", i, addrs[i], r);
                errors++;
            } else if (fr.size != sizes[i]) {
                fprintf(stderr, "  \xe2\x9d\x8c [%4u] size: got %u exp %u\n", i, fr.size, sizes[i]);
                errors++;
            } else if (memcmp(buf, datas[i], sizes[i]) != 0) {
                fprintf(stderr, "  \xe2\x9d\x8c [%4u] addr=%u data mismatch\n", i, addrs[i]);
                errors++;
            } else {
                verified++;
            }
            free(buf);
        }
        if (vf) fclose(vf);
        geof_free(bmap, offsets);
        fprintf(stderr, "[verify] %d/%d PASS, %d FAIL%s\n",
                verified, n, errors, errors > 0 ? " (collisions are expected)" : "");

        /* benchmark seek */
        {
            LARGE_INTEGER freq, t0, t1;
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&t0);
            int64_t reps = 1000000;
            GeoFHeader hdr2;
            uint8_t *bmap2;
            uint64_t *offsets2;
            uint32_t n2;
            if (geof_load(outpath, &hdr2, &bmap2, &offsets2, &n2) == 0) {
                for (int64_t k = 0; k < reps; k++) {
                    uint32_t a = addrs[k % n];
                    uint32_t idx;
                    uint64_t file_off;
                    geof_seek(bmap2, offsets2, a, n2, &idx, &file_off);
                }
                QueryPerformanceCounter(&t1);
                double total_s = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
                fprintf(stderr, "[bench] geof_seek: %.0f ns/op (%I64d random)\n",
                        total_s * 1e9 / reps, (int64_t)reps);
                geof_free(bmap2, offsets2);
            }
        }

        if (errors > 0) return 1;
    }

    for (uint32_t i = 0; i < n; i++) free(datas[i]);
    free(addrs); free(sizes); free(datas);
    gguf_close(&ggr);
    fprintf(stderr, "[done] all ok\n");
    return 0;
}
