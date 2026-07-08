/*
 * pipe_vlm.h — moondream2 VLM Integration via Pipe ABI
 *
 * Wraps moondream2 (starmie-v1) tensor I/O through the Pipe/DRamTile system.
 * Architecture (from safetensors header + vision.py/text.py):
 *   Vision encoder:  27 layers, enc_dim=1152, enc_ff_dim=4304, enc_n_heads=16
 *   Text decoder:    24 layers, hidden=2048, ff=8192, n_heads=32
 *   Total: ~365 tensors (27 vision blocks × 7 + 24 text blocks × 7 + 5 misc)
 *
 * Usage:
 *   PipeContext *vlm = pipe_open_vlm("moondream2", NULL);
 *   pipe_write_vlm_tensor(vlm, "model.vision.blocks.0.attn.qkv.weight", data, sz);
 *   const void *w = pipe_read_vlm_tensor(vlm, "model.text.wte");
 */

#ifndef PIPE_VLM_H
#define PIPE_VLM_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "pipe_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════
 * MOONDREAM2 VLM ARCHITECTURE CONSTANTS
 * ═══════════════════════════════════════════════════════════════ */

/* Vision encoder (SigLIP-like, 27 layers) */
#define VLM_VISION_ENC_DIM      1152
#define VLM_VISION_ENC_FF_DIM   4304
#define VLM_VISION_N_HEADS      16
#define VLM_VISION_N_LAYERS     27
#define VLM_VISION_PATCH_SIZE   14
#define VLM_VISION_CROP_SIZE    378
#define VLM_VISION_MAX_CROPS    4

/* Text decoder (24-layer transformer) */
#define VLM_TEXT_HIDDEN         2048
#define VLM_TEXT_FF             8192
#define VLM_TEXT_N_HEADS        32
#define VLM_TEXT_N_LAYERS       24
#define VLM_TEXT_VOCAB          51200
#define VLM_TEXT_MAX_POS        2048
#define VLM_TEXT_N_KV_HEADS     32

/* Projection */
#define VLM_PROJ_INNER_DIM      8192
#define VLM_PROJ_OUT_DIM        2048

/* Total tensors (~365) */
#define VLM_N_TENSORS_VISION    (VLM_VISION_N_LAYERS * 7 + 5)  /* 194 */
#define VLM_N_TENSORS_TEXT      (VLM_TEXT_N_LAYERS * 7 + 3)    /* 171 */
#define VLM_N_TENSORS           (VLM_N_TENSORS_VISION + VLM_N_TENSORS_TEXT)

/* ═══════════════════════════════════════════════════════════════
 * MOONDREAM2 TENSOR NAME PREFIXES
 * ═══════════════════════════════════════════════════════════════
 * From model.safetensors header analysis (BF16):
 *   model.vision.blocks.{0..26}.{attn/mlp/ln}.*
 *   model.text.blocks.{0..23}.{attn/mlp/ln}.*
 * ═══════════════════════════════════════════════════════════════ */

#define VLM_PREFIX_VISION       "model.vision"
#define VLM_PREFIX_TEXT         "model.text"
#define VLM_PREFIX_VISION_BLK   "model.vision.blocks."
#define VLM_PREFIX_TEXT_BLK     "model.text.blocks."

/* ═══════════════════════════════════════════════════════════════
 * SID CONFIG — VLM uses 8 faces (from sid_conf_vlm)
 * Based on HANDOFF_CTD: max_shift=7.85, Y6 lv1=84%
 *
 * NOTE: In production, #include "sid.h" and use SIDArchConfig.
 * This header defines a local SID config struct to avoid dragging
 * the entire collection dependency tree into simple pipe apps.
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    int         n_faces;
    int         use_tri;
    const void *face_order;
} VLM_SIDConfig;

static inline VLM_SIDConfig vlm_sid_config(void) {
    VLM_SIDConfig c = { .n_faces = 8, .use_tri = 0, .face_order = NULL };
    return c;
}

/* ═══════════════════════════════════════════════════════════════
 * VLM TENSOR NAME CLASSIFICATION
 * ═══════════════════════════════════════════════════════════════ */
/* Returns 1 if name is a vision tensor, 0 for text, -1 for unknown */
static inline int vlm_classify_tensor(const char *name) {
    if (!name) return -1;
    if (strstr(name, "model.vision") == name) return 1;
    if (strstr(name, "model.text") == name) return 0;
    return -1;
}

/* Returns 1 if name matches moondream2 tensor pattern */
static inline int vlm_is_vlm_tensor(const char *name) {
    return vlm_classify_tensor(name) >= 0;
}

/* ═══════════════════════════════════════════════════════════════
 * PIPE OPEN — Create a VLM-specific pipe
 * ═══════════════════════════════════════════════════════════════
 * If config is NULL, uses VLM defaults:
 *   name = "moondream2"
 *   capacity = 4GB (moondream2 safetensors is ~3.85GB)
 *   max_slots = VLM_N_TENSORS
 *   backend_path = optional file path for persistence
 * ═══════════════════════════════════════════════════════════════ */

