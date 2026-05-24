/*
 * gpr1_container.h — GeoPixel Residual codec (v1)
 *
 * Deterministic frame-predictive sparse residual codec.
 * File layout:
 *
 *   [FILE_HEADER]     19B
 *     magic      4B   'GPR1'
 *     version    1B   0x01
 *     chunk_size 2B   max 64  (little-endian)
 *     n_chunks   2B   number of chunk records  (little-endian)
 *     total_len  8B   original data length     (little-endian)
 *
 *   [CHUNK_RECORDS]   variable, one per chunk
 *     slot_idx   4B   slot index for prediction  (little-endian)
 *     orig_len   4B   original chunk length      (little-endian)
 *     mode       2B   0=perfect 1=sparse 2=raw   (little-endian)
 *     pay_len    2B   payload byte length         (little-endian)
 *     payload    ...  mode-dependent data
 *
 * Modes:
 *   0 = perfect prediction — payload is empty (pay_len=0)
 *   1 = sparse residual   — 8-byte bitmask + XOR bytes for mismatched positions
 *   2 = raw fallback      — full chunk verbatim
 *
 * All multi-byte values are little-endian.
 */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Constants ──────────────────────────────────────── */
#define GPR1_MAGIC            0x31525047u   /* 'GPR1' little-endian */
#define GPR1_VERSION          0x01
#define GPR1_MAX_CHUNK        64
#define GPR1_HEADER_SZ        17   /* magic(4) + ver(1) + chunk_size(2) + n_chunks(2) + total_len(8) */
#define GPR1_RECORD_HDR_SZ    12   /* slot_idx(4) + orig_len(4) + mode(2) + pay_len(2) */

/* modes */
#define GPR1_MODE_PERFECT     0
#define GPR1_MODE_SPARSE      1
#define GPR1_MODE_RAW         2

/* prediction constants (same as geo_pixel.h) */
#define GPR1_TRIT_MOD         27
#define GPR1_SPOKE_MOD        6
#define GPR1_COSET_MOD        9
#define GPR1_LETTER_MOD       26
#define GPR1_FIBO_MOD         144


/* ── Internal: predict one chunk from slot geometry ──── */

static inline void gpr1_predict_chunk(
        uint32_t slot_idx, uint32_t length, uint8_t *out)
{
    for(uint32_t i = 0; i < length; i++){
        uint32_t idx_mod = (slot_idx + (i / 3)) % 27;
        uint8_t r = (uint8_t)(((idx_mod % GPR1_TRIT_MOD) << 3) | (idx_mod % GPR1_SPOKE_MOD));
        uint8_t g = (uint8_t)(((idx_mod % GPR1_COSET_MOD) << 4) | (idx_mod % GPR1_LETTER_MOD & 0xF));
        uint8_t b = (uint8_t)(idx_mod % GPR1_FIBO_MOD);
        out[i] = (i % 3 == 0) ? r : (i % 3 == 1) ? g : b;
    }
}


/* ── Internal: encode one chunk record ──────────────── */

static inline uint32_t gpr1_encode_record(
        uint32_t slot_idx, const uint8_t *chunk, uint32_t chunk_len,
        uint8_t *out, uint32_t out_cap)
{
    if(out_cap < GPR1_RECORD_HDR_SZ + chunk_len + 8) return 0;

    uint8_t pred[GPR1_MAX_CHUNK];
    gpr1_predict_chunk(slot_idx, chunk_len, pred);

    /* measure mismatches */
    uint8_t diffs[GPR1_MAX_CHUNK];
    uint32_t ndiff = 0;
    uint64_t mask = 0;
    for(uint32_t i = 0; i < chunk_len && i < 64; i++){
        if(chunk[i] != pred[i]){
            mask |= (1ULL << i);
            diffs[ndiff++] = chunk[i] ^ pred[i];
        }
    }

    uint8_t *p = out;
    uint32_t mode = 0, pay_len = 0;

    if(ndiff == 0){
        mode = GPR1_MODE_PERFECT;
        pay_len = 0;
    } else {
        uint32_t sparse_sz = 8 + ndiff;   /* mask(8) + xor bytes */
        if(sparse_sz < chunk_len){
            mode = GPR1_MODE_SPARSE;
            pay_len = sparse_sz;
            if(pay_len + GPR1_RECORD_HDR_SZ > out_cap) return 0;
        } else {
            mode = GPR1_MODE_RAW;
            pay_len = chunk_len;
            if(pay_len + GPR1_RECORD_HDR_SZ > out_cap) return 0;
        }
    }

    /* write record header */
    memcpy(p, &slot_idx, 4); p += 4;
    memcpy(p, &chunk_len, 4); p += 4;
    memcpy(p, &mode, 2);     p += 2;
    memcpy(p, &pay_len, 2);  p += 2;

    /* write payload */
    if(mode == GPR1_MODE_PERFECT){
        /* no payload */
    } else if(mode == GPR1_MODE_SPARSE){
        memcpy(p, &mask, 8); p += 8;
        memcpy(p, diffs, ndiff); p += ndiff;
    } else {
        memcpy(p, chunk, chunk_len); p += chunk_len;
    }

    return (uint32_t)(p - out);
}


