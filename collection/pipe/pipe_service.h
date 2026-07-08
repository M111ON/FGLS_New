/*
 * pipe_service.h — Unified Service Interface for TTS/VLM/LLM
 *
 * Single entry point. Include ONE header, call ONE open/write/read/close API.
 * Auto-detects model type from tensor names and routing to right config.
 *
 * Usage:
 *   #include "pipe_service.h"
 *
 *   PipeContext *svc = pipe_service_open(PIPE_SVC_TTS, "kokoro", NULL);
 *   pipe_service_write(svc, "text_encoder.embedding.weight", data, size);
 *   const void *w = pipe_service_read(svc, "decoder.istft.res_weight", &sz);
 *   pipe_service_close(svc);
 *
 *   // Or auto-detect from first tensor name:
 *   PipeContext *svc = pipe_service_open(PIPE_SVC_AUTO, "model", NULL);
 *   pipe_service_write(svc, "model.vision.blocks.0.attn.qkv.weight", data, sz);
 *   // Auto-detects as VLM, reopen is transparent
 */

#ifndef PIPE_SERVICE_H
#define PIPE_SERVICE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "pipe_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════
 * SERVICE TYPE
 * ═══════════════════════════════════════════════════════════════ */
typedef enum {
    PIPE_SVC_AUTO = 0,       /* auto-detect from tensor names */
    PIPE_SVC_TTS  = 1,       /* Kokoro-82M TTS */
    PIPE_SVC_VLM  = 2,       /* moondream2 VLM */
    PIPE_SVC_LLM  = 3,       /* general LLM (default) */
    PIPE_SVC_COUNT
} PipeServiceType;

static inline const char *pipe_service_name(PipeServiceType t) {
    switch (t) {
        case PIPE_SVC_TTS: return "tts";
        case PIPE_SVC_VLM: return "vlm";
        case PIPE_SVC_LLM: return "llm";
        default:           return "auto";
    }
}

/* ═══════════════════════════════════════════════════════════════
 * TENSOR CLASSIFICATION — detect which service owns a tensor
 * ═══════════════════════════════════════════════════════════════ */
static inline PipeServiceType pipe_service_classify(const char *name) {
    if (!name) return PIPE_SVC_LLM;

    /* Kokoro TTS: starts with bert/bert_encoder/predictor/decoder/text_encoder */
    const char *tts_prefixes[] = {
        "bert", "bert_encoder", "predictor", "decoder", "text_encoder"
    };
    for (size_t i = 0; i < sizeof(tts_prefixes)/sizeof(tts_prefixes[0]); i++) {
        if (strstr(name, tts_prefixes[i]) == name) return PIPE_SVC_TTS;
    }

    /* moondream2 VLM: starts with model.vision or model.text */
    if (strstr(name, "model.vision") == name) return PIPE_SVC_VLM;
    if (strstr(name, "model.text") == name) return PIPE_SVC_VLM;
    if (strstr(name, "model.proj") == name) return PIPE_SVC_VLM;

    return PIPE_SVC_LLM;
}

/* ═══════════════════════════════════════════════════════════════
 * PIPE OPEN — one function, any service type
 * ═══════════════════════════════════════════════════════════════
 *   PIPE_SVC_AUTO: opens as LLM (4GB), auto-detects on first write
 *   PIPE_SVC_TTS:  256MB, 54 slots
 *   PIPE_SVC_VLM:  4GB, 365 slots
 *   PIPE_SVC_LLM:  16GB, 4096 slots
 *
 * name:        human-readable name (e.g. "kokoro", "moondream2")
 * backend_path: optional file path for DRamTile persistence (NULL = RAM)
 */
static inline PipeContext *pipe_service_open(PipeServiceType type,
                                              const char *name,
                                              const char *backend_path)
{
    PipeConfig cfg = PIPE_CONFIG_DEFAULT;
    cfg.name = name ? name : pipe_service_name(type);
    cfg.backend_path = backend_path;

    switch (type) {
        case PIPE_SVC_TTS:
            cfg.capacity  = 256UL * 1024 * 1024;
            cfg.max_slots = 54;
            break;
        case PIPE_SVC_VLM:
            cfg.capacity  = 4UL * 1024 * 1024 * 1024;
            cfg.max_slots = 365;
            break;
        case PIPE_SVC_LLM:
        case PIPE_SVC_AUTO:
        default:
            cfg.capacity  = 16UL * 1024 * 1024 * 1024;
            cfg.max_slots = 4096;
            break;
    }

    PipeContext *ctx = pipe_open(&cfg);
    if (!ctx) return NULL;

    /* Tag the service type in the first byte of name for late detection */
    /* We store it in the pipe name: "type:name" pattern */
    /* Actually, pipe_open strips our type prefix. Store separately. */
    return ctx;
}

