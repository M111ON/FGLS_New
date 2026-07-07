#include "pogls_v3_geoframe.h"
#include "addr_space.h"
#include "gguf_reader.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: geof_verify MODEL.gguf MODEL.geo\n"); return 1; }
    const char *gguf_path = argv[1];
    const char *geo_path  = argv[2];

    GgufReader ggr;
    if (gguf_open(gguf_path, &ggr) != 0) { fprintf(stderr, "FAIL: gguf_open\n"); return 1; }

    GeoFHeader hdr;
    uint8_t *bmap;
    uint64_t *offsets;
    uint32_t n_uniq;
    if (geof_load(geo_path, &hdr, &bmap, &offsets, &n_uniq) != 0) { fprintf(stderr, "FAIL: geof_load\n"); return 1; }

    printf("GGUF: %u tensors, %u unique addrs\n", ggr.n_tensors, n_uniq);

    /* verify seek for all addresses from GGUF */
    int ok = 0, err = 0;
    uint8_t *buf = (uint8_t *)malloc(256*1024*1024); /* 256 MB buffer */
    if (!buf) { fprintf(stderr, "OOM\n"); return 1; }

    FILE *vf = fopen(geo_path, "rb");
    if (!vf) { fprintf(stderr, "FAIL: open\n"); return 1; }

    for (uint32_t i = 0; i < ggr.n_tensors; i++) {
        uint32_t addr = addr_from_tensor_name(ggr.names[i], 0);
        uint32_t sz = ggr.sizes[i];

        /* Try to seek and read frame */
        GeoFFrame fr;
        memset(buf, 0, sizeof(size_t));
        int r = geof_read_frame(vf, bmap, offsets, addr, n_uniq, buf, sz, &fr);

        if (r != 0) {
            if (r == -1) {
                err++;
                if (err <= 5) fprintf(stderr, "  MISS addr=%u '%s'\n", addr, ggr.names[i]);
            } else if (r == -5) {
                err++;
                if (err <= 5) {
                    /* Read frame raw to see what size it claims */
                    uint32_t idx;
                    uint64_t file_off;
                    if (geof_seek(bmap, offsets, addr, n_uniq, &idx, &file_off) == 0) {
                        GeoFFrame fr_raw;
                        _fseeki64(vf, (__int64)file_off, SEEK_SET);
                        fread(&fr_raw, sizeof(fr_raw), 1, vf);
                        fprintf(stderr, "  SZ[%5u] addr=%u fr_size=%u cap=%u '%s'\n",
                                idx, addr, fr_raw.size, sz, ggr.names[i]);
                    } else {
                        fprintf(stderr, "  SEEK_FAIL addr=%u '%s'\n", addr, ggr.names[i]);
                    }
                }
            } else {
                err++;
                if (err <= 5) fprintf(stderr, "  ERR[%d] addr=%u '%s'\n", r, addr, ggr.names[i]);
            }
            continue;
        }

        if (fr.size != sz) {
            err++;
            if (err <= 3) fprintf(stderr, "  SZ addr=%u fr=%u exp=%u '%s'\n", addr, fr.size, sz, ggr.names[i]);
            continue;
        }

        ok++;
        if (i < 5 || i >= ggr.n_tensors - 2)
            fprintf(stderr, "  OK [%4u] addr=%u size=%u '%s'\n", i, addr, fr.size, ggr.names[i]);
    }

    fprintf(stderr, "\nResult: %d OK, %d err\n", ok, err);
    if (err > 0) fprintf(stderr, "(errors expected: collisions cause addr overwrite)\n");

    /* data verify for first tensor only (large buffers) */
    if (ok > 0 && ggr.n_tensors > 0 && ggr.sizes[0] <= 256*1024*1024) {
        fprintf(stderr, "\nData integrity test for tensor[0] (%s)...\n", ggr.names[0]);
        uint32_t addr = addr_from_tensor_name(ggr.names[0], 0);
        uint32_t sz = ggr.sizes[0];
        uint8_t *gguf_data = (uint8_t *)malloc(sz);
        if (gguf_data) {
            if (gguf_read_tensor(gguf_path, &ggr, 0, gguf_data, sz) == 0) {
                GeoFFrame fr;
                memset(buf, 0, sz);
                int r = geof_read_frame(vf, bmap, offsets, addr, n_uniq, buf, sz, &fr);
                if (r == 0 && fr.size == sz) {
                    if (memcmp(buf, gguf_data, sz) == 0) {
                        fprintf(stderr, "  DATA MATCH ✓\n");
                    } else {
                        fprintf(stderr, "  DATA MISMATCH ✗\n");
                    }
                }
            }
            free(gguf_data);
        }
    }

    fclose(vf);
    geof_free(bmap, offsets);
    gguf_close(&ggr);
    free(buf);
    return err > 0 ? 1 : 0;
}