/* ── Internal: decode one chunk record ──────────────── */

static inline int gpr1_decode_record(
        const uint8_t *in, uint32_t in_len, uint32_t *offset,
        uint8_t *out, uint32_t out_cap)
{
    if(*offset + GPR1_RECORD_HDR_SZ > in_len) return -1;

    uint32_t slot_idx = 0, orig_len = 0, mode = 0, pay_len = 0;
    memcpy(&slot_idx, in + *offset, 4);
    memcpy(&orig_len, in + *offset + 4, 4);
    memcpy(&mode,     in + *offset + 8, 2);
    memcpy(&pay_len,  in + *offset + 10, 2);
    *offset += GPR1_RECORD_HDR_SZ;

    if(orig_len > GPR1_MAX_CHUNK || orig_len > out_cap) return -1;
    if(*offset + pay_len > in_len) return -1;

    uint8_t pred[GPR1_MAX_CHUNK];
    gpr1_predict_chunk(slot_idx, orig_len, pred);

    if(mode == GPR1_MODE_PERFECT){
        memcpy(out, pred, orig_len);
    } else if(mode == GPR1_MODE_SPARSE){
        if(pay_len < 8) return -1;
        uint64_t mask;
        memcpy(&mask, in + *offset, 8);
        const uint8_t *vals = in + *offset + 8;
        uint32_t nvals = pay_len - 8;
        memcpy(out, pred, orig_len);
        uint32_t vi = 0;
        for(uint32_t i = 0; i < orig_len && i < 64; i++){
            if(mask & (1ULL << i)){
                if(vi >= nvals) return -1;
                out[i] ^= vals[vi++];
            }
        }
        if(vi != nvals) return -1;
    } else if(mode == GPR1_MODE_RAW){
        if(pay_len != orig_len) return -1;
        memcpy(out, in + *offset, orig_len);
    } else {
        return -1;
    }

    *offset += pay_len;
    return (int)orig_len;
}


/* ══════════════════════════════════════════════════════
 * HIGH-LEVEL API
 * ══════════════════════════════════════════════════════ */

/*
 * gpr1_encode_mem — encode data to GPR1 blob (caller frees *out)
 *   Returns blob size, or 0 on failure.
 */
static inline uint32_t gpr1_encode_mem(
        const uint8_t *data, uint32_t data_len,
        uint8_t chunk_size,
        uint8_t **out)
{
    if(chunk_size < 1 || chunk_size > GPR1_MAX_CHUNK) return 0;
    uint32_t n_chunks = (data_len + chunk_size - 1) / chunk_size;

    /* worst-case output size */
    uint32_t max_out = GPR1_HEADER_SZ
                     + n_chunks * (GPR1_RECORD_HDR_SZ + chunk_size + 8);
    *out = (uint8_t*)calloc(max_out, 1);
    if(!*out) return 0;

    uint32_t out_pos = GPR1_HEADER_SZ;
    for(uint32_t i = 0; i < n_chunks; i++){
        uint32_t off = i * chunk_size;
        uint32_t len = (off + chunk_size <= data_len) ? chunk_size
                                                      : (data_len - off);
        uint32_t rec_sz = gpr1_encode_record(
            i, data + off, len,
            *out + out_pos, max_out - out_pos);
        if(rec_sz == 0){ free(*out); *out = NULL; return 0; }
        out_pos += rec_sz;
    }

    /* write header */
    memcpy(*out, "GPR1", 4);
    (*out)[4] = GPR1_VERSION;
    uint16_t cs16 = chunk_size;
    uint64_t tl64 = data_len;
    memcpy(*out+5,  &cs16, 2);
    memcpy(*out+7,  &n_chunks, 2);
    memcpy(*out+9,  &tl64, 8);

    return out_pos;
}


/*
 * gpr1_decode_mem — decode GPR1 blob to original data
 *   Caller must provide out_cap >= total_len (from header).
 *   Returns original data length, or 0 on failure.
 */
