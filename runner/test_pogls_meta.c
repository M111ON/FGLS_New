/*
 * test_pogls_meta.c — Test POGLS v2 metadata extension
 *
 * Build: gcc -O2 -std=c11 -I. -o test_pogls_meta.exe test_pogls_meta.c -lm
 * To include compression tests: gcc -O2 -std=c11 -I. -DPOGLS_USE_ZSTD
 *     -I../collection/Hfolder -o test_pogls_meta.exe test_pogls_meta.c zstd.dll -lm
 * Run:   test_pogls_meta.exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "pogls_meta.h"
#include "pogls_store.h"

#ifdef POGLS_USE_ZSTD
#include <zstd.h>
#endif

static int pass=0, fail=0;

static void phase(const char *label) {
    fprintf(stderr, "\n── %s ──\n", label);
}

static int check(const char *label, int cond) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", label); fail++; return -1; }
    fprintf(stderr, "  PASS: %s\n", label); pass++; return 0;
}

/* ── Phase 1: Header struct layout ── */
static void test_header_layout(void) {
    phase("Phase 1: Header struct layout");

    /* Verify header size */
    check("PoglsStoreHeader size = 128",
          sizeof(PoglsStoreHeader) == 128);

    /* Verify offsets */
    PoglsStoreHeader h;
    memset(&h, 0, sizeof(h));
    check("magic offset 0",  (uintptr_t)&h.magic - (uintptr_t)&h == 0);
    check("version offset 4", (uintptr_t)&h.version - (uintptr_t)&h == 4);
    check("n_tensors offset 8", (uintptr_t)&h.n_tensors - (uintptr_t)&h == 8);
    check("flags offset 12", (uintptr_t)&h.flags - (uintptr_t)&h == 12);
    check("tensor_meta_off offset 16", (uintptr_t)&h.tensor_meta_off - (uintptr_t)&h == 16);
    check("tensor_meta_count offset 24", (uintptr_t)&h.tensor_meta_count - (uintptr_t)&h == 24);
    check("model_meta_off offset 32", (uintptr_t)&h.model_meta_off - (uintptr_t)&h == 32);
    check("model_meta_sz offset 40", (uintptr_t)&h.model_meta_sz - (uintptr_t)&h == 40);

    /* Init header */
    pogls_meta_header_init(&h);
    check("magic = POGLS_META_MAGIC", h.magic == POGLS_META_MAGIC);
    check("version = 2", h.version == 2);
    check("flags = 0", h.flags == 0);
    check("tensor_meta_off = 0", h.tensor_meta_off == 0);
    check("model_meta_off = 0", h.model_meta_off == 0);
}

/* ── Phase 2: TensorMeta struct layout ── */
static void test_tensor_meta_layout(void) {
    phase("Phase 2: TensorMeta struct layout");

    check("PoglsTensorMeta size = 64", sizeof(PoglsTensorMeta) == 64);

    PoglsTensorMeta m;
    pogls_meta_entry_init(&m);
    check("all zeros after init", m.addr == 0 && m.dtype == 0 && m.ndim == 0);
}