static inline PipeContext *pipe_open_vlm(const char *name, const char *backend_path) {
    PipeConfig cfg = PIPE_CONFIG_DEFAULT;
    cfg.name = name ? name : "moondream2";
    cfg.capacity = 4UL * 1024 * 1024 * 1024;  /* 4 GB */
    cfg.max_slots = VLM_N_TENSORS;
    cfg.backend_path = backend_path;
    return pipe_open(&cfg);
}

/* ═══════════════════════════════════════════════════════════════
 * PIPE WRITE TENSOR — Write a named tensor to VLM pipe
 * ═══════════════════════════════════════════════════════════════ */
static inline int pipe_write_vlm_tensor(PipeContext *ctx, const char *name,
                                         const void *data, uint32_t nbytes)
{
    return pipe_write(ctx, name, data, nbytes, PIPE_DTYPE_F32, PIPE_PRIO_NORMAL);
}

/* ═══════════════════════════════════════════════════════════════
 * PIPE READ TENSOR — Read a named tensor from VLM pipe (zero-copy)
 * ═══════════════════════════════════════════════════════════════ */
static inline const void *pipe_read_vlm_tensor(PipeContext *ctx, const char *name,
                                                uint32_t *nbytes)
{
    return pipe_read(ctx, name, nbytes);
}

/* ═══════════════════════════════════════════════════════════════
 * VISION ENCODER ITERATOR — Iterate over all vision blocks
 * ═══════════════════════════════════════════════════════════════
 * Calls callback for every tensor in a vision block.
 * block_idx: 0..VLM_VISION_N_LAYERS-1
 * sub_tensors: ln1, attn.qkv, attn.proj, ln2, mlp.fc1, mlp.fc2 (6 per block) */
typedef int (*vlm_vision_block_cb)(int block_idx, const char *sub_name,
                                    void *user);

static inline int vlm_foreach_vision_block(int block_idx,
                                            vlm_vision_block_cb cb, void *user)
{
    if (block_idx < 0 || block_idx >= VLM_VISION_N_LAYERS) return -1;
    const char *subs[] = {
        "ln1.weight", "ln1.bias",
        "attn.qkv.weight", "attn.qkv.bias",
        "attn.proj.weight", "attn.proj.bias",
        "ln2.weight", "ln2.bias",
        "mlp.fc1.weight", "mlp.fc1.bias",
        "mlp.fc2.weight", "mlp.fc2.bias"
    };
    char name[128];
    int count = 0;
    for (size_t i = 0; i < sizeof(subs)/sizeof(subs[0]); i++) {
        snprintf(name, sizeof(name), "model.vision.blocks.%d.%s", block_idx, subs[i]);
        if (cb(block_idx, name, user) == 0) count++;
    }
    return count;
}

/* ═══════════════════════════════════════════════════════════════
 * TEXT DECODER ITERATOR — Iterate over all text blocks
 * ═══════════════════════════════════════════════════════════════
 * block_idx: 0..VLM_TEXT_N_LAYERS-1
 * sub_tensors: ln, attn.qkv, attn.proj, mlp.fc1, mlp.fc2 (5 per block) */
typedef int (*vlm_text_block_cb)(int block_idx, const char *sub_name,
                                  void *user);

static inline int vlm_foreach_text_block(int block_idx,
                                          vlm_text_block_cb cb, void *user)
{
    if (block_idx < 0 || block_idx >= VLM_TEXT_N_LAYERS) return -1;
    const char *subs[] = {
        "ln.weight", "ln.bias",
        "attn.qkv.weight", "attn.qkv.bias",
        "attn.proj.weight", "attn.proj.bias",
        "mlp.fc1.weight", "mlp.fc1.bias",
        "mlp.fc2.weight", "mlp.fc2.bias"
    };
    char name[128];
    int count = 0;
    for (size_t i = 0; i < sizeof(subs)/sizeof(subs[0]); i++) {
        snprintf(name, sizeof(name), "model.text.blocks.%d.%s", block_idx, subs[i]);
        if (cb(block_idx, name, user) == 0) count++;
    }
    return count;
}

/* ═══════════════════════════════════════════════════════════════
 * PIPE STATUS — Print VLM pipe stats
 * ═══════════════════════════════════════════════════════════════ */
static inline void pipe_vlm_status(PipeContext *ctx) {
    if (!ctx) { printf("[vlm] pipe not open\n"); return; }
    PipeStats s;
    pipe_stats(ctx, &s);
    printf("[vlm] %s: %u/%u slots, %llu/%llu bytes, %llu reads, %llu writes\n",
           pipe_name(ctx), s.n_slots, ctx->n_slots,
           (unsigned long long)s.capacity_used,
           (unsigned long long)s.capacity_total,
           (unsigned long long)s.n_reads,
           (unsigned long long)s.n_writes);
}

#ifdef __cplusplus
}
#endif

#endif /* PIPE_VLM_H */
