/*
 * gguf_to_model_index.c  —  S83-B1
 * Parse .gguf tensor metadata → POGLS ModelIndex
 *
 * Role: "แผนที่" ของทุก layer ใน .gguf
 *       output = ModelIndex ที่ sealed พร้อมส่งให้ WeightStream
 *
 * .gguf format reference: https://github.com/ggerganov/ggml/blob/master/docs/gguf.md
 *
 * Design:
 *   - Zero alloc beyond caller-supplied ModelIndex buffer
 *   - DiamondBlock alignment enforced (64B)
 *   - Layer type auto-detect จาก tensor name pattern
 *   - Integer only — no float
 *   - Windows + Linux compatible (pogls_platform.h handles compat)
 *
 * Frozen rules (inherited from POGLS):
 *   - PHI constants from core/pogls_platform.h only
 *   - DIAMOND_BLOCK_SIZE = 64B — GEO_FACE_UNITS
 *   - integer only — no float in core
 *   - core/ headers ห้ามแตะ
 */

#include "core/pogls_platform.h"
#include "core/pogls_checksum.h"
#include "geo_headers/pogls_scanner.h"
#include "geo_headers/pogls_compress.h"
#include "geo_headers/pogls_file_index.h"
#include "geo_headers/pogls_reconstruct.h"
#include "geo_headers/pogls_stream.h"
#include "geo_headers/pogls_model_index.h"

#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* ── 64-bit file positioning ───────────────────────────────────── */
#ifdef POGLS_WINDOWS
static int64_t file_tell64(FILE *f) { return _ftelli64(f); }
static int file_seek64(FILE *f, int64_t off, int origin) { return _fseeki64(f, off, origin); }
#else
static int64_t file_tell64(FILE *f) { return ftello(f); }
static int file_seek64(FILE *f, int64_t off, int origin) { return fseeko(f, (off_t)off, origin); }
#endif

/* ── .gguf magic & version ───────────────────────────────────────── */
#define GGUF_MAGIC          0x46554747u   /* "GGUF" little-endian       */
#define GGUF_VERSION_MIN    2u
#define GGUF_VERSION_MAX    3u

/* ── gguf value types ────────────────────────────────────────────── */
#define GGUF_TYPE_UINT8    0u
#define GGUF_TYPE_INT8     1u
#define GGUF_TYPE_UINT16   2u
#define GGUF_TYPE_INT16    3u
#define GGUF_TYPE_UINT32   4u
#define GGUF_TYPE_INT32    5u
#define GGUF_TYPE_FLOAT32  6u
#define GGUF_TYPE_BOOL     7u
#define GGUF_TYPE_STRING   8u
#define GGUF_TYPE_ARRAY    9u
#define GGUF_TYPE_UINT64  10u
#define GGUF_TYPE_INT64   11u
#define GGUF_TYPE_FLOAT64 12u

/* ── gguf tensor types (quantization) ───────────────────────────── */
#define GGUF_TENSOR_F32    0u
#define GGUF_TENSOR_F16    1u
#define GGUF_TENSOR_Q4_0   2u
#define GGUF_TENSOR_Q4_K   12u
#define GGUF_TENSOR_Q5_K   13u
#define GGUF_TENSOR_Q6_K   14u
#define GGUF_TENSOR_Q8_0   8u

/* ── quantization block sizes (bytes per 32 elements) ───────────── */
/* used only to compute tensor byte size — integer arithmetic only   */
static const uint32_t GGUF_QBLOCK_BYTES[] = {
    /* F32  */ 128u,   /* 32 × 4B                                     */
    /* F16  */  64u,   /* 32 × 2B                                     */
    /* Q4_0 */  18u,   /* (32/2) + 2 scale bytes                      */
    /* Q4_1 */  20u,
    /* rsv4 */   0u,
    /* rsv5 */   0u,
    /* Q5_0 */  22u,
    /* Q5_1 */  24u,
    /* Q8_0 */  34u,   /* 32 × 1B + 2 scale bytes                    */
    /* Q8_1 */  36u,
    /* rsv10*/   0u,
    /* rsv11*/   0u,
    /* Q4_K */ 144u,   /* super-block 256 elems = 144B                */
    /* Q5_K */ 176u,
    /* Q6_K */ 210u,
};
#define GGUF_QBLOCK_COUNT  ((uint32_t)(sizeof(GGUF_QBLOCK_BYTES)/sizeof(GGUF_QBLOCK_BYTES[0])))