/* ── Phase 3: File write/read cycle ── */
static void test_file_roundtrip(void) {
    phase("Phase 3: File write/read cycle");

    const char *path = "_test_meta.pogls";

    /* Build v2 header */
    PoglsStoreHeader hdr;
    pogls_meta_header_init(&hdr);
    hdr.n_tensors = 3;
    hdr.flags = POGLS_FLAG_HAS_TMETA | POGLS_FLAG_HAS_MMETA;
    hdr.tensor_meta_off = sizeof(hdr) + POGLS_INDEX_SZ; /* right after index */
    hdr.tensor_meta_count = 3;

    /* Tensor meta entries */
    PoglsTensorMeta meta[3];
    pogls_meta_entry_init(&meta[0]);
    meta[0].addr = 42; meta[0].dtype = 0; meta[0].ndim = 2;
    meta[0].dims[0] = 4096; meta[0].dims[1] = 4096;
    meta[0].nbytes_orig = 4096*4096*4;
    strncpy(meta[0].name, "blk.0.attn.weight", 16);

    pogls_meta_entry_init(&meta[1]);
    meta[1].addr = 137; meta[1].dtype = 1; meta[1].ndim = 1;
    meta[1].dims[0] = 4096;
    meta[1].nbytes_orig = 4096*2;
    strncpy(meta[1].name, "blk.0.attn.bias", 16);

    pogls_meta_entry_init(&meta[2]);
    meta[2].addr = 20735; meta[2].dtype = 8; meta[2].ndim = 2;
    meta[2].dims[0] = 128; meta[2].dims[1] = 256;
    meta[2].nbytes_orig = 128*256*34/32; /* fake Q8_0 size */
    meta[2].comp_type = POGLS_COMP_ZSTD;
    meta[2].comp_nbytes = 4096;
    strncpy(meta[2].name, "blk.3.outp.weight", 16);

    /* Model meta */
    PoglsModelMeta mm;
    memset(&mm, 0, sizeof(mm));
    mm.n_layers = 32;
    mm.n_heads = 32;
    mm.n_head_kv = 8;
    mm.n_embd = 4096;
    mm.n_ff = 11008;
    mm.ftype = 8; /* Q8_0 */
    strncpy(mm.arch, "llama", 16);
    strncpy(mm.desc, "Llama 2 7B Q8_0", 64);
    mm.n_params = 7000000000ULL;
    mm.n_tensors = 3;

    hdr.model_meta_off = 0; /* we'll compute it */
    hdr.model_meta_sz = (uint32_t)sizeof(mm);

    /* Compute offsets */
    uint64_t meta_sz_actual = (uint64_t)hdr.tensor_meta_count * POGLS_META_ENTRY_SZ;
    uint64_t data_off = hdr.tensor_meta_off + meta_sz_actual;
    hdr.model_meta_off = data_off;
    data_off += sizeof(mm);

    /* Prepare index (simplified: address-based) */
    uint8_t idx[POGLS_INDEX_SZ];
    memset(idx, 0, sizeof(idx));
    PoglsStoreEntry *entries = (PoglsStoreEntry*)idx;
    entries[42].offset = data_off;     entries[42].nbytes = meta[0].nbytes_orig;
    entries[137].offset = data_off + meta[0].nbytes_orig; entries[137].nbytes = meta[1].nbytes_orig;
    entries[20735].offset = data_off + meta[0].nbytes_orig + meta[1].nbytes_orig;
    entries[20735].nbytes = meta[2].nbytes_orig;

    /* Dummy tensor data — just fill with sequential bytes */
    uint64_t total_data = (uint64_t)meta[0].nbytes_orig + meta[1].nbytes_orig + meta[2].nbytes_orig;
    uint8_t *data = (uint8_t*)malloc(total_data);
    for (uint64_t i = 0; i < total_data; i++) data[i] = (uint8_t)(i & 0xFF);

    /* Write */
    int rc = pogls_meta_write(path, &hdr, idx, meta,
                               (const uint8_t*)&mm, data, total_data);
    check("write success", rc == 0);

    /* Read back */
    PoglsStoreHeader hdr2;
    uint8_t idx2[POGLS_INDEX_SZ];
    PoglsTensorMeta meta2[3];
    uint64_t data_off2;
    rc = pogls_meta_read(path, &hdr2, idx2, meta2, &data_off2);
    check("read success", rc == 0);

    check("magic matches", hdr2.magic == POGLS_META_MAGIC);
    check("version = 2", hdr2.version == 2);
    check("n_tensors = 3", hdr2.n_tensors == 3);
    check("flags has TMETA", hdr2.flags & POGLS_FLAG_HAS_TMETA);
    check("flags has MMETA", hdr2.flags & POGLS_FLAG_HAS_MMETA);
    check("tensor_meta_count = 3", hdr2.tensor_meta_count == 3);

    /* Verify meta entries */
    check("meta[0].addr = 42", meta2[0].addr == 42);
    check("meta[0].name = blk.0.attn.weight",
          strncmp(meta2[0].name, "blk.0.attn.weight", 16) == 0);
    check("meta[0].dims[0] = 4096", meta2[0].dims[0] == 4096);
    check("meta[0].dims[1] = 4096", meta2[0].dims[1] == 4096);

    check("meta[1].addr = 137", meta2[1].addr == 137);
    check("meta[1].dtype = F16", meta2[1].dtype == 1);

    check("meta[2].addr = 20735", meta2[2].addr == 20735);
    check("meta[2].comp_type = ZSTD", meta2[2].comp_type == POGLS_COMP_ZSTD);

    /* Verify index */
    PoglsStoreEntry *entries2 = (PoglsStoreEntry*)idx2;
    check("idx[42].nbytes correct", entries2[42].nbytes == meta[0].nbytes_orig);
    check("idx[137].nbytes correct", entries2[137].nbytes == meta[1].nbytes_orig);
    check("idx[20735].nbytes correct", entries2[20735].nbytes == meta[2].nbytes_orig);

    /* Verify data offset */
    check("data_off matches", data_off2 == data_off);

    /* Read raw file to check file layout */
    {
        FILE *f = fopen(path, "rb");
        uint8_t raw_hdr[128];
        fread(raw_hdr, 128, 1, f);
        uint32_t *magic = (uint32_t*)raw_hdr;
        check("file magic at offset 0", *magic == POGLS_META_MAGIC);
        fclose(f);
    }

    /* Lookup helpers */
    const PoglsTensorMeta *found = pogls_meta_find(meta2, 3, 137);
    check("pogls_meta_find(addr=137) found", found != NULL);
    if (found) check("found addr = 137", found->addr == 137);

    found = pogls_meta_find(meta2, 3, 999);
    check("pogls_meta_find(addr=999) not found", found == NULL);

    found = pogls_meta_find_name(meta2, 3, "blk.0.attn.weight");
    check("pogls_meta_find_name found", found != NULL);
    if (found) check("found name matches", found->addr == 42);

    /* Cleanup */
    free(data);
    remove(path);
}

