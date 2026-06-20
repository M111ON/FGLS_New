#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <windows.h>
#include <psapi.h>
#include "llama.h"

#pragma comment(lib, "psapi.lib")

static size_t get_mem_mb(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return pmc.WorkingSetSize / (1024 * 1024);
    return 0;
}

static int tokenize(const struct llama_vocab *v, const char *t, int len, int **out, int add, int parse) {
    int need = llama_tokenize(v, t, len, NULL, 0, add, parse);
    if (need < 0) need = -need;
    if (need == 0) { *out = NULL; return 0; }
    *out = (int*)malloc((size_t)need * 4);
    int got = llama_tokenize(v, t, len, *out, need, add, parse);
    return got < 0 ? -got : got;
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (char*)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[rd] = 0;
    *out_len = rd;
    return buf;
}

static size_t perturb_kv(uint8_t *buf, size_t sz, int stride) {
    if (sz < 36) return 0;
    uint32_t n_cells = *(uint32_t*)(buf + 12);
    int cell_end = 16;
    for (uint32_t ci = 0; ci < n_cells && cell_end + 8 <= (int)sz; ci++) {
        uint32_t n_seq_id = *(uint32_t*)(buf + cell_end + 4);
        cell_end += 8 + (int)(n_seq_id * 4);
    }
    int data_hdr = cell_end;
    if (data_hdr + 8 > (int)sz) return 0;
    uint32_t n_layer = *(uint32_t*)(buf + data_hdr + 4);
    uint32_t n_cells_kv = *(uint32_t*)(buf + 12);

    int off = data_hdr + 8;
    size_t total = 0;
    for (uint32_t li = 0; li < n_layer && off + 12 <= (int)sz; li++) {
        uint64_t k_sz_row = *(uint64_t*)(buf + off + 4);
        size_t k_data_sz = (size_t)n_cells_kv * (size_t)k_sz_row;
        int k_data_off = off + 12;
        int v_off = k_data_off + (int)k_data_sz;
        uint64_t v_sz_row = *(uint64_t*)(buf + v_off + 4);
        size_t v_data_sz = (size_t)n_cells_kv * (size_t)v_sz_row;
        int v_data_off = v_off + 12;
        for (size_t b = 0; b < k_data_sz; b += stride) {
            buf[k_data_off + (int)b] ^= 0x01; total++;
        }
        for (size_t b = 0; b < v_data_sz; b += stride) {
            buf[v_data_off + (int)b] ^= 0x02; total++;
        }
        off = v_data_off + (int)v_data_sz;
    }
    return total;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { fprintf(stderr, "Usage: %s model.gguf [--file FILE] [--ctx N] [--stride N]\n", argv[0]); return 1; }

    char *file_path = "I:\\FGLS_new\\runner\\session-ses_1416.md";
    int n_ctx = 16384;
    int stride = 64;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--file") == 0 && i+1 < argc) file_path = argv[++i];
        else if (strcmp(argv[i], "--ctx") == 0 && i+1 < argc) n_ctx = atoi(argv[++i]);
        else if (strcmp(argv[i], "--stride") == 0 && i+1 < argc) stride = atoi(argv[++i]);
    }

    printf("=== KV Long-Context Test ===\n");
    printf("Config: n_ctx=%d stride=%d file=%s\n", n_ctx, stride, file_path ? file_path : "(none)");
    printf("Step 0 (idle): %zu MB\n", get_mem_mb());

    llama_backend_init();
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 99;
    struct llama_model *model = llama_model_load_from_file(argv[1], mp);
    if (!model) return 1;
    printf("Step 1 (model loaded): %zu MB\n", get_mem_mb());

    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = n_ctx;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) return 1;
    printf("Step 2 (context created, n_ctx=%d): %zu MB\n", cp.n_ctx, get_mem_mb());

    const struct llama_vocab *vocab = llama_model_get_vocab(model);

    /* Read long text */
    size_t text_len = 0;
    char *text = NULL;
    if (file_path) {
        text = read_file(file_path, &text_len);
        if (!text) return 1;
    }
    if (!text || text_len < 100) {
        fprintf(stderr, "Text too short!\n");
        text = strdup("The quick brown fox jumps over the lazy dog near the bank of the river. Machine learning is a subset of artificial intelligence that enables systems to learn from data and improve over time without being explicitly programmed for every possible scenario that might arise during deployment.");
        text_len = strlen(text);
    }

    printf("Input text: %zu bytes\n", text_len);

    /* Tokenize */
    int *tokens; int n_tokens = tokenize(vocab, text, (int)text_len, &tokens, false, false);
    if (n_tokens <= 0) { fprintf(stderr, "Tokenization failed\n"); return 1; }
    printf("Tokens: %d (%.1f tokens/sec at 60t/s would take %.0fs)\n",
        n_tokens, (double)n_tokens / 60.0, (double)n_tokens / 60.0);
    int max_tokens = n_ctx - 1; /* leave 1 slot for generation token */
    if (n_tokens > max_tokens) {
        printf("Truncating to %d tokens (n_ctx=%d, leaving 1 slot for gen)\n", max_tokens, n_ctx);
        n_tokens = max_tokens;
    }

    /* Decode full prompt in batches */
    int n_batch = 2048;
    printf("Decoding %d tokens in batches of %d...\n", n_tokens, n_batch);
    for (int i = 0; i < n_tokens; i += n_batch) {
        int n_tok = (n_tokens - i) > n_batch ? n_batch : (n_tokens - i);
        if (llama_decode(ctx, llama_batch_get_one(tokens + i, n_tok))) {
            fprintf(stderr, "  Batch decode failed at token %d\n", i); return 1;
        }
    }
    free(tokens);
    printf("Step 3 (after decode): %zu MB\n", get_mem_mb());

    /* Save clean state (prompt only) */
    size_t state_sz = llama_state_seq_get_size_ext(ctx, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    printf("State buffer size: %zu bytes (%.2f MB)\n", state_sz, (double)state_sz / (1024*1024));
    uint8_t *state_buf = (uint8_t*)malloc(state_sz);
    llama_state_seq_get_data_ext(ctx, state_buf, state_sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    uint8_t *clean_state = (uint8_t*)malloc(state_sz);
    memcpy(clean_state, state_buf, state_sz);
    printf("Step 4 (state saved): %zu MB (+%.1f MB heap)\n", get_mem_mb(), (double)state_sz / (1024*1024));

    /* Generation helper: restore KV state, then generate 1 token with logits */
    llama_token gen_inp = 0;
    struct llama_batch gen_batch = {
        .n_tokens = 1,
        .token    = &gen_inp,
        .embd     = NULL,
        .pos      = NULL,
        .n_seq_id = NULL,
        .seq_id   = NULL,
        .logits   = (int8_t[1]){1},
    };
    
    /* CLEAN generation */
    printf("\n=== Generation: CLEAN KV ===\n");
    memcpy(state_buf, clean_state, state_sz);
    llama_state_seq_set_data_ext(ctx, state_buf, state_sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    llama_decode(ctx, gen_batch);
    float logits_clean[151936];
    memcpy(logits_clean, llama_get_logits_ith(ctx, 0), sizeof(logits_clean));
    float best_vc = -1e30f; int best_tc = 0;
    for (int i = 0; i < 151936; i++) {
        if (logits_clean[i] > best_vc) { best_vc = logits_clean[i]; best_tc = i; }
    }
    char piece_c[128] = {0};
    int nc = llama_token_to_piece(vocab, best_tc, piece_c, sizeof(piece_c), 0, false);
    piece_c[nc < 0 ? 0 : nc] = 0;
    printf("  Clean:  token %d (logit=%.4f) piece='%s'\n", best_tc, best_vc, piece_c);

    /* PERTURBED generation */
    printf("\n=== Generation: PERTURBED KV (stride=%d) ===\n", stride);
    memcpy(state_buf, clean_state, state_sz);
    size_t n_pert = perturb_kv(state_buf, state_sz, stride);
    llama_state_seq_set_data_ext(ctx, state_buf, state_sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    llama_decode(ctx, gen_batch);
    float logits_pert[151936];
    memcpy(logits_pert, llama_get_logits_ith(ctx, 0), sizeof(logits_pert));
    float best_vp = -1e30f; int best_tp = 0;
    for (int i = 0; i < 151936; i++) {
        if (logits_pert[i] > best_vp) { best_vp = logits_pert[i]; best_tp = i; }
    }
    char piece_p[128] = {0};
    int np = llama_token_to_piece(vocab, best_tp, piece_p, sizeof(piece_p), 0, false);
    piece_p[np < 0 ? 0 : np] = 0;
    printf("  Perturbed: token %d (logit=%.4f) piece='%s'\n", best_tp, best_vp, piece_p);

    /* Compare logits */
    double cos_num = 0, cos_a2 = 0, cos_b2 = 0, max_diff = 0;
    int same_sign = 0, same_tok = 0;
    for (int i = 0; i < 151936; i++) {
        cos_num += (double)logits_clean[i] * (double)logits_pert[i];
        cos_a2 += (double)logits_clean[i] * (double)logits_clean[i];
        cos_b2 += (double)logits_pert[i] * (double)logits_pert[i];
        double d = (double)logits_clean[i] - (double)logits_pert[i];
        if (d < 0) d = -d;
        if (d > max_diff) max_diff = d;
        if ((logits_clean[i] > 0) == (logits_pert[i] > 0)) same_sign++;
    }
    double cos_sim = cos_num / (sqrt(cos_a2) * sqrt(cos_b2) + 1e-30);
    printf("\n=== Comparison ===\n");
    printf("  Same token? %s\n", best_tc == best_tp ? "YES" : "NO");
    printf("  Cosine similarity: %.6f\n", cos_sim);
    printf("  Max logit diff:    %.6f\n", max_diff);
    printf("  Same-sign ratio:   %.1f%%\n", 100.0 * same_sign / 151936);
    printf("  Perturbed bytes:   %zu\n", n_pert);
    printf("Final memory: %zu MB\n", get_mem_mb());

    free(state_buf); free(clean_state); free(text);
    llama_free(ctx); llama_model_free(model); llama_backend_free();
    printf("Cleanup: %zu MB\n", get_mem_mb());
    return 0;
}
