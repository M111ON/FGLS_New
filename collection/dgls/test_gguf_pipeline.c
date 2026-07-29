/*
 * test_gguf_pipeline.c — GGUF Real Data → Pipeline → DRamTile
 * ═══════════════════════════════════════════════════════════════════
 *
 * Connects real GGUF tensor data through the full geometric pipeline:
 *   GGUF → CHUNKING → BONDING → SHELLING → PIXELATING → ARRANGE → SPLIT → DRAMTILE
 *
 * Reports per-chunk geometric statistics:
 *   - FLAT/SPARSE/DENSE distribution
 *   - fibo_isect popcount (pc) distribution
 *   - Non-zero byte (nz) distribution
 *   - Hilbert z-order distribution
 *
 * Build:
 *   cd collection/dgls
 *   make gguf-pipe
 *
 * Run:
 *   ./test_gguf_pipeline.exe /i/model/Qwen3-0.6B-Q4_0.gguf [num_chunks]
 *
 * No GPU required — pure CPU pipeline test with real model data.
 */

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

/* ── Pipeline & DRamTile includes ──────────────────────────────── */
#ifndef NOGDI
#define NOGDI
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "pipeline_glue.h"
#include "dramtile_store.h"

/* ── Binary Shell Codec (v3 — pc-based classification) ────────── */
#include "binary_shell_codec.h"
#include "diamond_shell_v2.h"

/* ── DRamTile buffer (simplified — VirtualLock / mlock) ──────── */
typedef struct {
    uint8_t  *base;
    size_t    size;
    int       locked;
} DTileBuf;

static inline int dtile_init(DTileBuf *db, size_t sz)
{
    memset(db, 0, sizeof(*db));
    db->base = (uint8_t *)malloc(sz);
    if (!db->base) return -1;
    memset(db->base, 0, sz);
    db->size = sz;
    db->locked = 0;
    return 0;
}

static inline int dtile_lock(DTileBuf *db)
{
    if (db->locked) return 0;
    dt_enable_lock_privilege();
    int ret = dt_lock_pages(db->base, db->size);
    db->locked = (ret == 0);
    return ret;
}

static inline void dtile_free(DTileBuf *db)
{
    if (db->locked)
        dt_unlock_pages(db->base, db->size);
    free(db->base);
    db->base = NULL;
    db->size = 0;
}

/* ══════════════════════════════════════════════════════════════════
   GGUF Reader (V3) — extract tensor data bytes
   ══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t magic;         /* GGUF at offset 0           */
    uint32_t version;       /* 3                           */
    uint64_t n_tensors;     /* number of tensors           */
    uint64_t metadata_kv;   /* key-value count             */
} GGUFHeader;

#define GGUF_MAGIC 0x46554747u

/* ── Recursive GGUF value skipper ──
 * Type numbering (from llama.cpp GGUF spec V1/V2 — used by Qwen3 V3 files):
 *   0=uint8, 1=int8, 2=uint16, 3=int16, 4=uint32, 5=int32,
 *   6=float32, 7=bool, 8=string, 9=array, 10=uint64, 11=int64,
 *   12=float64, 13=bf16
 */
static int gguf_skip_value(FILE *f, uint32_t vtype)
{
    switch (vtype) {
        case 0: case 1:
            fseek(f, 1, SEEK_CUR); return 1;
        case 2: case 3:
            fseek(f, 2, SEEK_CUR); return 1;
        case 4: case 5: case 6:
            fseek(f, 4, SEEK_CUR); return 1;
        case 7:
            fseek(f, 1, SEEK_CUR); return 1; /* bool → 1 byte in actual GGUF V3 */
        case 8: {
            uint64_t slen;
            if (fread(&slen, sizeof(slen), 1, f) != 1) return 0;
            fseek(f, (long)slen, SEEK_CUR);
            return 1;
        }
        case 9: {
            uint32_t at; uint64_t alen;
            if (fread(&at, sizeof(at), 1, f) != 1 ||
                fread(&alen, sizeof(alen), 1, f) != 1) return 0;
            for (uint64_t j = 0; j < alen; j++)
                if (!gguf_skip_value(f, at)) return 0;
            return 1;
        }
        case 10: case 11: case 12: case 13:
            fseek(f, 8, SEEK_CUR); return 1;
        default:
            fseek(f, 4, SEEK_CUR); return 1;
    }
}