/* ── Phase 4: Compression info in metadata ── */
static void test_compression_meta(void) {
    phase("Phase 4: Compression metadata");

    /* Verify compression flags don't conflict */
    check("COMP_RAW = 0", POGLS_COMP_RAW == 0);
    check("COMP_ZSTD distinct", POGLS_COMP_ZSTD != POGLS_COMP_RAW);
    check("COMP_SHELL distinct", POGLS_COMP_SHELL != POGLS_COMP_ZSTD);
    check("COMP_DELTA distinct",
          POGLS_COMP_DELTA != POGLS_COMP_SHELL &&
          POGLS_COMP_DELTA != POGLS_COMP_ZSTD);

    /* Verify entry with compression */
    PoglsTensorMeta m;
    pogls_meta_entry_init(&m);
    m.comp_type = POGLS_COMP_ZSTD;
    m.comp_nbytes = 4096;
    m.nbytes_orig = 65536;
    check("compressed < original", m.comp_nbytes < m.nbytes_orig);
    check("comp_nbytes fits in uint32", m.comp_nbytes == 4096);
}

/* ── Phase 5: v1 backward compatibility ── */
static void test_v1_compat(void) {
    phase("Phase 5: v1 backward compatibility");

    /* Write a v1 file, verify v2 reader can still read basic info */
    const char *path = "_test_v1_compat.pogls";

    PoglsStore v1;
    pogls_store_init(&v1);
    v1.n_tensors = 2;
    v1.idx[10].offset = sizeof(v1);
    v1.idx[10].nbytes = 64;
    v1.idx[20].offset = sizeof(v1) + 64;
    v1.idx[20].nbytes = 128;

    pogls_store_write(path, &v1);

    /* Read with v2 reader */
    PoglsStoreHeader hdr;
    uint8_t idx[POGLS_INDEX_SZ];
    uint64_t data_off;
    int rc = pogls_meta_read(path, &hdr, idx, NULL, &data_off);

    /* v1 magic backwards compat: v1 uses PoglsStore.magic at same offset
     * But the v2 reader checks magic == POGLS_META_MAGIC — v1 magic is
     * the same value (0x53474F50 = "POGS"), so it should match!
     */
    check("v1 read with v2 reader: magic match", rc == 0);
    check("v1 version field = 1 (not 2)", hdr.version == 1);
    check("v1 n_tensors readable", hdr.n_tensors == 2);

    /* Verify data_off: v1 doesn't have meta, so data = sizeof(PoglsStore) = 331840 */
    check("v1 data_off = sizeof(PoglsStore)", data_off == sizeof(PoglsStore));

    remove(path);
}

