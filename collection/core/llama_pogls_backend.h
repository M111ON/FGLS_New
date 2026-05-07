/*
 * llama_pogls_backend.h  —  S83-B3/B4
 * Public API for llama_pogls_backend.c
 */
#ifndef LLAMA_POGLS_BACKEND_H
#define LLAMA_POGLS_BACKEND_H

#include <stdint.h>
#include "core/pogls_checksum.h"
#include "geo_headers/pogls_scanner.h"
#include "geo_headers/pogls_compress.h"
#include "geo_headers/pogls_file_index.h"
#include "geo_headers/pogls_reconstruct.h"
#include "geo_headers/pogls_stream.h"
#include "geo_headers/pogls_model_index.h"

/* ── error codes ─────────────────────────────────────────────────── */
#define POGLS_BACK_OK           0
#define POGLS_BACK_ERR_NULL    -1
#define POGLS_BACK_ERR_PARSE   -2
#define POGLS_BACK_ERR_IO      -3
#define POGLS_BACK_ERR_LAYER   -4
#define POGLS_BACK_ERR_WINSZ   -5
#define POGLS_BACK_ERR_CB      -6

/* ── callback types ──────────────────────────────────────────────── */
typedef int (*pogls_layer_cb_t)(uint32_t layer_id, uint32_t total,
                                 const ModelLayerRecord *rec,
                                 void *user_data);

/* ── config ──────────────────────────────────────────────────────── */
typedef struct {
    const char      *gguf_path;
    const char      *model_name;
    void            *ram_window;
    uint64_t         ram_window_size;
    pogls_layer_cb_t  layer_cb;
    void            *cb_user_data;
    uint32_t         layer_start;
    uint32_t         layer_end;
} PoglsBackendCfg;

/* ── stats ───────────────────────────────────────────────────────── */
typedef struct {
    uint32_t layers_read;
    uint32_t layers_skipped;
    uint64_t bytes_read;
    uint64_t peak_layer_bytes;
} PoglsBackendStats;

/* ── API ─────────────────────────────────────────────────────────── */
int  pogls_backend_run      (const PoglsBackendCfg *cfg,
                              PoglsBackendStats *stats);

int  pogls_backend_preflight(const char *gguf_path, const char *model_name,
                              uint64_t window_size,
                              uint64_t *out_required_bytes,
                              uint32_t *out_total_layers);

void pogls_backend_print_stats(const PoglsBackendStats *st);

/* forward decl needed by runner */
int gguf_parse_to_model_index(const char *gguf_path, ModelIndex *mi,
                               const char *model_name);

#endif /* LLAMA_POGLS_BACKEND_H */
