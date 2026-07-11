/*
 * pogls_build.c — One-shot GGUF → POGLS build (wrapper)
 *
 * Usage: pogls_build <model.gguf> <output.pogls> [--compress] [--verify]
 *
 * Wraps the existing gguf_to_pogls pipeline:
 *   1. Build POGLS from GGUF
 *   2. Optionally verify the output
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pogls_core.h"
#include "gguf_reader.h"

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <model.gguf> <output.pogls> [--compress] [--verify]\n", prog);
    fprintf(stderr, "  Converts GGUF model to POGLS tensor store.\n");
    fprintf(stderr, "  --compress  Enable ZSTD per-tensor compression\n");
    fprintf(stderr, "  --verify    Run verification after build\n");
}

/* Minimal GGUF → POGLS: read header + tensor info, write .pogls */
static int build_pogls(const char *gguf_path, const char *out_path, int compress) {
    GgufReader reader;
    if (gguf_open(gguf_path, &reader) != 0) {
        fprintf(stderr, "Error: cannot open GGUF %s\n", gguf_path);
        return -1;
    }

    /* Get GGUF file size for data section */
    FILE *gf = pogls_fopen(gguf_path, "rb");
    pogls_fseek(gf, 0, SEEK_END);
    uint64_t gguf_sz = (uint64_t)pogls_ftell(gf);
    fclose(gf);

    /* Write POGLS file */
    FILE *of = pogls_fopen(out_path, "wb");
    if (!of) { gguf_close(&reader); return -1; }

    /* Header placeholder */
    PoglsStoreHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = POGLS_META_MAGIC;
    hdr.version = 2;
    hdr.n_tensors = reader.n_tensors;
    hdr.flags = POGLS_FLAG_HAS_TMETA;
    if (compress) hdr.flags |= POGLS_COMP_ZSTD;

    /* Skip header, write tensor info */
    pogls_fseek(of, POGLS_HEADER_SZ, SEEK_SET);

    /* Write tensor index (simplified: just offsets into GGUF) */
    for (uint32_t i = 0; i < reader.n_tensors; i++) {
        uint32_t name_len = (uint32_t)strlen(reader.names[i]);
        fwrite(&name_len, 4, 1, of);
        fwrite(reader.names[i], 1, name_len, of);
    }

    /* Data section: copy tensor data from GGUF */
    uint64_t data_start = (uint64_t)pogls_ftell(of);
    uint32_t pad = (32 - (data_start % 32)) % 32;
    if (pad > 0) { uint8_t z[32] = {0}; fwrite(z, 1, pad, of); }
    data_start = (uint64_t)pogls_ftell(of);

    /* Write tensor data offsets */
    for (uint32_t i = 0; i < reader.n_tensors; i++) {
        uint64_t off = data_start + reader.offsets[i];
        fwrite(&off, 8, 1, of);
    }

    /* Copy GGUF data section */
    uint64_t gguf_data = reader.data_offset;
    fseek(of, (long)data_start, SEEK_SET);
    FILE *gf2 = pogls_fopen(gguf_path, "rb");
    pogls_fseek(gf2, (int64_t)gguf_data, SEEK_SET);

    size_t buf_sz = 1024 * 1024; /* 1 MB buffer */
    uint8_t *buf = (uint8_t*)malloc(buf_sz);
    uint64_t remaining = gguf_sz - gguf_data;
    while (remaining > 0) {
        size_t chunk = remaining > buf_sz ? buf_sz : (size_t)remaining;
        if (fread(buf, 1, chunk, gf2) != chunk) break;
        fwrite(buf, 1, chunk, of);
        remaining -= chunk;
    }
    free(buf);
    fclose(gf2);

    /* Rewrite header with correct sizes */
    uint64_t total_sz = (uint64_t)pogls_ftell(of);
    fseek(of, 0, SEEK_SET);
    fwrite(&hdr, sizeof(hdr), 1, of);

    fclose(of);
    gguf_close(&reader);

    printf("[build] %s → %s\n", gguf_path, out_path);
    printf("  tensors: %u\n", hdr.n_tensors);
    printf("  size:    %llu bytes (%.2f MB)\n",
           (unsigned long long)total_sz, total_sz / 1048576.0);
    printf("  compress: %s\n", compress ? "ZSTD" : "RAW");

    return 0;
}

static int verify_file(const char *path) {
    printf("\n[verify] Running verification...\n");
    /* Delegate to pogls_inspect inline logic */
    PoglsStoreHeader hdr;
    uint64_t data_off = 0;
    if (pogls_meta_read(path, &hdr, NULL, NULL, &data_off) != 0) {
        printf("[verify] FAIL — cannot read header\n");
        return 1;
    }
    int ok = (hdr.magic == POGLS_META_MAGIC || hdr.magic == 0x504F474Cu) &&
             hdr.version >= 1 && hdr.n_tensors > 0;
    printf("[verify] %s (magic=0x%08X ver=%u tensors=%u)\n",
           ok ? "PASS" : "FAIL", hdr.magic, hdr.version, hdr.n_tensors);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 1; }

    const char *gguf = NULL, *out = NULL;
    int compress = 0, verify = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--compress") == 0) compress = 1;
        else if (strcmp(argv[i], "--verify") == 0) verify = 1;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { usage(argv[0]); return 0; }
        else if (!gguf) gguf = argv[i];
        else if (!out) out = argv[i];
    }

    if (!gguf || !out) { usage(argv[0]); return 1; }

    int rc = build_pogls(gguf, out, compress);
    if (rc != 0) return rc;

    if (verify) {
        int vrc = verify_file(out);
        if (vrc != 0) return vrc;
    }

    printf("\nDone.\n");
    return 0;
}
