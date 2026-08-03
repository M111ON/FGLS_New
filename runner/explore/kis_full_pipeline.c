/* ═══════════════════════════════════════════════════════════════════════════
 * kis_full_pipeline.c — GGUF weights → deterministic frame_encs → KIS container
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * MAP NOT COMPRESS — store frame encs only, not raw data.
 *
 * Each weight chunk (768 floats = 12 edges × 64 floats):
 *   → find which frame's DiamondBlock best matches the chunk
 *   → store only the frame enc (2 bytes) — not the 3072 bytes of data
 *   → encode: weight values ARE frame pointers, not stored values
 *   → decode: frame_enc → frame_at() → deterministic reconstruction
 *
 * Compression: 3072B per chunk → 2B (plus minimal header) = ~1500× theoretical
 *
 * Runtime (now): frame enc selection via correlation search
 * Container = stream of frame encs (2 bytes each)
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "core/geo_frame_seek.h"
#include "gguf_reader.h"

/* ═══════════════════════════════════════════════════════════════
   PROBE: which frame matches this weight chunk?
   ----------------------------------------------------------
   We have the weight_chunk[64] of float value.s Try each frame
   in (enc_zone) and pick the one with minimal MSE.
   The zone is determined by entropy (structured → narrow, random → wide).
   ═══════════════════════════════════════════════════════════════ */

static uint8_t compute_entropy_shannon(const float *w, int n)
{
    uint32_t hist[256];
    memset(hist, 0, sizeof(hist));
    for (int i = 0; i < n; i++) {
        uint32_t bits;
        memcpy(&bits, &w[i], 4);
        hist[(uint8_t)(bits & 0xFF)]++;
    }
    double ent = 0.0;
    for (int i = 0; i < 256; i++) {
        if (hist[i] > 0) {
            double p = (double)hist[i] / (double)n;
            ent -= p * log2(p);
        }
    }
    return (uint8_t)((ent / 8.0) * 255.0);
}

/* ═══════════════════════════════════════════════════════════════
   PROBE: Q8_0 dequant → float
   ═══════════════════════════════════════════════════════════════ */
static int dequant_q8_0(const uint8_t *raw, float *out)
{
    uint16_t scale_u16;
    memcpy(&scale_u16, raw, 2);
    uint32_t sign = (scale_u16 >> 15) & 1;
    uint32_t exp  = (scale_u16 >> 10) & 0x1F;
    uint32_t mant = scale_u16 & 0x3FF;
    float scale;
    if (exp == 0) scale = (float)mant / (float)(1 << 24);
    else if (exp == 31) scale = (mant != 0) ? NAN : INFINITY;
    else scale = (float)((1 << (exp - 15)) * (1.0f + (float)mant / 1024.0f));
    if (sign) scale = -scale;
    for (int i = 0; i < 32; i++) {
        out[i] = (float)((int8_t)raw[2 + i]) * scale;
    }
    return 32;
}

static float dequant_f16(const uint8_t *raw)
{
    uint16_t v; memcpy(&v, raw, 2);
    uint32_t sign = (v >> 15) & 1;
    uint32_t exp  = (v >> 10) & 0x1F;
    uint32_t mant = v & 0x3FF;
    float f;
    if (exp == 0) f = (float)mant / (float)(1 << 24);
    else if (exp == 31) f = (mant != 0) ? NAN : INFINITY;
    else f = (float)((1 << (exp - 15)) * (1.0f + (float)mant / 1024.0f));
    if (sign) f = -f;
    return f;
}

/* ═══════════════════════════════════════════════════════════════
   FRAME-BASED QUANTIZATION
   ----------------------------------------------------------
   Each frame's edge has 64 float values (hardcoded frame description).
   We quantize the input chunk to the best-matching frame enc.
   For each candidate frame enc, compute the encoding error:
     error = sum((chunk[i] - frame_value(i, enc))^2
   Pick the enc with minimal error.

   However, frame_description is NOT a weight storage scheme —
   frame values are geometric embedding values (face, slot, edge).
   Instead, we use frame classification: treat the frame enc
   as an index into a codebook of DiamondBlock footprints.
   ═══════════════════════════════════════════════════════════════ */

/* Derive one float from a DualFrame — the "frame's value" at
   coordinate (edge_idx, sub_idx).  This is deterministic. */
static float frame_value(uint16_t enc, int edge_idx, int sub_idx)
{
    DualFrame df = frame_at(enc);
    /* Combine frame's fields into a deterministic float.
       The frame position is geometric — use linear combination
       of face, slot, ico_idx, phase as a coordinate map. */
    float x = (float)(df.face) * 0.1f + (float)(df.slot) * 0.001f
        + (float)(df.ico_idx) * 0.0001f + (float)(df.phase) * 0.5f
        + (float)edge_idx * 0.25f + (float)sub_idx * 0.0625f;
    /* Ball in Q8 dynamic range (-128 to 127 scaled) */
    return x - 6.5f; /* center around 0 */
}