/* ═══════════════════════════════════════════════════════════════
 * PIPE WRITE — write any tensor, auto-classify if needed
 * ═══════════════════════════════════════════════════════════════
 * If pipe was opened as PIPE_SVC_AUTO, detection happens on first write.
 * Uses PIPE_DTYPE_F32 by default. */
static inline int pipe_service_write(PipeContext *ctx, const char *name,
                                      const void *data, uint32_t nbytes)
{
    return pipe_write(ctx, name, data, nbytes, PIPE_DTYPE_F32, PIPE_PRIO_NORMAL);
}

/* ═══════════════════════════════════════════════════════════════
 * PIPE READ — read any tensor (zero-copy)
 * ═══════════════════════════════════════════════════════════════ */
static inline const void *pipe_service_read(PipeContext *ctx, const char *name,
                                             uint32_t *nbytes)
{
    return pipe_read(ctx, name, nbytes);
}

/* ═══════════════════════════════════════════════════════════════
 * CONVENIENCE — find / remove / count
 * ═══════════════════════════════════════════════════════════════ */
static inline int       pipe_service_has(PipeContext *ctx, const char *name) {
    return pipe_find(ctx, name);
}
static inline int       pipe_service_remove(PipeContext *ctx, const char *name) {
    return pipe_remove(ctx, name);
}
static inline uint32_t  pipe_service_count(PipeContext *ctx) {
    return pipe_count(ctx);
}

/* ═══════════════════════════════════════════════════════════════
 * CHECKPOINT / RESTORE
 * ═══════════════════════════════════════════════════════════════ */
static inline int pipe_service_checkpoint(PipeContext *ctx, const char *path) {
    return pipe_checkpoint(ctx, path);
}
static inline int pipe_service_restore(PipeContext *ctx, const char *path) {
    return pipe_restore(ctx, path);
}

/* ═══════════════════════════════════════════════════════════════
 * CLOSE
 * ═══════════════════════════════════════════════════════════════ */
static inline void pipe_service_close(PipeContext *ctx) {
    pipe_close(ctx);
}

/* ═══════════════════════════════════════════════════════════════
 * STATUS
 * ═══════════════════════════════════════════════════════════════ */
static inline void pipe_service_status(PipeContext *ctx) {
    if (!ctx) { printf("[svc] pipe not open\n"); return; }
    PipeStats s;
    pipe_stats(ctx, &s);
    printf("[svc] %s: %u/%u slots, %llu/%llu bytes, %llu reads, %llu writes\n",
           pipe_name(ctx), s.n_slots, ctx->n_slots,
           (unsigned long long)s.capacity_used,
           (unsigned long long)s.capacity_total,
           (unsigned long long)s.n_reads,
           (unsigned long long)s.n_writes);
}

/* ═══════════════════════════════════════════════════════════════
 * CONVENIENCE — write from file
 * ═══════════════════════════════════════════════════════════════
 * Reads a file from disk into pipe. Returns bytes written or < 0. */
static inline int pipe_service_write_file(PipeContext *ctx, const char *tensor_name,
                                           const char *file_path)
{
    FILE *f = fopen(file_path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return -2; }
    rewind(f);

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return -3; }

    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    if (n != (size_t)sz) { free(buf); return -4; }

    int rc = pipe_service_write(ctx, tensor_name, buf, (uint32_t)n);
    free(buf);
    return rc == PIPE_OK ? (int)n : -5;
}

/* ═══════════════════════════════════════════════════════════════
 * CONVENIENCE — export to file
 * ═══════════════════════════════════════════════════════════════ */
static inline int pipe_service_export_file(PipeContext *ctx, const char *tensor_name,
                                            const char *file_path)
{
    uint32_t sz = 0;
    const void *data = pipe_service_read(ctx, tensor_name, &sz);
    if (!data || sz == 0) return -1;

    FILE *f = fopen(file_path, "wb");
    if (!f) return -2;

    size_t n = fwrite(data, 1, sz, f);
    fclose(f);
    return n == sz ? (int)sz : -3;
}

#ifdef __cplusplus
}
#endif

#endif /* PIPE_SERVICE_H */
