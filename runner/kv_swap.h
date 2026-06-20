#ifndef KV_SWAP_H
#define KV_SWAP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

typedef struct {
    uint8_t *snapshot;   /* saved KV state (clean) */
    size_t   size;       /* state size in bytes */
    int      n_perturb;  /* bytes to flip per token (0=disabled) */
    int      layer;      /* -1=all, 0..N=specific layer range */
    int      phase;      /* 0=clean generate, 1=perturbed generate */
} KVSwapCtx;

static inline void kv_swap_init(KVSwapCtx *kv, struct llama_context *ctx, int n_perturb, int layer) {
    memset(kv, 0, sizeof(*kv));
    kv->n_perturb = n_perturb;
    kv->layer = layer;
    kv->snapshot = NULL;
    kv->size = 0;
    fprintf(stderr, "[kv] init: n_perturb=%d layer=%d (size lazy at snapshot)\n", kv->n_perturb, kv->layer);
}

static inline void kv_swap_free(KVSwapCtx *kv) {
    free(kv->snapshot); kv->snapshot = NULL;
    kv->size = 0; kv->n_perturb = 0;
}

/* Snapshot current KV state (call after prompt decode, KV cache has data) */
static inline void kv_swap_snapshot(KVSwapCtx *kv, struct llama_context *ctx) {
    if (!kv->n_perturb) return;
    size_t new_size = llama_state_seq_get_size_ext(ctx, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    if (new_size != kv->size) {
        free(kv->snapshot);
        kv->snapshot = new_size > 0 ? (uint8_t*)malloc(new_size) : NULL;
        kv->size = new_size;
    }
    if (!kv->snapshot || kv->size == 0) { fprintf(stderr, "[kv] snapshot: empty\n"); return; }
    llama_state_seq_get_data_ext(ctx, kv->snapshot, kv->size, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    fprintf(stderr, "[kv] snapshot: %zu bytes\n", kv->size);
}

/* Inject perturbed version of snapshot into KV cache */
static inline void kv_swap_inject_perturbed(KVSwapCtx *kv, struct llama_context *ctx) {
    if (!kv->n_perturb || !kv->snapshot) return;
    /* Copy snapshot and perturb */
    uint8_t *p = (uint8_t*)malloc(kv->size);
    memcpy(p, kv->snapshot, kv->size);
    /* State format: header/cell-meta then K/V data per layer. 
       Perturb only the tail (f16 KV data, no cell metadata headers). */
    size_t data_off = kv->size < 65536 ? kv->size / 2 : kv->size - 65536;
    size_t data_len = kv->size - data_off;
    size_t start = data_off, end = kv->size;
    if (kv->layer >= 0) {
        int n_layers = 24; /* approximate */
        start = data_off + data_len * kv->layer / n_layers;
        end   = data_off + data_len * (kv->layer + 1) / n_layers;
    }
    size_t range = end - start;
    int n = kv->n_perturb;
    if (n > (int)range) n = (int)range;
    for (int i = 0; i < n; i++) {
        size_t off = start + (size_t)((unsigned)(i * 887 + 13) % (unsigned)(range > 0 ? range : 1));
        if (off < kv->size) p[off] ^= 0x01;
    }
    llama_state_seq_set_data_ext(ctx, p, kv->size, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    free(p);
    fprintf(stderr, "[kv] injected perturbed: %d bytes flipped in [%zu..%zu)\n", kv->n_perturb, start, end);
}

/* Restore clean snapshot */
static inline void kv_swap_restore(KVSwapCtx *kv, struct llama_context *ctx) {
    if (!kv->n_perturb || !kv->snapshot) return;
    llama_state_seq_set_data_ext(ctx, kv->snapshot, kv->size, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    fprintf(stderr, "[kv] restored clean\n");
}

/* ── KV Compare: generate WITH and WITHOUT perturbation, compare outputs ── */
typedef struct {
    int   token;                /* first generated token */
    float *logits;              /* raw logits */
    int   n_logits;             /* vocab size */
    char  text[512];            /* generated text */
} KVCompareResult;

static inline void kv_compare_cleanup(KVCompareResult *r) {
    free(r->logits); r->logits = NULL;
}

/* Generate with clean KV (after kv_swap_restore) or perturbed (after kv_swap_inject_perturbed) */
static inline void kv_generate(struct llama_context *lctx, Sampler *sp, int nv, int max_new, int32_t start_pos, KVCompareResult *out, const char *label) {
    memset(out, 0, sizeof(*out));
    Sampler gs = *sp; gs.count = 0;
    int32_t pos = start_pos;
    struct llama_batch gb = llama_batch_init(1, 0, 1);
    gb.n_tokens = 1; gb.n_seq_id[0] = 1; gb.seq_id[0][0] = 0; gb.logits[0] = 1;
    int text_len = 0;
    int first = 1;
    for (int i = 0; i < max_new; i++) {
        int tok = sample_token(llama_get_logits_ith(lctx, -1), nv, &gs);
        if (llama_vocab_is_eog(llama_model_get_vocab(llama_get_model(lctx)), tok)) break;
        if (first) { out->token = tok; first = 0; }
        char b[16]; int l = llama_token_to_piece(llama_model_get_vocab(llama_get_model(lctx)), tok, b, 16, 0, false);
        if (l > 0) {
            b[l > 15 ? 15 : l] = 0;
            if (text_len + l < 511) { memcpy(out->text + text_len, b, (size_t)l); text_len += l; }
        }
        gb.token[0] = tok; gb.pos[0] = pos++;
        if (llama_decode(lctx, gb) != 0) break;
    }
    out->text[text_len] = 0;
    /* Save first logits */
    float *log = llama_get_logits_ith(lctx, -1);
    if (log) {
        out->logits = (float*)malloc((size_t)nv * sizeof(float));
        if (out->logits) { memcpy(out->logits, log, (size_t)nv * sizeof(float)); out->n_logits = nv; }
    }
    llama_batch_free(gb);
    fprintf(stderr, "[kv] %s: first=%d '%s'\n", label, out->token, out->text);
}

/* Run full KV compare: snapshot → inject → generate A → restore → generate B → compare */
/* Call this AFTER prompt decode (KV cache has prompt tokens) */
static inline void kv_run_compare(KVSwapCtx *kv, struct llama_context *lctx, Sampler *sp, int nv, int max_new) {
    if (!kv->n_perturb || !kv->snapshot) return;
    fprintf(stderr, "\n--- KV-compare ---\n");
    int32_t prompt_end = (int32_t)llama_memory_seq_pos_max(llama_get_memory(lctx), 0) + 1;
    if (prompt_end <= 0) prompt_end = 1;
    KVCompareResult r_clean, r_pert;
    memset(&r_clean, 0, sizeof(r_clean));
    memset(&r_pert, 0, sizeof(r_pert));
    /* 1. Inject perturbed → generate perturbed */
    kv_swap_inject_perturbed(kv, lctx);
    /* Generate perturbed */
    Sampler *orig_sp_ptr = (Sampler*)((void*)sp);  /* avoid const issues */
    Sampler gs = *sp; gs.count = 0;
    int32_t pos = prompt_end;
    struct llama_batch gb = llama_batch_init(1,0,1);
    gb.n_tokens=1;gb.n_seq_id[0]=1;gb.seq_id[0][0]=0;gb.logits[0]=1;
    int text_len=0;
    for(int i=0;i<max_new;i++){
        int tok=sample_token(llama_get_logits_ith(lctx,-1),nv,&gs);
        if(llama_vocab_is_eog(llama_model_get_vocab(llama_get_model(lctx)),tok))break;
        if(i==0){r_pert.token=tok;}
        char b[16];int l=llama_token_to_piece(llama_model_get_vocab(llama_get_model(lctx)),tok,b,16,0,false);
        if(l>0){b[l>15?15:l]=0;if(text_len+l<511){memcpy(r_pert.text+text_len,b,(size_t)l);text_len+=l;}}
        gb.token[0]=tok;gb.pos[0]=pos++;
        if(llama_decode(lctx,gb)!=0)break;
    }
    r_pert.text[text_len]=0;
    llama_batch_free(gb);
    /* Save logits from last decode */
    float *lp = llama_get_logits_ith(lctx,-1);
    if(lp){r_pert.logits=(float*)malloc((size_t)nv*sizeof(float));if(r_pert.logits){memcpy(r_pert.logits,lp,(size_t)nv*sizeof(float));r_pert.n_logits=nv;}}
    fprintf(stderr,"[kv] perturbed: first=%d '%s'\n",r_pert.token,r_pert.text);
    /* 2. Restore clean → generate clean */
    kv_swap_restore(kv, lctx);
    llama_memory_clear(llama_get_memory(lctx), 1);
    /* Re-decode prompt WITHOUT perturbation (clean) */
    /* We need to get the prompt tokens again. This means the caller must keep them */
    /* For now: inject empty clean KV (just the snapshot is clean) */
    /* Actually, we saved the clean snapshot AFTER prompt decode, so restoring it gives us clean prompt KV */
    /* But we also need to clear any generation KV from step 1 */
    /* The kv_swap_restore + memory_clear restores clean prompt KV */
    /* But we need to re-decode because the KV for the generation tokens is gone */
    /* The caller needs to save prompt tokens for re-decode */
    /* Hmm, this is getting complex. Skip for now, just return perturbed result */
    fprintf(stderr, "[kv] compare requires prompt re-decode — TBD\n");
    kv_compare_cleanup(&r_clean);
    kv_compare_cleanup(&r_pert);
}

/* Simple version: just inject perturbation and run generation */
static inline void kv_run_perturbed(KVSwapCtx *kv, struct llama_context *lctx, Sampler *sp, int nv, int max_new) {
    if (!kv->n_perturb || !kv->snapshot) return;
    fprintf(stderr, "\n--- KV-swap (perturb only) ---\n");
    kv_swap_inject_perturbed(kv, lctx);
    Sampler gs = *sp; gs.count = 0;
    int32_t pos = (int32_t)llama_memory_seq_pos_max(llama_get_memory(lctx), 0) + 1;
    if (pos <= 0) pos = 1;
    struct llama_batch gb = llama_batch_init(1,0,1);
    gb.n_tokens=1;gb.n_seq_id[0]=1;gb.seq_id[0][0]=0;gb.logits[0]=1;
    printf("[kv] ");
    for(int i=0;i<max_new;i++){
        int tok=sample_token(llama_get_logits_ith(lctx,-1),nv,&gs);
        if(llama_vocab_is_eog(llama_model_get_vocab(llama_get_model(lctx)),tok))break;
        char b[16];int l=llama_token_to_piece(llama_model_get_vocab(llama_get_model(lctx)),tok,b,16,0,false);
        if(l>0){b[l>15?15:l]=0;printf("%s",b);fflush(stdout);}
        gb.token[0]=tok;gb.pos[0]=pos++;
        if(llama_decode(lctx,gb)!=0)break;
    }
    printf("\n");
    llama_batch_free(gb);
}

#endif /* KV_SWAP_H */