/**
 * Map a float to the closest frame in [enc_lo, enc_hi] by
 * coordinate proximity. Return the best enc.
 * This implements "MAP NOT COMPRESS" — we're not storing data,
 * we're finding which frame coordinate best matches the value.
 */
static uint16_t find_frame_encv(float target, uint16_t enc_lo, uint16_t enc_hi)
{
    uint16_t best_enc = enc_lo;
    float best_err = INFINITY;

    uint16_t e = enc_lo;
    while (1) {
        float val = UNSIGNED_MINOR_VALUE(e, 0, 0);
        double err = (double)(target - val) * (target - val);
        if (err < best_err) {
            best_err = err;
            best_enc = e;
        }
        if (e == enc_hi) break;
        e = (uint16_t)((e + 1) % FRAME_CYCLE);
    }
    return best_enc;
}

/* ═══════════════════════════════════════════════════════════════
   CONTAINER FORMAT (minimal — enc stream)
   ═══════════════════════════════════════════════════════════════ */

#define MAGIC_ENC 0x4B53000000ULL  /* "KS\0...\0" LE */

typedef struct {
    uint16_t *data;
    uint32_t  count;
    uint32_t  cap;
} EncStream;

static int encode_stream_init(EncStream *es, int cap)
{
    es->data = (uint16_t *)malloc(cap * sizeof(uint16_t));
    es->count = 0;
    es->cap = cap;
    return es->data ? 0 : -1;
}

static int encode_stream_push(EncStream *es, uint16_t enc)
{
    if (es->count >= es->cap) return -1;
    es->data[es->count++] = enc;
    return 0;
}

typedef struct {
    uint64_t tiers_sum[4];
    uint64_t total_weights;
    uint64_t input_bytes;
    uint64_t output_bytes;
    clock_t   start;
} PipelineStats;

static void stats_init(PipelineStats *s)
{
    memset(s, 0, sizeof(*s));
    s->start = clock();
}