static uint8_t* read_gguf_tensor_data(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "ERROR: cannot open %s\\n", path); return NULL; }

    /* Read header */
    GGUFHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fprintf(stderr, "ERROR: cannot read GGUF header\\n");
        fclose(f); return NULL;
    }
    if (hdr.magic != GGUF_MAGIC) {
        fprintf(stderr, "ERROR: not a GGUF file (magic=0x%08x)\\n", hdr.magic);
        fclose(f); return NULL;
    }
    printf("  GGUF v%u  tensors=%llu  kv=%llu\\n",
           (unsigned)hdr.version,
           (unsigned long long)hdr.n_tensors,
           (unsigned long long)hdr.metadata_kv);

           /* Skip metadata KV pairs using recursive value skipper */
           for (uint64_t i = 0; i < hdr.metadata_kv; i++) {
        uint64_t klen;
        if (fread(&klen, sizeof(klen), 1, f) != 1) {
            fprintf(stderr, "ERROR: reading metadata key length at KV %llu\\n",
                    (unsigned long long)i);
            fclose(f); return NULL;
        }
        fseek(f, (long)klen, SEEK_CUR);

        uint32_t vtype;
        if (fread(&vtype, sizeof(vtype), 1, f) != 1) {
            fprintf(stderr, "ERROR: reading value type at KV %llu\\n",
                    (unsigned long long)i);
            fclose(f); return NULL;
        }
        if (!gguf_skip_value(f, vtype)) {
            fprintf(stderr, "ERROR: skip value failed at KV %llu type=%u\\n",
                    (unsigned long long)i, vtype);
            fclose(f); return NULL;
        }
    }

    /* Skip tensor info */
    for (uint64_t i = 0; i < hdr.n_tensors; i++) {
        uint64_t nlen;
        if (fread(&nlen, sizeof(nlen), 1, f) != 1) {
            fprintf(stderr, "ERROR: reading tensor name length\\n");
            fclose(f); return NULL;
        }
        fseek(f, (long)nlen, SEEK_CUR);
        uint32_t n_dims;
        if (fread(&n_dims, sizeof(n_dims), 1, f) != 1) {
            fprintf(stderr, "ERROR: reading tensor n_dims\\n");
            fclose(f); return NULL;
        }
        fseek(f, (long)(n_dims * sizeof(int64_t)), SEEK_CUR);
        fseek(f, (long)(sizeof(uint32_t) + sizeof(uint64_t)), SEEK_CUR);
    }

    /* Read tensor data */
    long data_start = ftell(f);
    fseek(f, 0, SEEK_END);
    long data_end = ftell(f);
    size_t data_size = (size_t)(data_end - data_start);
    fseek(f, data_start, SEEK_SET);

    if (data_size == 0) {
        fprintf(stderr, "ERROR: no tensor data found\\n");
        fclose(f); return NULL;
    }

    uint8_t *data = (uint8_t*)malloc(data_size);
    if (!data) {
        fprintf(stderr, "ERROR: malloc(%zu) failed\\n", data_size);
        fclose(f); return NULL;
    }

    size_t nread = fread(data, 1, data_size, f);
    fclose(f);

    if (nread != data_size) {
        fprintf(stderr, "ERROR: read %zu/%zu bytes\\n", nread, data_size);
        free(data); return NULL;
    }

    printf("  Tensor data: %zu bytes (%.2f MB)\\n", data_size, data_size / 1048576.0);
    *out_size = data_size;
    return data;
}

/* ══════════════════════════════════════════════════════════════════
   GEOMETRIC STATISTICS
   ══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t n_flat;
    uint32_t n_sparse;
    uint32_t n_dense;
    uint32_t n_raw;
    uint32_t total;

    uint32_t pc_min, pc_max, pc_sum;
    uint32_t nz_min, nz_max, nz_sum;

    double   ratio_sum;      /* sum of (enc_size / 64.0) */
} GeoStats;

static void stats_init(GeoStats *s)
{
    memset(s, 0, sizeof(*s));
    s->pc_min = 64;
    s->pc_max = 0;
    s->nz_min = 64;
    s->nz_max = 0;
}

static void stats_add(GeoStats *s, const BinChunkResult *r)
{
    s->total++;
    switch (r->flag) {
        case BIN_FLAG_FLAT:   s->n_flat++;   break;
        case BIN_FLAG_SPARSE: s->n_sparse++; break;
        case BIN_FLAG_DENSE:  s->n_dense++;  break;
        case BIN_FLAG_RAW:    s->n_raw++;    break;
    }
    if (r->isect_pc < s->pc_min) s->pc_min = r->isect_pc;
    if (r->isect_pc > s->pc_max) s->pc_max = r->isect_pc;
    s->pc_sum += r->isect_pc;
    if (r->nz_count < s->nz_min) s->nz_min = r->nz_count;
    if (r->nz_count > s->nz_max) s->nz_max = r->nz_count;
    s->nz_sum += r->nz_count;
    s->ratio_sum += (double)r->enc_size / 64.0;
}