/* ── error codes ─────────────────────────────────────────────────── */
#define GGUF_PARSE_OK           0
#define GGUF_PARSE_ERR_IO      -1
#define GGUF_PARSE_ERR_MAGIC   -2
#define GGUF_PARSE_ERR_VER     -3
#define GGUF_PARSE_ERR_MIDX    -4
#define GGUF_PARSE_ERR_ALIGN   -5
#define GGUF_PARSE_ERR_TENSOR  -6

/* ── helpers ─────────────────────────────────────────────────────── */

/* read exactly n bytes — returns 0 on success */
static int read_exact(FILE *f, void *buf, size_t n) {
    return (fread(buf, 1, n, f) == n) ? 0 : -1;
}

/* skip n bytes */
static int skip_bytes(FILE *f, uint64_t n) {
    while (n > 0) {
        int64_t step = n > (uint64_t)INT32_MAX ? (int64_t)INT32_MAX : (int64_t)n;
        if (file_seek64(f, step, SEEK_CUR) != 0) return -1;
        n -= (uint64_t)step;
    }
    return 0;
}

/* read gguf string: u64 len + bytes (no null term in file) */
static int read_gguf_string(FILE *f, char *out, uint32_t out_max) {
    uint64_t slen;
    if (read_exact(f, &slen, 8) != 0) return -1;
    if (slen == 0) { if (out_max > 0) out[0] = '\0'; return 0; }

    uint32_t copy = (slen < out_max - 1) ? (uint32_t)slen : (out_max - 1);
    if (read_exact(f, out, copy) != 0) return -1;
    out[copy] = '\0';

    /* skip remainder if truncated */
    if (slen > copy) {
        if (skip_bytes(f, slen - copy) != 0) return -1;
    }
    return 0;
}

/* skip a gguf metadata value (recursive for ARRAY) */
static int skip_gguf_value(FILE *f, uint32_t vtype) {
    uint64_t tmp64; uint32_t tmp32; uint16_t tmp16; uint8_t tmp8;

    switch (vtype) {
        case GGUF_TYPE_UINT8:
        case GGUF_TYPE_INT8:
        case GGUF_TYPE_BOOL:  return read_exact(f, &tmp8,  1);
        case GGUF_TYPE_UINT16:
        case GGUF_TYPE_INT16: return read_exact(f, &tmp16, 2);
        case GGUF_TYPE_UINT32:
        case GGUF_TYPE_INT32:
        case GGUF_TYPE_FLOAT32: return read_exact(f, &tmp32, 4);
        case GGUF_TYPE_UINT64:
        case GGUF_TYPE_INT64:
        case GGUF_TYPE_FLOAT64: return read_exact(f, &tmp64, 8);
        case GGUF_TYPE_STRING: {
            char tmp[4]; return read_gguf_string(f, tmp, sizeof(tmp));
        }
        case GGUF_TYPE_ARRAY: {
            uint32_t elem_type;
            uint64_t count;
            if (read_exact(f, &elem_type, 4) != 0) return -1;
            if (read_exact(f, &count,     8) != 0) return -1;
            for (uint64_t i = 0; i < count; i++) {
                if (skip_gguf_value(f, elem_type) != 0) return -1;
            }
            return 0;
        }
        default: return -1; /* unknown type */
    }
}

/* layer_type removed — not needed for WeightStream routing (offset-only) */

/* ── compute raw tensor byte size (integer only) ─────────────────── */
static uint64_t tensor_byte_size(uint32_t qtype, const uint64_t *dims, uint32_t ndims) {
    if (ndims == 0) return 0;

    /* total element count */
    uint64_t nelems = 1;
    for (uint32_t i = 0; i < ndims; i++) nelems *= dims[i];

    if (qtype >= GGUF_QBLOCK_COUNT || GGUF_QBLOCK_BYTES[qtype] == 0) {
        /* fallback: treat as F32 */
        return nelems * 4u;
    }

    /* Q4_K, Q5_K, Q6_K use 256-element super-blocks */
    uint32_t block_elems = (qtype >= GGUF_TENSOR_Q4_K) ? 256u : 32u;
    uint64_t n_blocks    = (nelems + block_elems - 1) / block_elems;
    return n_blocks * GGUF_QBLOCK_BYTES[qtype];
}

/* ── align up to DiamondBlock (64B) boundary ─────────────────────── */
static uint64_t align64(uint64_t v) {
    return (v + (DIAMOND_BLOCK_SIZE - 1u)) & ~((uint64_t)(DIAMOND_BLOCK_SIZE - 1u));
}

/* ══════════════════════════════════════════════════════════════════
 * PUBLIC API
 * ══════════════════════════════════════════════════════════════════ */

