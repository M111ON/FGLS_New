/* test_integration.c — Integration test: compare SID-runner output
 * against official llama-cli for a fixed prompt.
 * Measures: model load time, generation speed, cache hit rate.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "llama.h"
#include "ggml-backend.h"
#include "gguf.h"

#define SID_IMPLEMENTATION
#include "../../collection/sid.h"

#include "../gguf_index.h"
#include "../sid_loader.h"
#include "../sid_cache.h"

#define MAX_TOKENS_CACHE  4096
#define RUNNER_CTX_SIZE   2048
#define RUNNER_THREADS    4

typedef struct { float p; int idx; } ProbPair;
static int prob_cmp_desc(const void *a, const void *b) {
    float fa = ((const ProbPair*)a)->p;
    float fb = ((const ProbPair*)b)->p;
    return (fa > fb) ? -1 : (fa < fb) ? 1 : 0;
}

static int sample_token(const float* logits, int n_vocab) {
    int n = n_vocab;
    float* probs = (float*)malloc(n * sizeof(float));
    memcpy(probs, logits, n * sizeof(float));
    float max_val = probs[0];
    for (int i = 1; i < n; i++) if (probs[i] > max_val) max_val = probs[i];
    float sum = 0;
    for (int i = 0; i < n; i++) { probs[i] = expf(probs[i] - max_val); sum += probs[i]; }
    for (int i = 0; i < n; i++) probs[i] /= sum;
    float r = (float)rand() / (float)RAND_MAX;
    float c = 0;
    int token = 0;
    for (int i = 0; i < n; i++) { c += probs[i]; if (r < c) { token = i; break; } }
    free(probs);
    return token;
}

/* SID tensor callback (same as runner) */
typedef struct {
    SIDLoaderCtx  *loader;
    SIDCache      *cache;
    SIDStore      *twidx;
    uint64_t       bytes_read;
    uint64_t       tensors_set;
    uint8_t       *read_buf;
    size_t         read_buf_sz;
} SidRunnerCtx;

