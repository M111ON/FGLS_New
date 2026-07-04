/*
 * gguf_to_pogls.c — Convert GGUF model → POGLS v2 tensor store (streaming)
 *
 * Reads GGUF directly (no llama DLL), writes .pogls v2 with metadata
 * and optional per-tensor zstd compression.  Streams through tensors one
 * at a time — O(total_raw) heap allocation eliminated.
 *
 * Supports --sid-faces N for SID multi-face perturbation (stores N copies
 * of each tensor at different capo addresses in the zero-copy POGLS address
 * space).
 *
 * Build:
 *   gcc -O2 -std=c11 -I. -I../collection -I../collection/Hfolder
 *       -DPOGLS_USE_ZSTD
 *       -o gguf_to_pogls.exe gguf_to_pogls.c zstd.dll -lm
 *
 * Usage:
 *   .\gguf_to_pogls.exe model.gguf output.pogls
 *   .\gguf_to_pogls.exe model.gguf output.pogls --compress
 *   .\gguf_to_pogls.exe model.gguf output.pogls --sid-faces 4 --compress
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zstd.h>

#include "pogls_store.h"
#include "pogls_meta.h"
#include "gguf_index.h"
#include "addr_space.h"

/* Perturb tensor data in-place for a given SID face.
 * Uses a sparse XOR pattern: XOR every stride-th byte with a
 * face-dependent key derived from tensor name hash + face index.
 * Stride = max(4, nbytes / 32) so each face gets ~32 modifications.
 */
static void sid_perturb(uint8_t *data, size_t nbytes, const char *name, int face_idx) {
    if (!data || nbytes == 0 || face_idx < 1) return;
    /* FNV-1a hash of name (same as addr_space.h uses internally) */
    uint32_t h = 0x811c9dc5u;
    for (const char *p = name; *p; p++) { h ^= (uint8_t)*p; h *= 0x01000193u; }
    uint8_t key = (uint8_t)((h >> (face_idx % 4) * 8) ^ (uint8_t)(face_idx * 37));
    size_t step = nbytes / 32;
    if (step < 4) step = 4;
    size_t off = (size_t)(face_idx - 1) % step;
    for (size_t j = off; j < nbytes; j += step)
        data[j] ^= key;
}