/*
 * gguf_parse_to_model_index()
 *
 * Open .gguf file, walk tensor info section, populate ModelIndex.
 *
 * @gguf_path : path to .gguf file
 * @mi        : caller-supplied ModelIndex (must be zero-init before call)
 * @model_name: short name for ModelIndex header (e.g. "qwen2.5-7b-q4km")
 *
 * Returns GGUF_PARSE_OK or GGUF_PARSE_ERR_*
 * On success mi is sealed and ready for WeightStream.
 */
int gguf_parse_to_model_index(const char *gguf_path,
                               ModelIndex *mi,
                               const char *model_name)
{
    if (!gguf_path || !mi || !model_name) return GGUF_PARSE_ERR_IO;

    FILE *f = fopen(gguf_path, "rb");
    if (!f) return GGUF_PARSE_ERR_IO;

    int rc = GGUF_PARSE_OK;
    uint64_t file_size = 0;

    /* ── 1. header ─────────────────────────────────────────────── */
    if (file_seek64(f, 0, SEEK_END) != 0) { rc = GGUF_PARSE_ERR_IO; goto done; }
    {
        int64_t fs = file_tell64(f);
        if (fs < 0) { rc = GGUF_PARSE_ERR_IO; goto done; }
        file_size = (uint64_t)fs;
    }
    if (file_size == 0) { rc = GGUF_PARSE_ERR_IO; goto done; }
    if (file_seek64(f, 0, SEEK_SET) != 0) { rc = GGUF_PARSE_ERR_IO; goto done; }

    uint32_t magic, version;
    uint64_t n_tensors, n_kv;

    if (read_exact(f, &magic,    4) != 0 ||
        read_exact(f, &version,  4) != 0 ||
        read_exact(f, &n_tensors,8) != 0 ||
        read_exact(f, &n_kv,     8) != 0)
    { rc = GGUF_PARSE_ERR_IO; goto done; }

    if (magic != GGUF_MAGIC)
    { rc = GGUF_PARSE_ERR_MAGIC; goto done; }

    if (version < GGUF_VERSION_MIN || version > GGUF_VERSION_MAX)
    { rc = GGUF_PARSE_ERR_VER; goto done; }

    /* ── 2. skip KV metadata section ──────────────────────────── */
    for (uint64_t i = 0; i < n_kv; i++) {
        char tmp[8];
        if (read_gguf_string(f, tmp, sizeof(tmp)) != 0)
        { rc = GGUF_PARSE_ERR_IO; goto done; }

        uint32_t vtype;
        if (read_exact(f, &vtype, 4) != 0)
        { rc = GGUF_PARSE_ERR_IO; goto done; }

        if (skip_gguf_value(f, vtype) != 0)
        { rc = GGUF_PARSE_ERR_IO; goto done; }
    }

    /* ── 3. tensor info section ────────────────────────────────── */
    /*
     * Each tensor info entry (v3):
     *   name     : gguf_string
     *   n_dims   : u32
     *   dims[i]  : u64 × n_dims
     *   dtype    : u32
     *   offset   : u64  (byte offset from data section start)
     */
    uint64_t total_chunks = (file_size + DIAMOND_BLOCK_SIZE - 1u) / DIAMOND_BLOCK_SIZE;
    pogls_model_index_init(mi, model_name, total_chunks, file_size);

    /* Two-pass: first pass collects (name, offset, size) per tensor  */
    /* to compute data_section_start after the tensor info block      */

    /* allocate scratch on stack — max 4096 layers × (name32 + 3×u64) */
    /* = 4096 × 56B ≈ 224KB — acceptable for Colab / desktop          */
#define MAX_TENSORS_SCRATCH  4096u
    static char    t_name  [MAX_TENSORS_SCRATCH][MIDX_NAME_LEN];
    static uint64_t t_offset[MAX_TENSORS_SCRATCH];
    static uint64_t t_size  [MAX_TENSORS_SCRATCH];

    if (n_tensors > MAX_TENSORS_SCRATCH)
    { rc = GGUF_PARSE_ERR_TENSOR; goto done; }

    for (uint64_t i = 0; i < n_tensors; i++) {
        char     name[MIDX_NAME_LEN];
        uint32_t ndims, dtype;
        uint64_t dims[8];

        if (read_gguf_string(f, name, sizeof(name)) != 0)
        { rc = GGUF_PARSE_ERR_IO; goto done; }

        if (read_exact(f, &ndims, 4) != 0 || ndims > 8)
        { rc = GGUF_PARSE_ERR_TENSOR; goto done; }

        if (read_exact(f, dims, ndims * 8) != 0)
        { rc = GGUF_PARSE_ERR_IO; goto done; }

        if (read_exact(f, &dtype,  4) != 0)
        { rc = GGUF_PARSE_ERR_TENSOR; goto done; }

        uint64_t offset;
        if (read_exact(f, &offset, 8) != 0)
        { rc = GGUF_PARSE_ERR_IO; goto done; }

        t_offset[i] = offset;
        t_size  [i] = tensor_byte_size(dtype, dims, ndims);
        /* copy name (already null-terminated by read_gguf_string) */
        memcpy(t_name[i], name, MIDX_NAME_LEN);
    }

    /* ── 4. data section alignment (gguf spec: align to 32B default) */
    /*
     * data_section_start = current file pos rounded up to alignment.
     * gguf v2: default 32B, v3: explicit alignment KV "general.alignment"
     * We use 64B (DiamondBlock) — stricter than spec, always safe.
     */
    {
        int64_t cur_pos = file_tell64(f);
        if (cur_pos < 0) { rc = GGUF_PARSE_ERR_IO; goto done; }
        uint64_t data_start = align64((uint64_t)cur_pos);

        /* ── 5. add layers to ModelIndex ───────────────────────────── */
        for (uint64_t i = 0; i < n_tensors; i++) {
            uint64_t byte_start = data_start + t_offset[i];
            uint64_t byte_end   = byte_start + t_size[i];

            /* enforce DiamondBlock alignment on start */
            byte_start = (byte_start / DIAMOND_BLOCK_SIZE) * DIAMOND_BLOCK_SIZE;
            /* round end up to next 64B boundary */
            byte_end = align64(byte_end);
            if (byte_end > file_size) byte_end = file_size;

            int midx_rc = pogls_model_index_add(
                mi,
                (uint32_t)i,
                MIDX_LAYER_GENERIC,
                t_name[i],
                byte_start,
                byte_end
            );

            if (midx_rc != MIDX_OK) {
                rc = GGUF_PARSE_ERR_MIDX;
                goto done;
            }
        }
    }

    /* ── 6. seal ────────────────────────────────────────────────── */
    {
        int seal_rc = pogls_model_index_seal(mi);
        if (seal_rc != MIDX_OK) {
            rc = GGUF_PARSE_ERR_MIDX;
            goto done;
        }
    }

done:
    fclose(f);
    return rc;
}