static void stats_print(const GeoStats *s)
{
    printf("\n═══ Geometric Statistics (%u chunks) ═══\n", s->total);
    double pct = 100.0 / (s->total ? s->total : 1);
    printf("  FLAT:   %5u (%5.1f%%)  [all-zero]\n",  s->n_flat,  s->n_flat  * pct);
    printf("  SPARSE: %5u (%5.1f%%)  [pc≤%d, nz≤%d]\n", s->n_sparse, s->n_sparse * pct,
           BIN_PC_THRESH, BIN_NZ_THRESH);
    printf("  DENSE:  %5u (%5.1f%%)  [pc>%d or nz>%d]\n", s->n_dense, s->n_dense * pct,
           BIN_PC_THRESH, BIN_NZ_THRESH);
    if (s->total > 0) {
        printf("  Avg pc: %.1f  [range %u..%u]\n",
               (double)s->pc_sum / s->total, s->pc_min, s->pc_max);
        printf("  Avg nz: %.1f  [range %u..%u]\n",
               (double)s->nz_sum / s->total, s->nz_min, s->nz_max);
        printf("  Avg ratio: %.4f\n", s->ratio_sum / s->total);
    }
    double ratio = s->ratio_sum / (s->total ? s->total : 1);
    printf("  Effective ratio: %.4f (%.2f%% of original)\n",
           ratio, ratio * 100.0);
}