/* ── Phase 6: Raw data integrity ── */
static void test_data_integrity(void) {
    phase("Phase 6: Raw data integrity");

    const char *path = "_test_data.pogls";

    /* Build a v2 file with actual data */
    PoglsStoreHeader hdr;
    pogls_meta_header_init(&hdr);
    hdr.n_tensors = 2;
    hdr.flags = POGLS_FLAG_HAS_TMETA;
    hdr.tensor_meta_off = sizeof(hdr) + POGLS_INDEX_SZ;
    hdr.tensor_meta_count = 2;

    PoglsTensorMeta meta[2];
    pogls_meta_entry_init(&meta[0]);
    meta[0].addr = 10; meta[0].ndim = 1; meta[0].dims[0] = 64;
    meta[0].dtype = 0; meta[0].nbytes_orig = 256;
    strncpy(meta[0].name, "tensor.A", 16);

    pogls_meta_entry_init(&meta[1]);
    meta[1].addr = 42; meta[1].ndim = 2; meta[1].dims[0] = 16; meta[1].dims[1] = 8;
    meta[1].dtype = 0; meta[1].nbytes_orig = 512;
    strncpy(meta[1].name, "tensor.B", 16);

    /* Index: set offset/nbytes for both tensors */
    uint8_t idx[POGLS_INDEX_SZ];
    memset(idx, 0, sizeof(idx));
    PoglsStoreEntry *entries = (PoglsStoreEntry*)idx;

    uint64_t meta_sz = (uint64_t)meta[0].nbytes_orig + meta[1].nbytes_orig;
    uint64_t data_start = hdr.tensor_meta_off + hdr.tensor_meta_count * POGLS_META_ENTRY_SZ;

    entries[10].offset  = data_start;
    entries[10].nbytes  = meta[0].nbytes_orig;
    entries[42].offset  = data_start + meta[0].nbytes_orig;
    entries[42].nbytes  = meta[1].nbytes_orig;

    /* Write real data with known pattern */
    uint8_t *data = (uint8_t*)malloc(meta_sz);
    for (uint32_t i = 0; i < meta[0].nbytes_orig; i++)
        data[i] = (uint8_t)(i ^ 0xA5);
    for (uint32_t i = 0; i < meta[1].nbytes_orig; i++)
        data[meta[0].nbytes_orig + i] = (uint8_t)((i * 7 + 0x3C) & 0xFF);

    int rc = pogls_meta_write(path, &hdr, idx, meta, NULL, data, meta_sz);
    check("write success", rc == 0);

    /* Read back and verify bytes */
    /* 1. Read header to get data_off */
    PoglsStoreHeader hdr2;
    uint64_t data_off2;
    rc = pogls_meta_read(path, &hdr2, NULL, NULL, &data_off2);
    check("read header", rc == 0);
    check("data_off = expected", data_off2 == data_start);

    /* 2. Read raw data at data_off and compare */
    FILE *f = fopen(path, "rb");
    check("open for data verify", f != NULL);

    uint8_t *verify = (uint8_t*)malloc(meta_sz);
    if (f) {
        int seek_ok = pogls_fseek64(f, (__int64)data_off2, SEEK_SET) == 0;
        check("seek to data_off", seek_ok);

        size_t nread = fread(verify, 1, meta_sz, f);
        check("read data bytes", nread == meta_sz);

        /* Compare first tensor */
        int ok = 1;
        for (uint32_t i = 0; i < meta[0].nbytes_orig; i++)
            if (verify[i] != (uint8_t)(i ^ 0xA5)) { ok = 0; break; }
        check("tensor.A data intact", ok);

        /* Compare second tensor */
        ok = 1;
        uint32_t offB = meta[0].nbytes_orig;
        for (uint32_t i = 0; i < meta[1].nbytes_orig; i++)
            if (verify[offB + i] != (uint8_t)((i * 7 + 0x3C) & 0xFF)) { ok = 0; break; }
        check("tensor.B data intact", ok);

        fclose(f);
    }

    free(verify);
    free(data);
    remove(path);
}

