#include "pogls_v3_geoframe.h"
#include "pogls_v3_framestore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2 || argc > 4) {
        fprintf(stderr, "Usage: framestore_to_geoframe INPUT.framestore [OUTPUT.geo]\n");
        return 1;
    }

    const char *inpath = argv[1];
    char outpath[1024];
    if (argc >= 3) {
        strncpy(outpath, argv[2], sizeof(outpath) - 1);
    } else {
        size_t len = strlen(inpath);
        if (len > 11 && strcmp(inpath + len - 11, ".framestore") == 0) {
            memcpy(outpath, inpath, len - 11);
            memcpy(outpath + len - 11, ".geo", 5);
        } else {
            snprintf(outpath, sizeof(outpath), "%s.geo", inpath);
        }
    }

    /* read framestore header */
    FrameStoreHeader fsh;
    int r = framestore_read_header(inpath, &fsh);
    if (r != 0) {
        fprintf(stderr, "ERROR: cannot read framestore '%s'\n", inpath);
        return 1;
    }

    uint32_t n = fsh.n_tensors;
    fprintf(stderr, "[framestore] %s: %u tensors\n", inpath, n);

    /* read framestore entries */
    size_t entries_sz = n * sizeof(FrameStoreEntry);
    FrameStoreEntry *entries = (FrameStoreEntry *)malloc(entries_sz);
    if (!entries) { fprintf(stderr, "ERROR: OOM\n"); return 1; }

    FILE *f = fopen(inpath, "rb");
    if (!f) { free(entries); fprintf(stderr, "ERROR: open\n"); return 1; }
    fseek(f, fsh.addr_map_off, SEEK_SET);
    if (fread(entries, sizeof(FrameStoreEntry), n, f) != n) {
        fclose(f); free(entries); fprintf(stderr, "ERROR: read entries\n"); return 1;
    }

    /* read tensor data */
    uint32_t *addrs = (uint32_t *)malloc(n * sizeof(uint32_t));
    uint32_t *sizes = (uint32_t *)malloc(n * sizeof(uint32_t));
    uint8_t **datas = (uint8_t **)malloc(n * sizeof(uint8_t *));
    if (!addrs || !sizes || !datas) {
        fclose(f); free(entries); free(addrs); free(sizes); free(datas);
        fprintf(stderr, "ERROR: OOM\n"); return 1;
    }

    uint64_t total_data = 0;
    for (uint32_t i = 0; i < n; i++) {
        addrs[i] = entries[i].addr;
        sizes[i] = entries[i].nbytes;
        fprintf(stderr, "  [%3u] addr=%5u  size=%u\n", i, entries[i].addr, entries[i].nbytes);
        datas[i] = (uint8_t *)malloc(sizes[i] ? sizes[i] : 1);
        if (!datas[i]) {
            for (uint32_t j = 0; j < i; j++) free(datas[j]);
            fclose(f); free(entries); free(addrs); free(sizes); free(datas);
            fprintf(stderr, "ERROR: OOM\n"); return 1;
        }
        if (sizes[i] > 0) {
            fseek(f, (long)entries[i].file_offset, SEEK_SET);
            if (fread(datas[i], sizes[i], 1, f) != 1) {
                for (uint32_t j = 0; j <= i; j++) free(datas[j]);
                fclose(f); free(entries); free(addrs); free(sizes); free(datas);
                fprintf(stderr, "ERROR: read data[%u] at offset %lu\n", i, (unsigned long)entries[i].file_offset);
                return 1;
            }
            total_data += sizes[i];
        }
    }
    fclose(f);
    free(entries);

    fprintf(stderr, "[framestore] total tensor data: %lu bytes\n", (unsigned long)total_data);

    /* sort by address */
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = i + 1; j < n; j++) {
            if (addrs[i] > addrs[j]) {
                uint32_t ta = addrs[i]; addrs[i] = addrs[j]; addrs[j] = ta;
                uint32_t ts = sizes[i]; sizes[i] = sizes[j]; sizes[j] = ts;
                uint8_t *td = datas[i]; datas[i] = datas[j]; datas[j] = td;
            }
        }
    }

    /* write geoframe */
    r = geof_write(outpath, addrs, (const uint8_t *const *)datas, sizes, n);
    if (r != 0) {
        fprintf(stderr, "ERROR: geof_write returned %d\n", r);
        for (uint32_t i = 0; i < n; i++) free(datas[i]);
        free(addrs); free(sizes); free(datas);
        return 1;
    }

    fprintf(stderr, "[geoframe] wrote '%s': %u tensors, %lu data bytes\n",
            outpath, n, (unsigned long)total_data);

    /* verify roundtrip */
    GeoFHeader hdr;
    uint8_t *bmap;
    uint32_t *offsets;
    uint32_t loaded_n;
    r = geof_load(outpath, &hdr, &bmap, &offsets, &loaded_n);
    if (r != 0) { fprintf(stderr, "ERROR: geof_load\n"); return 1; }

    int ok = 1;
    FILE *vf = fopen(outpath, "rb");
    if (!vf) { ok = 0; }
    for (uint32_t i = 0; i < n && ok; i++) {
        uint8_t *buf = (uint8_t *)malloc(sizes[i] ? sizes[i] : 1);
        GeoFFrame fr;
        r = geof_read_frame(vf, bmap, offsets, addrs[i], loaded_n, buf, sizes[i], &fr);
        if (r != 0 || fr.size != sizes[i] || memcmp(buf, datas[i], sizes[i]) != 0) {
            fprintf(stderr, "  ❌ verify addr=%u size=%u\n", addrs[i], sizes[i]);
            ok = 0;
        }
        free(buf);
    }
    if (vf) fclose(vf);
    geof_free(bmap, offsets);

    if (ok) fprintf(stderr, "[geoframe] verify: ALL %u frames PASS\n", n);
    else    fprintf(stderr, "[geoframe] verify: FAILED\n");

    for (uint32_t i = 0; i < n; i++) free(datas[i]);
    free(addrs); free(sizes); free(datas);
    return ok ? 0 : 1;
}
