/*
 * pipe_tts.h — Kokoro TTS Integration via Pipe ABI
 *
 * Wraps Kokoro-82M tensor I/O through the Pipe/DRamTile system.
 * Architecture (from config.json + checkpoint analysis):
 *   text_encoder:   dim_in=64, hidden_dim=512, n_layer=3, kernel=5
 *   plbert:         hidden_size=768, n_layer=12, attn_heads=12, ff=2048
 *   predictor:      style_dim=128, max_dur=50
 *   decoder:        istftnet, upsample_rates=[10,6], initial_channel=512
 *   n_token=178, n_mels=80, total ~54 tensors
 *
 * Usage:
 *   PipeContext *tts = pipe_open_tts("kokoro", NULL);
 *   pipe_write_tts_tensor(tts, "text_encoder.embedding.weight", data, nbytes);
 *   const void *w = pipe_read_tts_tensor(tts, "decoder.istft.res_weight");
 */

#ifndef PIPE_TTS_H
#define PIPE_TTS_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "pipe_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════
 * KOKORO TTS ARCHITECTURE CONSTANTS
 * ═══════════════════════════════════════════════════════════════ */

#define TTS_N_TOKEN             178
#define TTS_DIM_IN              64
#define TTS_HIDDEN_DIM          512
#define TTS_MAX_CONV_DIM        512
#define TTS_N_LAYER             3
#define TTS_N_MELS              80
#define TTS_STYLE_DIM           128
#define TTS_MAX_DUR             50
#define TTS_TEXT_ENCODER_KERNEL 5
#define TTS_DROPOUT             0.2f

/* PLBERT */
#define TTS_PLBERT_HIDDEN       768
#define TTS_PLBERT_N_LAYER      12
#define TTS_PLBERT_N_HEADS      12
#define TTS_PLBERT_FF           2048
#define TTS_PLBERT_MAX_POS      512

/* Decoder (iSTFTNet) */
#define TTS_UP_INITIAL_CHANNEL  512
#define TTS_UP_RATES_COUNT      2
#define TTS_N_RESBLOCK          3
#define TTS_RESBLOCK_KERNELS    {3,7,11}
#define TTS_RESBLOCK_DILATIONS  {{1,3,5},{1,3,5},{1,3,5}}

/* Total known tensors */
#define TTS_N_TENSORS           54

/* ═══════════════════════════════════════════════════════════════
 * KOKORO TENSOR NAME PREFIXES
 * ═══════════════════════════════════════════════════════════════
 * The .pth checkpoint has 5 top-level groups:
 *   bert, bert_encoder, predictor, decoder, text_encoder
 * Each is an OrderedDict with sub-modules.
 * ═══════════════════════════════════════════════════════════════ */

#define TTS_GROUP_BERT          "bert"
#define TTS_GROUP_BERT_ENCODER  "bert_encoder"
#define TTS_GROUP_PREDICTOR     "predictor"
#define TTS_GROUP_DECODER       "decoder"
#define TTS_GROUP_TEXT_ENCODER  "text_encoder"

/* ═══════════════════════════════════════════════════════════════
 * SID CONFIG — Kokoro TTS uses 4 faces (low variance, stable)
 * Based on HANDOFF_CTD: max_shift=0.629, Y6 lv1=93%
 *
 * NOTE: In production, #include "sid.h" and use SIDArchConfig.
 * This header defines a local SID config struct to avoid dragging
 * the entire collection dependency tree into simple pipe apps.
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    int         n_faces;
    int         use_tri;
    const void *face_order;
} TTS_SIDConfig;

static inline TTS_SIDConfig tts_sid_config(void) {
    TTS_SIDConfig c = { .n_faces = 4, .use_tri = 0, .face_order = NULL };
    return c;
}

/* ═══════════════════════════════════════════════════════════════
 * TTS TENSOR NAME LOOKUP
 * ═══════════════════════════════════════════════════════════════
 * Check if a tensor name belongs to Kokoro TTS.
 * Returns 1 if name matches TTS pattern, 0 otherwise.
 * ═══════════════════════════════════════════════════════════════ */