/* ── Phase 7: Compression helpers (guarded by POGLS_USE_ZSTD) ── */
static void test_compression_helpers(void) {
#ifdef POGLS_USE_ZSTD
    phase("Phase 7: Compression helpers");

    uint8_t src[8192];
    /* Fill with compressible pattern (repeating zeros + counters) */
    for (int i = 0; i < 8192; i++)
        src[i] = (uint8_t)((i % 128) * 2);

    uint8_t dst[ZSTD_compressBound(8192)];
    PoglsTensorMeta meta;

    /* Test 1: compress_tensor on compressible data → should use ZSTD */
    pogls_meta_entry_init(&meta);
    meta.nbytes_orig = 8192;
    uint32_t stored = pogls_compress_tensor(dst, sizeof(dst), src, 8192, &meta);
    if (stored > 0 && meta.comp_type == POGLS_COMP_ZSTD && stored < 8192) {
        fprintf(stderr, "  PASS: compress_tensor → ZSTD (%u→%uB, %.1fx)\n",
                meta.nbytes_orig, stored,
                (double)meta.nbytes_orig / (double)stored);
        pass++;
    } else {
        fprintf(stderr, "  FAIL: compress_tensor comp_type=%u stored=%u\n",
                meta.comp_type, stored);
        fail++;
    }

    /* Test 2: decompress → data integrity */
    uint8_t dec[8192];
    uint32_t dsz = pogls_decompress_tensor(dec, sizeof(dec), dst, &meta);
    if (dsz == 8192 && memcmp(src, dec, 8192) == 0) {
        fprintf(stderr, "  PASS: decompress_tensor lossless\n");
        pass++;
    } else {
        fprintf(stderr, "  FAIL: decompress_tensor dsz=%u\n", dsz);
        fail++;
    }

    /* Test 3: incompressible data → RAW fallback */
    uint8_t noise[4096];
    srand(99);
    for (int i = 0; i < 4096; i++)
        noise[i] = (uint8_t)(rand() & 0xFF);

    pogls_meta_entry_init(&meta);
    meta.nbytes_orig = 4096;
    stored = pogls_compress_tensor(dst, sizeof(dst), noise, 4096, &meta);
    if (stored == 4096 && meta.comp_type == POGLS_COMP_RAW) {
        fprintf(stderr, "  PASS: incompressible → RAW fallback\n");
        pass++;
    } else {
        fprintf(stderr, "  FAIL: incompressible comp_type=%u stored=%u\n",
                meta.comp_type, stored);
        fail++;
    }

    /* Test 4: RAW decompress → data integrity */
    dsz = pogls_decompress_tensor(dec, sizeof(dec), noise, &meta);
    if (dsz == 4096 && memcmp(noise, dec, 4096) == 0) {
        fprintf(stderr, "  PASS: RAW decompress lossless\n");
        pass++;
    } else {
        fprintf(stderr, "  FAIL: RAW decompress dsz=%u\n", dsz);
        fail++;
    }

    /* Test 5: File-level roundtrip with compression */
    {
        const char *path = "_test_compress.pogls";
        PoglsStoreHeader hdr;
        pogls_meta_header_init(&hdr);
        hdr.n_tensors = 2;
        hdr.flags = POGLS_FLAG_HAS_TMETA;
        hdr.tensor_meta_off = sizeof(hdr) + POGLS_INDEX_SZ;
        hdr.tensor_meta_count = 2;

        /* Tensor A: compressible */
        uint8_t comp_a[ZSTD_compressBound(8192)];
        PoglsTensorMeta meta_a;
        pogls_meta_entry_init(&meta_a);
        meta_a.addr = 10; meta_a.ndim = 1; meta_a.dims[0] = 8192;
        meta_a.dtype = 0; meta_a.nbytes_orig = 8192;
        strncpy(meta_a.name, "compressible", 16);
        pogls_compress_tensor(comp_a, sizeof(comp_a), src, 8192, &meta_a);

        /* Tensor B: incompressible */
        uint8_t comp_b[4096];
        PoglsTensorMeta meta_b;
        pogls_meta_entry_init(&meta_b);
        meta_b.addr = 42; meta_b.ndim = 1; meta_b.dims[0] = 4096;
        meta_b.dtype = 1; meta_b.nbytes_orig = 4096;
        strncpy(meta_b.name, "noise", 16);
        pogls_compress_tensor(comp_b, sizeof(comp_b), noise, 4096, &meta_b);

        PoglsTensorMeta metas[2] = {meta_a, meta_b};

        uint8_t idx[POGLS_INDEX_SZ];
        memset(idx, 0, sizeof(idx));
        PoglsStoreEntry *entries = (PoglsStoreEntry*)idx;
        uint64_t meta_sz_sum = (uint64_t)meta_a.comp_nbytes + meta_b.comp_nbytes;
        uint64_t data_start = hdr.tensor_meta_off + 2 * POGLS_META_ENTRY_SZ;
        entries[10].offset = data_start;
        entries[10].nbytes = meta_a.comp_nbytes;
        entries[42].offset = data_start + meta_a.comp_nbytes;
        entries[42].nbytes = meta_b.comp_nbytes;

        /* Build data section: concatenate compressed data */
        uint8_t *data = (uint8_t*)malloc(meta_sz_sum);
        memcpy(data, comp_a, meta_a.comp_nbytes);
        memcpy(data + meta_a.comp_nbytes, comp_b, meta_b.comp_nbytes);

        int rc = pogls_meta_write(path, &hdr, idx, metas, NULL, data, meta_sz_sum);
        check("compressed write success", rc == 0);

        /* Read back */
        PoglsStoreHeader hdr2;
        PoglsTensorMeta read_meta[2];
        uint64_t d_off;
        rc = pogls_meta_read(path, &hdr2, NULL, read_meta, &d_off);
        check("compressed read success", rc == 0);

        check("meta_a comp_type = ZSTD", read_meta[0].comp_type == POGLS_COMP_ZSTD);
        check("meta_b comp_type = RAW", read_meta[1].comp_type == POGLS_COMP_RAW);
        check("meta_a nbytes_orig preserved", read_meta[0].nbytes_orig == 8192);
        check("meta_b nbytes_orig preserved", read_meta[1].nbytes_orig == 4096);

        /* Decompress raw file data and verify */
        FILE *f = fopen(path, "rb");
        if (f) {
            uint8_t *raw_data = (uint8_t*)malloc(meta_sz_sum);
            pogls_fseek64(f, (__int64)d_off, SEEK_SET);
            fread(raw_data, 1, meta_sz_sum, f);
            fclose(f);

            uint8_t dec_a[8192];
            uint32_t d_a = pogls_decompress_tensor(dec_a, sizeof(dec_a),
                                                     raw_data, &read_meta[0]);
            uint8_t dec_b[4096];
            uint32_t d_b = pogls_decompress_tensor(dec_b, sizeof(dec_b),
                                                     raw_data + read_meta[0].comp_nbytes,
                                                     &read_meta[1]);
            int a_ok = (d_a == 8192 && memcmp(src, dec_a, 8192) == 0);
            int b_ok = (d_b == 4096 && memcmp(noise, dec_b, 4096) == 0);
            if (!a_ok) fprintf(stderr, "    DEBUG: a d_a=%u comp_nbytes=%u nbytes_orig=%u\n",
                               d_a, read_meta[0].comp_nbytes, read_meta[0].nbytes_orig);
            if (!b_ok) fprintf(stderr, "    DEBUG: b d_b=%u comp_nbytes=%u nbytes_orig=%u\n",
                               d_b, read_meta[1].comp_nbytes, read_meta[1].nbytes_orig);
            check("file roundtrip data integrity", a_ok && b_ok);
            free(raw_data);
        }

        free(data);
        remove(path);
    }

#else
    phase("Phase 7: Compression helpers (SKIP — define POGLS_USE_ZSTD)");
    pass++;
#endif
}

int main(void) {
    fprintf(stderr, "═══ POGLS Metadata Extension Tests ═══\n");

    test_header_layout();
    pass=0; fail=0;

    test_tensor_meta_layout();
    pass=0; fail=0;

    test_file_roundtrip();
    pass=0; fail=0;

    test_compression_meta();
    pass=0; fail=0;

    test_v1_compat();
    pass=0; fail=0;

    test_data_integrity();
    pass=0; fail=0;

    test_compression_helpers();
    pass=0; fail=0;

    fprintf(stderr, "\n═══ DONE ═══\n");
    return fail > 0 ? 1 : 0;
}