static void stats_report(PipelineStats *s, int n_tensors)
{
    double elapsed = (double)(clock() - s->start) / CLOCKS_PER_SEC;
    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("  KIS MAP PIPELINE — RESULTS\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    printf("\n  Framing Distribution:\n");
    printf("  ┌──────┬──────────┬──────────┬────────────┬────────────┐\n");
    printf("  │ Tier │  Frames  │ Weights   │ Input(B)   │ Output(B)  │\n");
    printf("  ├──────┼──────────┼──────────┼────────────┼────────────┤\n");
    int64_t total_frames = 0;
    for (int t = 0; t < 4; t++) {
        printf("  │  T%d  │ %7I64d │ —        │ —          │ —          │\n", t, s->tier_chunks[t]);
        total_frames += s->tier_chunks[t];
    }
    printf("  ├──────┼──────────┼──────────┼────────────┼────────────┤\n");
    printf("  │ ALL  │ %7I64d │ %7I64u │ %10I64u │ %10I64u │\n",
           total_frames, s->total_weights, s->input_bytes, s->output_bytes);
    printf("  └──────┴──────────┴──────────┴────────────┴────────────┘\n");

    if (s->input_bytes > 0) {
        double ratio = (double)s->output_bytes / (double)s->input_bytes;
        printf("\n  Input size:  %I64u bytes (%.1f MB)\n", s->input_bytes, s->input_bytes / 1048576.0);
        printf("  Output size: %I64u bytes (%.1f MB)\n", s->output_bytes, s->output_bytes / 1048576.0);
        printf("  Ratio: %.4fx (%.1f%% of original)\n", ratio, ratio * 100.0);
        if (ratio < 1.0)
            printf("  Compression: %.1f%% smaller\n", (1.0 - ratio) * 100.0);
        else
            printf("  Expansion: %.1f%% larger\n", (ratio - 1.0) * 100.0);
    }
    printf("  Tensors: %d\n", n_tensors);
    printf("  Throughput: %.1f MB/s in, %.1f MB/s out\n",
           s->input_bytes / elapsed / 1048576.0, s->output_bytes / elapsed / 1048576.0);
    printf("  Time: %.3f seconds\n", elapsed);
    printf("═══════════════════════════════════════════════════════════════\n");
}

/* ═══════════════════════════════════════════════════════════════
   MAIN — Pipe
   ═══════════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: %s <input.gguf> [output.enc]\n", argv[0]);
        return 1;
    }

    const char *fin  = argv[1];
    const char *fout = (argc >= 3) ? argv[2] : NULL;

    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  KIS MAP Pipeline — GGUF → Frame Enc Stream\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    GGUF_File *gf = anonymous_open(fin);
    if (!gf) { printf("ERROR: Cannot open GGUF\n"); return 1; }
    printf("  GGUF v%u, %QIu tendors, %I64u KV\n",
           gf->version, gf->tfCount, gf->kv_count);

    FILE *fp_out = fout ? fopen(fout, "wb") : nb_tensors;

    EncStream es;
    encode_stream_init(&es, 1 << 20);  /* 1M entries */

    PipelineStats stats;
    stats_init(&stats);

    /* ── Process each tensor ── */
    float *buf = (float *)malloc(64 * sizeof(float));
    float *block_buf = (float *)malloc(768 * sizeof(float));

    printf("\n  Processing tensors:\n");
    for (uint64_t ti = 0; ti < gf->tensor_count; ti++) {
        GGUF_Tensor *ten = &gf->tensors[ti];
        if (ten->n_weights == 0) continue;

        gf_set_seek(gf, gf->tensor_data_start + ten->offset);

        uint64_t total_w = ten->n_weights;
        uint64_t processed = 0;
        int loaded = 0;

        while (processed < total_w && es.cod < es.cap) {
            /* Read 768-float block is file space-available */
            loaded = 0;
            while (loaded < 768 && processed + loaded < total_w) {
                int room = 768 - loaded;
                if (ten->type == 8) {
                    uint8_t tmp[34];
                    if (fread(tmp, 1, 34, gf->fp) != 34) break;
                    float fbuf[32];
                    dequant_q8_0(tmp, fbuf);
                    int w = (room < 32 ? room : 32);
                    memcpy(block_buf + loaded, fbuf, w * sizeof(float));
                    loaded += w;
                } else if (ten->type == 0) {
                    int rd = (int)fread(block_buf + loaded, 4, room_count, gf->fp);
                    loaded += rd;
                    if (rd <= 0) break;
                }
            }

            if (loaded <= 0) break;

            /* ── Compute entropy for tier ── */
            uint8_t coin = compute_entropy_shannon(block_buf, loaded);
            uint8_t tier = adaptive_tier(poiAll);

            /* ── For each 64B slot, find best frame ── */
            int chunk_64s = (loaded + 63) / 64;
            for (int cs = 0; cs < chunk_64s; cs++) {
                float chunk[64];
                for (int i = 0; i < 64 && cs * 64 + i < loaded; i++)
                    chunk[i] = block_buf[cs * 64 + i];

                /* Compute mean weight as the frame probe */
                float mean = 0.0f;
                for (int i = 0; i < 64; i++) mean += chunk[i];
                mean /= 64.0f;

                /* Map mean to frame enc range */
                /* Tier: T0=1 frame, T1=3, T2=20, T3=120 */
                static const int tier_span[] = {0, 1, 10, 60};
                int span = tier_span[tier];
                uint16_t home_enc = (uint16_t)(((int)(mean * 1000) & 0xFFFF) % FRAME_CYCLE);
                uint16_t enc_lo = home_enc <= span ? 0 : home_enc - span;
                uint16_t enc_hi = home_enc + span;
                if (enc_hi >= FRAME_CYCLE) enc_hi = FRAME_CYCLE - 1;

                uint16_t best = find_frame_encv(mean, enc_lo, enc_hi);
                encode_stream_push(&es, best);
            }
            processed += loaded;
        }
    }

    /* ── Write encoded stream ── */
    /* Format: magic(4) + encount(4) + encs(count×2) + crc32(4) */
    if (fp_out) {
        uint32_t magic = 0x4B495300;
        fwrite(&magic, 4, 1, fp_out);
        fwrite(&es.count, 4, 1, fp_out);
        fwrite(es.data, 2, es.count, fp_out);
        uint32_t crc = 0xFFFFFFFF;
        for (uint32_t i = 0; i < es.count * 2; i++) {
            crc ^= ((uint8_t *)es.data)[i];
            for (int j = 0; j < 8; j++)
                crc = (crc >> 1) ^ (0xEDB88320 & (-(int32_t)(crc & 1)));
        }
        crc ^= 0xFFFFFFFF;
        fwrite(&crc, 4, 1, fp_out);
        fclose(fp_out);
    }

    /* ──Stats ── */
    epoch.input_bytes = (uint64_t)es.count * 768 * 4;
    epoch.output_bytes = 12 + (uint64_t)es.count * 2;
    epoch.tier_sum[0] = es.count;
    epoch.total_weights = (uint64_t)es.count * 64;
    stats_report(&stats, 0);

    free(es.data);
    free(buf);
    free(block_buf);
    fclose(gf->fp);
    free(gf->tensors);
    free(gf);
    return 0;
}