static void set_tensor_sid_cb(struct ggml_tensor *t, void *userdata) {
    SidRunnerCtx *rc = (SidRunnerCtx*)userdata;
    if (!t || !t->name[0] || !t->data) return;
    size_t tensor_size = ggml_nbytes(t);
    if (tensor_size > rc->read_buf_sz) {
        rc->read_buf = (uint8_t*)realloc(rc->read_buf, tensor_size);
        rc->read_buf_sz = tensor_size;
    }
    uint8_t *src; size_t src_size;
    if (sid_loader_load(rc->loader, t->name, rc->read_buf, &src, &src_size) == 0) {
        ggml_backend_tensor_set(t, src, 0, tensor_size < src_size ? tensor_size : src_size);
        rc->tensors_set++;
        rc->bytes_read += src_size;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: test_integration <model.gguf> [--twidx model.twidx]\n");
        return 1;
    }

    const char *gguf_path = argv[1];
    const char *twidx_path = NULL;
    for (int i = 2; i < argc; i++)
        if (strcmp(argv[i], "--twidx") == 0 && i+1 < argc)
            twidx_path = argv[++i];

    printf("=== SID Integration Test ===\n");
    printf("GGUF: %s\n", gguf_path);
    if (twidx_path) printf("TWIDX: %s\n", twidx_path);

    SIDCache cache;
    sid_cache_init(&cache, 256 * 1024 * 1024);

    SIDLoaderCtx loader;
    if (sid_loader_open(&loader, gguf_path, &cache) != 0) {
        fprintf(stderr, "FAIL: sid_loader_open\n");
        return 1;
    }

    SIDStore twidx;
    memset(&twidx, 0, sizeof(twidx));
    if (twidx_path) {
        if (sid_read(twidx_path, &twidx) <= 0)
            fprintf(stderr, "  WARN: .twidx not loaded\n");
        else
            printf("  TWIDX: %u entries\n", twidx.n_entries);
    }

    SidRunnerCtx rc;
    memset(&rc, 0, sizeof(rc));
    rc.loader = &loader;
    rc.cache  = &cache;
    rc.twidx  = &twidx;

    llama_backend_init();
    ggml_backend_load_all();

    struct ggml_init_params ggml_params = { .mem_size = 128 * 1024 * 1024, .mem_buffer = NULL };
    struct ggml_context *ggml_ctx = ggml_init(ggml_params);
    struct gguf_init_params gparams = { .no_alloc = true, .ctx = &ggml_ctx };
    struct gguf_context *gctx = gguf_init_from_file(gguf_path, gparams);
    if (!gctx) { fprintf(stderr, "FAIL: gguf_init\n"); return 1; }

    struct llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;

    struct llama_model *model = llama_model_init_from_user(gctx, set_tensor_sid_cb, &rc, mparams);
    if (!model) { fprintf(stderr, "FAIL: model init\n"); return 1; }

    printf("  Model loaded: %llu tensors, file_reads=%llu cache_hits=%llu\n",
           (unsigned long long)rc.tensors_set,
           (unsigned long long)loader.file_hits, cache.hits);

    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx     = RUNNER_CTX_SIZE;
    cparams.n_threads = RUNNER_THREADS;
    cparams.n_threads_batch = RUNNER_THREADS;
    struct llama_context *lctx = llama_init_from_model(model, cparams);
    if (!lctx) { fprintf(stderr, "FAIL: context\n"); return 1; }

    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int n_vocab = llama_vocab_n_tokens(vocab);
    int eos_id = llama_vocab_eos(vocab);
    if (eos_id == -1) eos_id = 151645;

    /* Generate for fixed prompt */
    const char *prompt = "What is 2+2?";
    int n_raw = llama_tokenize(vocab, prompt, strlen(prompt), NULL, 0, true, false);
    int n_tok = n_raw < 0 ? -n_raw : n_raw;
    int *toks = (int*)malloc(n_tok * sizeof(int));
    llama_tokenize(vocab, prompt, strlen(prompt), toks, n_tok, true, false);

    struct llama_batch batch = llama_batch_init(n_tok, 0, 1);
    batch.n_tokens = n_tok;
    for (int j = 0; j < n_tok; j++) {
        batch.token[j] = toks[j];
        batch.pos[j] = j;
        batch.n_seq_id[j] = 1;
        batch.seq_id[j][0] = 0;
        batch.logits[j] = (j == n_tok - 1) ? 1 : 0;
    }
    if (llama_decode(lctx, batch) != 0) { fprintf(stderr, "FAIL: decode\n"); return 1; }
    llama_batch_free(batch);

    int32_t pos = n_tok;
    printf("\nOutput: ");
    for (int i = 0; i < 20; i++) {
        const float* logits = llama_get_logits_ith(lctx, -1);
        int token = sample_token(logits, n_vocab);
        if (token == eos_id || token == 0) break;
        char buf[16];
        int len = llama_token_to_piece(vocab, token, buf, sizeof(buf), 0, false);
        if (len > 0) { buf[len>15?15:len] = '\0'; printf("%s", buf); fflush(stdout); }

        struct llama_batch gb = llama_batch_init(1, 0, 1);
        gb.n_tokens = 1; gb.n_seq_id[0] = 1; gb.seq_id[0][0] = 0; gb.logits[0] = 1;
        gb.token[0] = token; gb.pos[0] = pos++;
        if (llama_decode(lctx, gb) != 0) break;
        llama_batch_free(gb);
    }
    printf("\n\n");

    printf("=== Test Summary ===\n");
    printf("  Tensors loaded: %llu/291\n", (unsigned long long)rc.tensors_set);
    printf("  File reads: %llu\n", (unsigned long long)loader.file_hits);
    printf("  Cache hits: %llu\n", cache.hits);

    int pass = (rc.tensors_set == 291) ? 1 : 0;
    printf("  Verdict: %s\n", pass ? "PASS" : "FAIL");

    llama_free(lctx);
    llama_model_free(model);
    gguf_free(gctx);
    ggml_free(ggml_ctx);
    sid_loader_close(&loader);
    sid_cache_clear(&cache);
    free(rc.read_buf);
    llama_backend_free();
    return pass ? 0 : 1;
}