/* ══════════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    printf("═══ GGUF → Geometric Pipeline Integration ═══\n\n");

    if (argc < 2) {
        printf("Usage: %s <gguf_path> [num_chunks]\n", argv[0]);
        printf("  num_chunks: chunks to process (default: all = all)\n");
        printf("  Example: %s /i/model/Qwen3-0.6B-Q4_0.gguf 1000\n", argv[0]);
        return 1;
    }

    const char *gguf_path = argv[1];
    uint32_t max_chunks = (uint32_t)-1;  /* all */
    if (argc >= 3) {
        unsigned long n = strtoul(argv[2], NULL, 10);
        if (n > 0) max_chunks = (uint32_t)n;
    }

    /* ── 1. Read GGUF tensor data ───────────────────────────── */
    printf("[gguf] Reading %s ...\n", gguf_path);
    size_t data_size = 0;
    uint8_t *tensor_data = read_gguf_tensor_data(gguf_path, &data_size);
    if (!tensor_data) return 1;

    uint32_t n_chunks = (uint32_t)(data_size / 64);
    if (n_chunks == 0) { printf("ERROR: data too small\n"); free(tensor_data); return 1; }
    if (max_chunks < n_chunks) n_chunks = max_chunks;
    uint32_t input_size = n_chunks * 64;
    printf("[gguf] Processing %u chunks (%u bytes)\n", n_chunks, input_size);

    /* ── 2. Run pipeline (chunking through pixelating) ──────── */
    PGContext pg;
    memset(&pg, 0, sizeof(pg));
    int ret = pg_init(&pg, tensor_data, input_size, 0xF00D, PG_FLAG_NONE);
    if (ret != 0) { printf("[pipeline] init FAIL\n"); free(tensor_data); return 1; }

    if (pg_stage_chunking(&pg)  == 0) { printf("[pipeline] chunk FAIL\n");  goto cleanup; }
    if (pg_stage_bonding(&pg)   == 0) { printf("[pipeline] bond FAIL\n");   goto cleanup; }
    if (pg_stage_shelling(&pg)  == 0) { printf("[pipeline] shell FAIL\n");  goto cleanup; }
    if (pg_stage_pixelating(&pg)== 0) { printf("[pipeline] pixel FAIL\n");  goto cleanup; }

    printf("[pipeline] %u chunks processed\n", pg.n_chunks);

    /* ── 3. ARRANGE: Hilbert sort + sequence headers ────────── */
    /* (Reusing inline arrange from test_pipeline_integration.c approach) */

    /* Compute Hilbert z-order for each chunk */
    uint32_t *z_order = (uint32_t *)malloc(n_chunks * sizeof(uint32_t));
    if (!z_order) { printf("[arrange] alloc FAIL\n"); goto cleanup; }

    /* 2D Hilbert on 4x4 grid */
    static const uint8_t hilb[16][2] = {
        {0,0},{0,1},{1,1},{1,0},{0,2},{0,3},{1,3},{1,2},
        {2,2},{2,3},{3,3},{3,2},{2,1},{2,0},{3,0},{3,1}
    };

    for (uint32_t i = 0; i < n_chunks; i++) {
        uint64_t hl = pg.chunks[i].piece.bond_L;
        uint8_t hx = (uint8_t)(hl >> 0) & 3;
        uint8_t hy = (uint8_t)(hl >> 2) & 3;
        uint32_t z = 0;
        for (int j = 0; j < 16; j++)
            if (hilb[j][0] == hx && hilb[j][1] == hy) { z = j; break; }
        z_order[i] = z;
    }

    /* Sort by z-order */
    typedef struct { uint32_t z; uint32_t idx; } ZPair;
    ZPair *zp = (ZPair *)malloc(n_chunks * sizeof(ZPair));
    if (!zp) { printf("[arrange] zp alloc FAIL\n"); goto cleanup; }
    for (uint32_t i = 0; i < n_chunks; i++)
        { zp[i].z = z_order[i]; zp[i].idx = i; }
    for (uint32_t i = 1; i < n_chunks; i++) {
        ZPair t = zp[i];
        int j = (int)i - 1;
        while (j >= 0 && zp[j].z > t.z) { zp[j+1] = zp[j]; j--; }
        zp[j+1] = t;
    }

    /* ── 4. SPLIT: payload to DRamTile, classify all chunks ─── */
    GeoStats gstats;
    stats_init(&gstats);

    DTileBuf dt;
    if (dtile_init(&dt, n_chunks * 64) != 0) {
        printf("[split] dtile init FAIL\n"); goto cleanup;
    }

    uint32_t arrange_ok = 1;
    uint32_t roundtrip_ok = 1;

    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t ci = zp[i].idx;
        PGChunk *ch = &pg.chunks[ci];

        /* Classify chunk */
        BinChunkResult bcr = bin_classify_chunk(ch->data);
        stats_add(&gstats, &bcr);

        /* Write to DRamTile */
        memcpy(dt.base + i * 64, ch->data, 64);

        /* Encode/decode roundtrip test */
        uint8_t enc_buf[128];
        BinChunkResult bcr2;
        uint32_t esz = bin_encode_chunk(enc_buf, ch->data, &bcr2);
        uint8_t dec[64];
        uint32_t dsz = bin_decode_chunk(enc_buf, dec);
        if (dsz != esz || memcmp(dec, ch->data, 64) != 0) {
            if (roundtrip_ok)
                printf("[roundtrip] FAIL at chunk %u (flag=%u sz=%u/%u)\n",
                       ci, bcr2.flag, esz, dsz);
            roundtrip_ok = 0;
        }

        /* Verify z-order */
        if (i > 0 && z_order[zp[i-1].idx] > z_order[ci]) {
            arrange_ok = 0;
        }
    }

    printf("[arrange] z-order %s\n", arrange_ok ? "PASS" : "FAIL");
    printf("[roundtrip] %s\n", roundtrip_ok ? "PASS" : "FAIL");
    printf("[split] %u bytes to DRamTile\n", (uint32_t)(n_chunks * 64));

    /* ── 5. Print statistics ────────────────────────────────── */
    stats_print(&gstats);

    /* ── 6. DRamTile readback verify ─────────────────────────── */
    uint32_t dram_ok = 1;
    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t ci = zp[i].idx;
        if (memcmp(dt.base + i * 64, pg.chunks[ci].data, 64) != 0) {
            dram_ok = 0; break;
        }
    }
    printf("[dramtile] readback %s\n", dram_ok ? "PASS" : "FAIL");

    /* ── Print FILO TOTALS ──────────────────────────────────── */
    uint32_t overall = (arrange_ok && roundtrip_ok && dram_ok) ? 1 : 0;
    printf("\n═══════════════════════════════════════════\n");
    printf("GGUF Pipeline: %s\n", overall ? "ALL PASS" : "SOME FAILS");

    /* ── Cleanup ─────────────────────────────────────────────── */
    free(z_order);
    free(zp);
    dtile_free(&dt);
    pg_free(&pg);
    free(tensor_data);
    return overall ? 0 : 1;

cleanup:
    pg_free(&pg);
    free(tensor_data);
    return 1;
}
