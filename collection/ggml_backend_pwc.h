#pragma once
// ggml_backend_pwc.h — PWC Weight Backend for llama.cpp
// Pattern: map-once, decode-on-demand, no intermediate files
// Requires: pogls_weight_container.h, ggml-backend.h

#include "ggml.h"
#include "ggml-backend.h"
#include "pogls_weight_container.h"
#include <stdint.h>
#include <string.h>

// ─── Backend context ──────────────────────────────────────────────
typedef struct {
    PWCReader*  pwc;        // mmap'd PWC file handle
    char        path[512];
} pwc_backend_ctx_t;

// ─── Buffer context (one per tensor) ─────────────────────────────
typedef struct {
    pwc_backend_ctx_t* bctx;
    uint32_t    tensor_idx;   // index into PWC tensor table
    size_t      nbytes;
    void*       decoded;      // NULL until first access (lazy)
} pwc_buffer_ctx_t;

// ─── Forward declarations ─────────────────────────────────────────
static const char* pwc_backend_name(ggml_backend_t backend);
static void        pwc_backend_free(ggml_backend_t backend);
static ggml_backend_buffer_t pwc_alloc_buffer(ggml_backend_buffer_type_t buft, size_t size);
static bool        pwc_backend_graph_compute(ggml_backend_t backend, struct ggml_cgraph* cgraph);

// ─── Buffer interface ─────────────────────────────────────────────
static void pwc_buffer_free(ggml_backend_buffer_t buf) {
    pwc_buffer_ctx_t* bctx = (pwc_buffer_ctx_t*)buf->context;
    if (bctx->decoded) free(bctx->decoded);
    free(bctx);
}

// Called when llama.cpp needs actual tensor data
static void* pwc_buffer_get_base(ggml_backend_buffer_t buf) {
    pwc_buffer_ctx_t* bctx = (pwc_buffer_ctx_t*)buf->context;
    if (!bctx->decoded) {
        // Lazy decode: seek → decode → cache in memory
        bctx->decoded = malloc(bctx->nbytes);
        pwc_decode_tensor(bctx->bctx->pwc, bctx->tensor_idx,
                          bctx->decoded, bctx->nbytes);
    }
    return bctx->decoded;
}

static struct ggml_backend_buffer_i pwc_buffer_iface = {
    /* .free_buffer    = */ pwc_buffer_free,
    /* .get_base       = */ pwc_buffer_get_base,
    /* .init_tensor    = */ NULL,
    /* .memset_tensor  = */ NULL,
    /* .set_tensor     = */ NULL,
    /* .get_tensor     = */ NULL,
    /* .cpy_tensor     = */ NULL,
    /* .clear          = */ NULL,
    /* .reset          = */ NULL,
};

// ─── Buffer type interface ────────────────────────────────────────
// Called during model load: llama asks "allocate space for this tensor"
static ggml_backend_buffer_t pwc_buft_alloc_buffer(
        ggml_backend_buffer_type_t buft, size_t size) {
    pwc_buffer_ctx_t* bctx = calloc(1, sizeof(pwc_buffer_ctx_t));
    bctx->bctx    = (pwc_backend_ctx_t*)buft->context;
    bctx->nbytes  = size;
    bctx->decoded = NULL;  // lazy — don't decode yet
    // tensor_idx resolved in init_tensor via name lookup
    return ggml_backend_buffer_init(buft, pwc_buffer_iface, bctx, size);
}

static const char* pwc_buft_name(ggml_backend_buffer_type_t buft) {
    return "PWC";
}

static bool pwc_buft_supports_op(ggml_backend_buffer_type_t buft,
                                  const struct ggml_tensor* op) {
    return true; // CPU-compatible buffer, all ops supported
}

static struct ggml_backend_buffer_type_i pwc_buft_iface = {
    /* .get_name         = */ pwc_buft_name,
    /* .alloc_buffer     = */ pwc_buft_alloc_buffer,
    /* .get_alignment    = */ NULL,
    /* .get_max_size     = */ NULL,
    /* .get_alloc_size   = */ NULL,
    /* .supports_op      = */ pwc_buft_supports_op,
    /* .is_host          = */ NULL,
};

// ─── Backend interface ────────────────────────────────────────────
static const char* pwc_backend_name(ggml_backend_t backend) { return "PWC"; }

static void pwc_backend_free(ggml_backend_t backend) {
    pwc_backend_ctx_t* ctx = (pwc_backend_ctx_t*)backend->context;
    pwc_close(ctx->pwc);
    free(ctx);
    free(backend);
}

// PWC doesn't compute — delegates to CPU backend
static ggml_status pwc_backend_graph_compute(ggml_backend_t backend,
                                              struct ggml_cgraph* cgraph) {
    return GGML_STATUS_SUCCESS;
}

static struct ggml_backend_i pwc_backend_iface = {
    /* .get_name            = */ pwc_backend_name,
    /* .free                = */ pwc_backend_free,
    /* .graph_plan_create   = */ NULL,
    /* .graph_plan_free     = */ NULL,
    /* .graph_plan_update   = */ NULL,
    /* .graph_plan_compute  = */ NULL,
    /* .graph_compute       = */ pwc_backend_graph_compute,
    /* .supports_op         = */ NULL,
    /* .supports_buft       = */ NULL,
    /* .offload_op          = */ NULL,
    /* .event_new           = */ NULL,
    /* .event_free          = */ NULL,
    /* .event_record        = */ NULL,
    /* .event_wait          = */ NULL,
    /* .event_synchronize   = */ NULL,
};

// ─── Public API ───────────────────────────────────────────────────

// Init: open PWC file, mmap index
ggml_backend_t ggml_backend_pwc_init(const char* pwc_path) {
    pwc_backend_ctx_t* ctx = calloc(1, sizeof(pwc_backend_ctx_t));
    strncpy(ctx->path, pwc_path, 511);
    ctx->pwc = pwc_open(pwc_path);  // mmap + read 11.6KB index only
    if (!ctx->pwc) { free(ctx); return NULL; }

    ggml_backend_t backend = calloc(1, sizeof(struct ggml_backend));
    backend->iface   = pwc_backend_iface;
    backend->context = ctx;
    return backend;
}

// Buffer type: used by llama.cpp to allocate weight tensors
ggml_backend_buffer_type_t ggml_backend_pwc_buffer_type(ggml_backend_t backend) {
    static struct ggml_backend_buffer_type buft;
    buft.iface   = pwc_buft_iface;
    buft.context = backend->context;
    return &buft;
}

// Resolve tensor name → PWC index (call after model graph is built)
// llama.cpp names: "blk.0.attn_q.weight", etc.
bool pwc_backend_bind_tensor(ggml_backend_buffer_t buf,
                              const char* tensor_name) {
    pwc_buffer_ctx_t* bctx = (pwc_buffer_ctx_t*)buf->context;
    int idx = pwc_find_tensor(bctx->bctx->pwc, tensor_name);
    if (idx < 0) return false;
    bctx->tensor_idx = (uint32_t)idx;
    return true;
}