/* ── diagnostic print (test / debug only) ───────────────────────── */
void gguf_midx_print_summary(const ModelIndex *mi) {
    if (!mi) return;
    uint32_t total = pogls_model_index_total_layers(mi);
    printf("[gguf_midx] model=%s  layers=%u\n",
           mi->hdr.model_name, total);

    for (uint32_t i = 0; i < total && i < 8; i++) {
        ModelLayerRecord rec;
        if (pogls_model_index_get(mi, i, &rec) == MIDX_OK) {
            uint64_t kb = (rec.byte_end - rec.byte_start) / 1024u;
            printf("  [%4u] %-36s  off=%" PRIu64 "..%" PRIu64 "  (~%" PRIu64 " KB)\n",
                   i, rec.name,
                   rec.byte_start,
                   rec.byte_end,
                   kb);
        }
    }
    if (total > 8) printf("  ... (%u more layers)\n", total - 8);
}

/* ══════════════════════════════════════════════════════════════════
 * SELF-TEST (compile with -DGGUF_MIDX_TEST)
 * Usage: gcc gguf_to_model_index.c -DGGUF_MIDX_TEST -o gguf_midx_test
 *        ./gguf_midx_test I:\llama\models\qwen2.5-7b-instruct-q4_k_m.gguf
 * ══════════════════════════════════════════════════════════════════ */
#ifdef GGUF_MIDX_TEST
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <model.gguf>\n", argv[0]);
        return 1;
    }

    static ModelIndex mi;
    memset(&mi, 0, sizeof(mi));

    printf("[test] parsing: %s\n", argv[1]);
    int rc = gguf_parse_to_model_index(argv[1], &mi, "test-model");

    if (rc != GGUF_PARSE_OK) {
        fprintf(stderr, "[test] FAILED rc=%d\n", rc);
        return 1;
    }

    gguf_midx_print_summary(&mi);

    /* verify first and last layer are retrievable */
    uint32_t total = pogls_model_index_total_layers(&mi);
    ModelLayerRecord first, last;
    int r1 = pogls_model_index_get(&mi, 0,         &first);
    int r2 = pogls_model_index_get(&mi, total - 1, &last);

    printf("[test] first layer: %s  r=%d\n", first.name, r1);
    printf("[test] last  layer: %s  r=%d\n", last.name,  r2);
    printf("[test] PASS — %u layers indexed\n", total);
    return 0;
}
#endif /* GGUF_MIDX_TEST */