int main(int argc, char **argv) {
    int use_compress = 0;
    int opt_sid_faces = 0;
    int opt_rdh = 0;
    const char *model_path = NULL, *out_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--compress") == 0 || strcmp(argv[i], "-c") == 0)
            use_compress = 1;
        else if (strcmp(argv[i], "--sid-faces") == 0 && i + 1 < argc)
            opt_sid_faces = atoi(argv[++i]);
        else if (strcmp(argv[i], "--rdh") == 0)
            opt_rdh = 1;
        else if (!model_path) model_path = argv[i];
        else if (!out_path) out_path = argv[i];
    }
    if (!model_path || !out_path) {
        fprintf(stderr, "Usage: gguf_to_pogls.exe model.gguf output.pogls [--compress] [--sid-faces N] [--rdh]\n");
        return 1;
    }
    if (opt_sid_faces < 0) opt_sid_faces = 0;
    if (opt_sid_faces > 12) opt_sid_faces = 12;
    if (opt_sid_faces > 0)
        fprintf(stderr, "[sid] multi-face: %d faces (face 0 = original, 1..%d = perturbed)\n",
                opt_sid_faces, opt_sid_faces - 1);

    GGUFTensorIndex idx;
    memset(&idx, 0, sizeof(idx));
    if (gguf_idx_open(model_path, &idx) != 0) { fprintf(stderr, "ERROR: can't open %s\n", model_path); return 1; }
    fprintf(stderr, "[pogls] GGUF has %llu tensors\n", (unsigned long long)idx.n_tensors);

    /* ── Read GGUF metadata blob (header + KV + tensor infos, no weights) ── */
    uint8_t *meta_blob = NULL;
    uint64_t meta_blob_sz = 0;
    if (gguf_idx_read_meta_blob(model_path, &meta_blob, &meta_blob_sz) != 0) {
        fprintf(stderr, "WARNING: can't read GGUF meta blob (non-fatal)\n");
    } else {
        fprintf(stderr, "[pogls] GGUF meta blob: %llu bytes\n", (unsigned long long)meta_blob_sz);
    }

    FILE *f = fopen(model_path, "rb");
    if (!f) { gguf_idx_close(&idx); free(meta_blob); return 1; }

    /* Stream: process one tensor at a time — no O(total_raw) alloc */
    uint64_t total_raw = 0;
    for (uint64_t i = 0; i < idx.n_tensors; i++) total_raw += idx.sizes[i];

    /* First pass: count valid tensors (including SID face variants) */
    uint32_t total_valid = 0;
    for (uint64_t i = 0; i < idx.n_tensors; i++) {
        uint32_t addr = opt_rdh ? addr_from_rdh_name(idx.names[i], 0) : addr_from_tensor_name(idx.names[i], 0);
        if (addr < ADDR_BASE) total_valid++;
    }
    uint32_t n_meta = total_valid;
    if (opt_sid_faces > 1)
        n_meta = total_valid + total_valid * (uint32_t)(opt_sid_faces - 1);
    fprintf(stderr, "[pogls] valid tensors = %u, meta entries = %u (faces=%d)\n",
            total_valid, n_meta, opt_sid_faces > 0 ? opt_sid_faces : 1);

    /* Allocate descriptor array + per-tensor scratch buffers */
    PoglsTensorMeta *meta_arr = (PoglsTensorMeta*)calloc(n_meta, sizeof(PoglsTensorMeta));
    if (!meta_arr) { fprintf(stderr, "ERROR: meta OOM\n"); fclose(f); gguf_idx_close(&idx); return 1; }

    size_t max_sz = 0;
    for (uint64_t i = 0; i < idx.n_tensors; i++)
        if (idx.sizes[i] > max_sz) max_sz = idx.sizes[i];
    uint8_t *tbuf = (uint8_t*)malloc(max_sz);
    uint8_t *cbuf = (uint8_t*)malloc(ZSTD_compressBound(max_sz));
    if (!tbuf || !cbuf) { fprintf(stderr, "ERROR: scratch OOM\n"); free(tbuf); free(cbuf); free(meta_arr); fclose(f); gguf_idx_close(&idx); return 1; }

    /* Phase 1: stream-read & compress each tensor, write meta */
    fprintf(stderr, "[pogls] streaming %llu bytes from GGUF%s...\n",
            (unsigned long long)total_raw, use_compress ? " (compress)" : "");

    /* We write as we go: meta first (we know count), then data */
    /* Strategy: write placeholder header+index, then stream meta+data, then seek back to fix header */

    /* Open output file for streaming writes */
    FILE *out = fopen(out_path, "wb");
    if (!out) { fprintf(stderr, "ERROR: can't write %s\n", out_path); goto cleanup; }

    /* Write placeholder header + zero index */
    PoglsStoreHeader hdr;
    pogls_meta_header_init(&hdr);
    hdr.n_tensors          = n_meta;
    hdr.flags              = POGLS_FLAG_HAS_TMETA;
    hdr.tensor_meta_off    = sizeof(hdr) + POGLS_INDEX_SZ;
    hdr.tensor_meta_count  = n_meta;

    /* Model metadata (GGUF blob) — place after tensor meta, before data */
    uint64_t meta_end = hdr.tensor_meta_off + (uint64_t)n_meta * sizeof(PoglsTensorMeta);
    if (meta_blob && meta_blob_sz > 0) {
        hdr.model_meta_off = meta_end;
        hdr.model_meta_sz  = (uint32_t)meta_blob_sz;
        hdr.flags |= POGLS_FLAG_HAS_MMETA;
        meta_end += meta_blob_sz;
    }

    /* GGUF path — store original GGUF path after model meta */
    {
        hdr.gguf_path_off = meta_end;
        hdr.gguf_path_sz  = (uint32_t)(strlen(model_path) + 1); /* include null */
        meta_end += hdr.gguf_path_sz;
    }

    fwrite(&hdr, sizeof(hdr), 1, out);
    {
        uint8_t zero[4096] = {0};
        uint64_t rem = POGLS_INDEX_SZ;
        while (rem > 0) {
            uint64_t chunk = rem < sizeof(zero) ? rem : sizeof(zero);
            fwrite(zero, chunk, 1, out);
            rem -= chunk;
        }
    }

    /* Now stream meta + data: we'll collect meta in array, write data immediately */
    uint64_t data_start = meta_end;  /* after header + index + tensor meta + model meta */
    uint32_t mi = 0;
    uint64_t data_pos = data_start;

    /* Seek to data_start — data is written between model_meta and end-of-file */
    fseek(out, (long)data_start, SEEK_SET);

    for (uint64_t gi = 0; gi < idx.n_tensors; gi++) {
        uint32_t addr = opt_rdh ? addr_from_rdh_name(idx.names[gi], 0) : addr_from_tensor_name(idx.names[gi], 0);
        if (addr >= ADDR_BASE) continue;

        size_t sz = idx.sizes[gi];
        uint64_t abs_off = gguf_idx_tensor_abs_offset(&idx, gi);
        fseeko(f, (off_t)abs_off, SEEK_SET);
        if (fread(tbuf, sz, 1, f) != 1) continue;

        PoglsTensorMeta *m = &meta_arr[mi];
        m->addr          = addr;
        m->dtype         = idx.dtypes[gi];
        m->ndim          = 2;
        m->nbytes_orig   = (uint32_t)sz;
        m->dims[0]       = (uint32_t)(sz > 0 ? 1 : 0);
        m->dims[1]       = (uint32_t)sz;
        snprintf(m->name, sizeof(m->name), "%s", idx.names[gi]);

        if (use_compress) {
            pogls_compress_tensor(cbuf, ZSTD_compressBound(sz), tbuf, sz, m);
        } else {
            m->comp_type   = POGLS_COMP_RAW;
            m->comp_nbytes = (uint32_t)sz;
        }

        /* Write face 0 data immediately */
        uint8_t *src = (m->comp_type == POGLS_COMP_RAW) ? tbuf : cbuf;
        fwrite(src, m->comp_nbytes, 1, out);
        mi++;

        /* ── Write SID face variants (faces 1..opt_sid_faces-1) ── */
        for (int f = 1; f < opt_sid_faces; f++) {
            uint32_t face_addr = opt_rdh ? addr_rdh_capo(addr, (uint32_t)f, 0) : addr_capo(addr, (uint32_t)f, 0);
            if (face_addr >= ADDR_BASE) continue;

            /* Copy original data to cbuf workspace, then perturb in-place */
            if (sz > ZSTD_compressBound(max_sz)) continue;
            memcpy(cbuf, tbuf, sz);
            sid_perturb(cbuf, sz, idx.names[gi], f);

            PoglsTensorMeta *fm = &meta_arr[mi];
            fm->addr          = face_addr;
            fm->dtype         = idx.dtypes[gi];
            fm->ndim          = 2;
            fm->nbytes_orig   = (uint32_t)sz;
            fm->dims[0]       = (uint32_t)(sz > 0 ? 1 : 0);
            fm->dims[1]       = (uint32_t)sz;
            snprintf(fm->name, sizeof(fm->name), "%s", idx.names[gi]);

            if (use_compress) {
                /* compress perturbed data → tbuf output buffer */
                uint32_t csz = pogls_compress_tensor(tbuf, ZSTD_compressBound(sz), cbuf, sz, fm);
                if (csz == 0) {
                    fm->comp_type   = POGLS_COMP_RAW;
                    fm->comp_nbytes = (uint32_t)sz;
                }
                uint8_t *fdata = (fm->comp_type == POGLS_COMP_RAW) ? cbuf : tbuf;
                fwrite(fdata, fm->comp_nbytes, 1, out);
            } else {
                fm->comp_type   = POGLS_COMP_RAW;
                fm->comp_nbytes = (uint32_t)sz;
                fwrite(cbuf, sz, 1, out);
            }
            mi++;
        }
    }

    /* Seek back and write the meta array + model meta + gguf path between index and data */
    fseek(out, (long)(sizeof(hdr) + POGLS_INDEX_SZ), SEEK_SET);
    fwrite(meta_arr, sizeof(PoglsTensorMeta), n_meta, out);
    if (meta_blob && meta_blob_sz > 0) {
        fwrite(meta_blob, meta_blob_sz, 1, out);
    }
    if (hdr.gguf_path_off > 0 && hdr.gguf_path_sz > 0) {
        fwrite(model_path, hdr.gguf_path_sz, 1, out);
    }

    /* Update header with actual data offset */
    fseek(out, 0, SEEK_SET);
    hdr.tensor_meta_count = n_meta;
    fwrite(&hdr, sizeof(hdr), 1, out);

    fclose(out);
    fclose(f);

    uint64_t file_sz = (uint64_t)ftell(out) > 0 ? 0 : data_pos + (total_raw - (total_raw - data_pos));
    /* Get final file size by seeking to end */
    out = fopen(out_path, "rb");
    if (out) { fseek(out, 0, SEEK_END); file_sz = ftell(out); fclose(out); }

    uint64_t total_uncompressed = total_raw;
    if (opt_sid_faces > 1)
        total_uncompressed = total_raw * (uint64_t)opt_sid_faces;
    double ratio = total_uncompressed > 0 ? (double)total_uncompressed / (double)(file_sz - data_start + 0.1) : 1.0;
    fprintf(stderr, "[pogls] wrote %llu bytes (%.1f MB), ratio=%.2f",
            (unsigned long long)file_sz, file_sz / (1024.*1024), ratio);
    if (opt_sid_faces > 1) {
        fprintf(stderr, " (%u faces)\n", opt_sid_faces);
    } else {
        fprintf(stderr, "\n");
    }
    if (use_compress) {
        uint64_t saved = total_uncompressed - (file_sz - data_start);
        fprintf(stderr, "[pogls] compression saved %llu MB (%.1f%%)\n",
                (unsigned long long)(saved >> 20), 100.0 * (1.0 - (double)(file_sz - data_start) / (double)total_uncompressed));
    }

cleanup:
    free(tbuf);
    free(cbuf);
    free(meta_arr);
    free(meta_blob);
    gguf_idx_close(&idx);
    return 0;
}