static inline int tts_is_tts_tensor(const char *name) {
    if (!name) return 0;
    return (strstr(name, TTS_GROUP_BERT) == name ||
            strstr(name, TTS_GROUP_BERT_ENCODER) == name ||
            strstr(name, TTS_GROUP_PREDICTOR) == name ||
            strstr(name, TTS_GROUP_DECODER) == name ||
            strstr(name, TTS_GROUP_TEXT_ENCODER) == name);
}

/* ═══════════════════════════════════════════════════════════════
 * PIPE OPEN — Create a TTS-specific pipe
 * ═══════════════════════════════════════════════════════════════
 * If config is NULL, uses TTS defaults:
 *   name = "kokoro-82M"
 *   capacity = 256MB (typical Kokoro is ~150MB)
 *   max_slots = TTS_N_TENSORS
 *   backend_path = optional file path for persistence
 * ═══════════════════════════════════════════════════════════════ */

static inline PipeContext *pipe_open_tts(const char *name, const char *backend_path) {
    PipeConfig cfg = PIPE_CONFIG_DEFAULT;
    cfg.name = name ? name : "kokoro-82M";
    cfg.capacity = 256UL * 1024 * 1024;  /* 256 MB */
    cfg.max_slots = TTS_N_TENSORS;
    cfg.backend_path = backend_path;
    return pipe_open(&cfg);
}

/* ═══════════════════════════════════════════════════════════════
 * PIPE WRITE TENSOR — Write a named tensor to TTS pipe
 * ═══════════════════════════════════════════════════════════════
 * Returns PIPE_OK on success, error code on failure. */
static inline int pipe_write_tts_tensor(PipeContext *ctx, const char *name,
                                         const void *data, uint32_t nbytes)
{
    return pipe_write(ctx, name, data, nbytes, PIPE_DTYPE_F32, PIPE_PRIO_NORMAL);
}

/* ═══════════════════════════════════════════════════════════════
 * PIPE READ TENSOR — Read a named tensor from TTS pipe (zero-copy)
 * ═══════════════════════════════════════════════════════════════
 * Returns pointer to tensor data, or NULL if not found.
 * nbytes is set to the tensor size. */
static inline const void *pipe_read_tts_tensor(PipeContext *ctx, const char *name,
                                                uint32_t *nbytes)
{
    return pipe_read(ctx, name, nbytes);
}

/* ═══════════════════════════════════════════════════════════════
 * PIPE LOAD ALL — Load all 5 checkpoint groups iteratively
 * ═══════════════════════════════════════════════════════════════
 * Calls a user-provided callback for each of the 5 known groups.
 * Returns the total tensors loaded. */
typedef int (*tts_group_loader)(const char *group_name, void *user);

static inline int tts_load_groups(PipeContext *ctx,
                                   tts_group_loader loader, void *user)
{
    const char *groups[] = {
        TTS_GROUP_BERT,
        TTS_GROUP_BERT_ENCODER,
        TTS_GROUP_PREDICTOR,
        TTS_GROUP_DECODER,
        TTS_GROUP_TEXT_ENCODER
    };
    int total = 0;
    for (size_t i = 0; i < sizeof(groups)/sizeof(groups[0]); i++) {
        if (loader(groups[i], user) == 0) total++;
    }
    return total;
}

/* ═══════════════════════════════════════════════════════════════
 * PIPE STATUS — Print TTS pipe stats
 * ═══════════════════════════════════════════════════════════════ */
static inline void pipe_tts_status(PipeContext *ctx) {
    if (!ctx) { printf("[tts] pipe not open\n"); return; }
    PipeStats s;
    pipe_stats(ctx, &s);
    printf("[tts] %s: %u/%u slots, %llu/%llu bytes, %llu reads, %llu writes\n",
           pipe_name(ctx), s.n_slots, ctx->n_slots,
           (unsigned long long)s.capacity_used,
           (unsigned long long)s.capacity_total,
           (unsigned long long)s.n_reads,
           (unsigned long long)s.n_writes);
}

#ifdef __cplusplus
}
#endif

#endif /* PIPE_TTS_H */