static inline uint32_t gpr1_decode_mem(
        const uint8_t *blob, uint32_t blob_len,
        uint8_t *out, uint32_t out_cap)
{
    if(blob_len < GPR1_HEADER_SZ) return 0;
    if(memcmp(blob, "GPR1", 4) != 0) return 0;
    if(blob[4] != GPR1_VERSION) return 0;

    uint16_t chunk_size, n_chunks;
    uint64_t total_len;
    memcpy(&chunk_size, blob+5, 2);
    memcpy(&n_chunks,   blob+7, 2);
    memcpy(&total_len,  blob+9, 8);

    if(chunk_size < 1 || chunk_size > GPR1_MAX_CHUNK) return 0;
    if(total_len > out_cap) return 0;

    uint32_t offset = GPR1_HEADER_SZ;
    uint32_t out_pos = 0;

    for(uint32_t i = 0; i < n_chunks; i++){
        uint8_t pred[GPR1_MAX_CHUNK];
        int ret = gpr1_decode_record(blob, blob_len, &offset,
                                      pred, GPR1_MAX_CHUNK);
        if(ret < 0) return 0;
        if(out_pos + (uint32_t)ret > total_len){
            memcpy(out + out_pos, pred, total_len - out_pos);
            out_pos = (uint32_t)total_len;
            break;
        }
        memcpy(out + out_pos, pred, ret);
        out_pos += ret;
    }

    return out_pos;
}


/* ── File-level API ─────────────────────────────────── */

/*
 * gpr1_encode_file — encode file to .gpr1
 *   Returns 0 on success, -1 on error.
 */
static inline int gpr1_encode_file(
        const char *input_path,
        const char *output_path,
        uint8_t chunk_size)
{
    if(chunk_size < 1 || chunk_size > GPR1_MAX_CHUNK) return -1;

    FILE *fi = fopen(input_path, "rb");
    if(!fi) return -1;
    fseek(fi, 0, SEEK_END);
    uint32_t data_len = (uint32_t)ftell(fi);
    rewind(fi);

    uint8_t *data = (uint8_t*)malloc(data_len);
    if(!data){ fclose(fi); return -1; }
    if(fread(data, 1, data_len, fi) != data_len){
        free(data); fclose(fi); return -1;
    }
    fclose(fi);

    uint8_t *blob = NULL;
    uint32_t blob_len = gpr1_encode_mem(data, data_len, chunk_size, &blob);
    free(data);
    if(blob_len == 0 || !blob) return -1;

    FILE *fo = fopen(output_path, "wb");
    if(!fo){ free(blob); return -1; }
    fwrite(blob, 1, blob_len, fo);
    fclose(fo);
    free(blob);
    return 0;
}


/*
 * gpr1_decode_file — decode .gpr1 to output file
 *   Returns 0 on success, -1 on error.
 */
static inline int gpr1_decode_file(
        const char *input_path,
        const char *output_path)
{
    FILE *fi = fopen(input_path, "rb");
    if(!fi) return -1;
    fseek(fi, 0, SEEK_END);
    uint32_t blob_len = (uint32_t)ftell(fi);
    rewind(fi);

    uint8_t *blob = (uint8_t*)malloc(blob_len);
    if(!blob){ fclose(fi); return -1; }
    if(fread(blob, 1, blob_len, fi) != blob_len){
        free(blob); fclose(fi); return -1;
    }
    fclose(fi);

    /* peek total_len from header */
    if(blob_len < GPR1_HEADER_SZ || memcmp(blob, "GPR1", 4) != 0){
        free(blob); return -1;
    }
    uint64_t total_len;
    memcpy(&total_len, blob+9, 8);
    if(total_len > 1024*1024*1024){ free(blob); return -1; }  /* 1GB sanity */

    uint8_t *out = (uint8_t*)malloc((size_t)total_len);
    if(!out){ free(blob); return -1; }

    uint32_t out_len = gpr1_decode_mem(blob, blob_len, out, (uint32_t)total_len);
    free(blob);
    if(out_len == 0){ free(out); return -1; }

    FILE *fo = fopen(output_path, "wb");
    if(!fo){ free(out); return -1; }
    fwrite(out, 1, out_len, fo);
    fclose(fo);
    free(out);
    return 0;
}


/*
 * gpr1_info — read GPR1 header metadata
 *   Returns 0 on success, -1 on error.
 *   All args are output pointers (can be NULL to skip).
 */
static inline int gpr1_info(
        const char *path,
        uint16_t *out_chunk_size,
        uint16_t *out_n_chunks,
        uint64_t *out_total_len)
{
    FILE *f = fopen(path, "rb");
    if(!f) return -1;

    uint8_t hdr[GPR1_HEADER_SZ];
    if(fread(hdr, 1, GPR1_HEADER_SZ, f) != GPR1_HEADER_SZ){
        fclose(f); return -1;
    }
    fclose(f);

    if(memcmp(hdr, "GPR1", 4) != 0) return -1;
    if(hdr[4] != GPR1_VERSION) return -1;

    uint16_t cs, nc;
    uint64_t tl;
    memcpy(&cs, hdr+5, 2);
    memcpy(&nc, hdr+7, 2);
    memcpy(&tl, hdr+9, 8);

    if(out_chunk_size) *out_chunk_size = cs;
    if(out_n_chunks)   *out_n_chunks   = nc;
    if(out_total_len)  *out_total_len  = tl;
    return 0;
}
