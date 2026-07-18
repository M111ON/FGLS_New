/*
 * llama_pogls_runner_sid.c — SID-coordinate inference runner
 *
 * Loads model via llama_model_init_from_user with a custom tensor
 * callback that uses SID coordinate routing + TWFaceRewind cache.
 *
 * Build:
 *   gcc -O2 -I. -I<llama_include> -o llama_pogls_runner_sid.exe \
 *       llama_pogls_runner_sid.c \
 *       llama.dll ggml.dll ggml-base.dll ggml-cpu-x64.dll -lzstd -lm
 *
 * Usage:
 *   llama_pogls_runner_sid.exe model.gguf --twidx model.twidx [options]
 */

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #define CLOCK_MONOTONIC 0
  typedef struct { long tv_sec; long tv_nsec; } PoglsTime;
  static inline int clock_gettime(int _clk, PoglsTime *ts) {
      LARGE_INTEGER freq, cnt;
      QueryPerformanceFrequency(&freq);
      QueryPerformanceCounter(&cnt);
      ts->tv_sec  = (long)(cnt.QuadPart / freq.QuadPart);
      ts->tv_nsec = (long)(cnt.QuadPart % freq.QuadPart
                           * 1000000000LL / freq.QuadPart);
      (void)_clk; return 0;
  }
#else
  #include <time.h>
  typedef struct timespec PoglsTime;
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>

#include "llama.h"
#include "ggml-backend.h"
#include "gguf.h"

#define SID_IMPLEMENTATION
#include "../collection/sid.h"

#include "gguf_index.h"
#include "sid_loader.h"
#include "sid_cache.h"

/* ── Constants ─────────────────────────────────────────────── */
#define RUNNER_CTX_SIZE     2048u
#define RUNNER_THREADS      4
#define MAX_LINE            4096
#define MAX_TOKENS_CACHE    4096
#define MAX_CHAT_HISTORY    128
#define CACHE_POOL_SIZE     (256 * 1024 * 1024)  /* 256 MB default */

/* ── Sampler ───────────────────────────────────────────────── */
typedef struct {
    float temp;
    float top_p;
    int   top_k;
    float repeat_penalty;
    int   repeat_last_n;
    int   penalty_tokens[MAX_TOKENS_CACHE];
    int   penalty_count;
} Sampler;

typedef struct { float p; int idx; } ProbPair;

static int prob_cmp_desc(const void *a, const void *b) {
    float fa = ((const ProbPair*)a)->p;
    float fb = ((const ProbPair*)b)->p;
    if (fa > fb) return -1;
    if (fa < fb) return  1;
    return 0;
}

static int sample_token(const float* logits, int n_vocab, Sampler* sp) {
    int n = n_vocab;
    float* probs = (float*)malloc(n * sizeof(float));
    if (!probs) return 0;
    memcpy(probs, logits, n * sizeof(float));

    if (sp->repeat_penalty != 1.0f && sp->penalty_count > 0) {
        int start = sp->penalty_count > sp->repeat_last_n
                    ? sp->penalty_count - sp->repeat_last_n : 0;
        for (int i = start; i < sp->penalty_count; i++) {
            int t = sp->penalty_tokens[i];
            if (t >= 0 && t < n) {
                if (probs[t] < 0) probs[t] *= sp->repeat_penalty;
                else              probs[t] /= sp->repeat_penalty;
            }
        }
    }

    if (sp->temp > 0.0f)
        for (int i = 0; i < n; i++) probs[i] /= sp->temp;

    float max_val = probs[0];
    for (int i = 1; i < n; i++) if (probs[i] > max_val) max_val = probs[i];
    float sum = 0;
    for (int i = 0; i < n; i++) { probs[i] = expf(probs[i] - max_val); sum += probs[i]; }
    if (sum > 0) for (int i = 0; i < n; i++) probs[i] /= sum;

    int do_k = (sp->top_k > 0 && sp->top_k < n);
    int do_p = (sp->top_p < 1.0f);
    if (do_k || do_p) {
        ProbPair* pairs = (ProbPair*)malloc(n * sizeof(ProbPair));
        for (int i = 0; i < n; i++) { pairs[i].p = probs[i]; pairs[i].idx = i; }
        qsort(pairs, n, sizeof(ProbPair), prob_cmp_desc);

        if (do_k) {
            float kth = pairs[sp->top_k - 1].p;
            for (int i = 0; i < n; i++) if (probs[i] < kth) probs[i] = 0;
            sum = 0; for (int i = 0; i < n; i++) sum += probs[i];
            if (sum > 0) for (int i = 0; i < n; i++) probs[i] /= sum;
        }
        if (do_p) {
            float cum = 0;
            for (int i = 0; i < n; i++) {
                if (cum >= sp->top_p) { for (int j = i; j < n; j++) probs[pairs[j].idx] = 0; break; }
                cum += pairs[i].p;
            }
            sum = 0; for (int i = 0; i < n; i++) sum += probs[i];
            if (sum > 0) for (int i = 0; i < n; i++) probs[i] /= sum;
        }
        free(pairs);
    }

    float r = (float)rand() / (float)RAND_MAX;
    float c = 0;
    int token = 0;
    for (int i = 0; i < n; i++) { c += probs[i]; if (r < c) { token = i; break; } }
    free(probs);

    if (sp->penalty_count < MAX_TOKENS_CACHE)
        sp->penalty_tokens[sp->penalty_count++] = token;
    return token;
}

/* ── Chat history ──────────────────────────────────────────── */
typedef struct {
    char* role[MAX_CHAT_HISTORY];
    char* content[MAX_CHAT_HISTORY];
    int   count;
} ChatHistory;

static void chat_push(ChatHistory* ch, const char* role, const char* content) {
    if (ch->count >= MAX_CHAT_HISTORY) return;
    ch->role[ch->count]    = _strdup(role);
    ch->content[ch->count] = _strdup(content);
    ch->count++;
}

static void chat_free(ChatHistory* ch) {
    for (int i = 0; i < ch->count; i++) {
        free(ch->role[i]); free(ch->content[i]);
    }
    ch->count = 0;
}

static char* chat_format_qwen(ChatHistory* ch) {
    size_t total = 0;
    for (int i = 0; i < ch->count; i++)
        total += 64 + strlen(ch->content[i]);
    total += 128;
    char* buf = (char*)calloc(total + 1, 1);
    if (!buf) return NULL;
    size_t pos = 0;
    for (int i = 0; i < ch->count; i++)
        pos += sprintf(buf + pos, "<|im_start|>%s\n%s<|im_end|>\n",
                       ch->role[i], ch->content[i]);
    pos += sprintf(buf + pos, "<|im_start|>assistant\n");
    return buf;
}

/* ── SID tensor callback ───────────────────────────────────── */
typedef struct {
    SIDLoaderCtx  *loader;
    SIDCache      *cache;
    SIDStore      *twidx;
    uint64_t       bytes_read;
    uint64_t       tensors_set;
    uint8_t       *read_buf;       /* temporary read buffer, sized to largest tensor */
    size_t         read_buf_sz;
} SidRunnerCtx;

static void set_tensor_sid_cb(struct ggml_tensor *t, void *userdata) {
    SidRunnerCtx *rc = (SidRunnerCtx*)userdata;
    if (!t || !t->name[0] || !t->data) return;

    size_t tensor_size = ggml_nbytes(t);

    /* Ensure read buffer is large enough */
    if (tensor_size > rc->read_buf_sz) {
        rc->read_buf = (uint8_t*)realloc(rc->read_buf, tensor_size);
        rc->read_buf_sz = tensor_size;
    }

    /* Load via SID cache-aware path */
    uint8_t *src;
    size_t   src_size;
    if (sid_loader_load(rc->loader, t->name, rc->read_buf, &src, &src_size) == 0) {
        ggml_backend_tensor_set(t, src, 0, tensor_size < src_size ? tensor_size : src_size);
        rc->tensors_set++;
        rc->bytes_read += src_size;
    }
}

/* ── CLI ───────────────────────────────────────────────────── */
static void print_help(void) {
    fprintf(stderr,
        "Usage: llama_pogls_runner_sid <model.gguf> --twidx <model.twidx> [options]\n"
        "Options:\n"
        "  --twidx <path>    SID coordinate index (from sid_capture_gguf)\n"
        "  --ngl <N>         GPU layers (default 0 = CPU)\n"
        "  --temp <T>        sampling temperature (default 0.7)\n"
        "  --top-p <P>       top-p sampling (default 0.9)\n"
        "  --top-k <K>       top-k sampling (default 40, 0=off)\n"
        "  --repeat-penalty  repeat penalty (default 1.1)\n"
        "  --max-new <N>     max generated tokens (default 256)\n"
        "  --prompt <str>    input prompt\n"
        "  --chat            interactive chat mode\n"
        "  --cache <MB>      SID cache size in MB (default 256)\n"
        "  -h                help\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { print_help(); return 1; }

    const char *gguf_path  = NULL;
    const char *twidx_path = NULL;
    int opt_ngl    = 0;
    int opt_max_new = 256;
    const char *opt_prompt = NULL;
    int opt_chat = 0;
    uint64_t cache_mb = 256;

    Sampler sp;
    sp.temp = 0.7f;
    sp.top_p = 0.9f;
    sp.top_k = 40;
    sp.repeat_penalty = 1.1f;
    sp.repeat_last_n = 64;
    sp.penalty_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--twidx") == 0 && i+1 < argc)
            twidx_path = argv[++i];
        else if (strcmp(argv[i], "--ngl") == 0 && i+1 < argc)
            opt_ngl = atoi(argv[++i]);
        else if (strcmp(argv[i], "--temp") == 0 && i+1 < argc)
            sp.temp = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--top-p") == 0 && i+1 < argc)
            sp.top_p = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--top-k") == 0 && i+1 < argc)
            sp.top_k = atoi(argv[++i]);
        else if (strcmp(argv[i], "--repeat-penalty") == 0 && i+1 < argc)
            sp.repeat_penalty = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--max-new") == 0 && i+1 < argc)
            opt_max_new = atoi(argv[++i]);
        else if (strcmp(argv[i], "--prompt") == 0 && i+1 < argc)
            opt_prompt = argv[++i];
        else if (strcmp(argv[i], "--chat") == 0)
            opt_chat = 1;
        else if (strcmp(argv[i], "--cache") == 0 && i+1 < argc)
            cache_mb = (uint64_t)atol(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0)
            { print_help(); return 0; }
        else if (!gguf_path)
            gguf_path = argv[i];
    }

    if (!gguf_path) {
        fprintf(stderr, "ERROR: missing model.gguf\n");
        return 1;
    }

    fprintf(stderr, "=== SID Inference Runner ===\n");
    fprintf(stderr, "GGUF: %s\n", gguf_path);
    if (twidx_path)
        fprintf(stderr, "TWIDX: %s\n", twidx_path);
    fprintf(stderr, "Cache: %llu MB\n", (unsigned long long)cache_mb);

    /* ── Init SID cache + loader ── */
    SIDCache cache;
    sid_cache_init(&cache, cache_mb * 1024 * 1024);

    SIDLoaderCtx loader;
    if (sid_loader_open(&loader, gguf_path, &cache) != 0) {
        fprintf(stderr, "ERROR: failed to open GGUF\n");
        return 1;
    }

    SIDStore twidx;
    memset(&twidx, 0, sizeof(twidx));
    if (twidx_path) {
        if (sid_read(twidx_path, &twidx) <= 0)
            fprintf(stderr, "WARN: no .twidx found, running without SID routing\n");
        else
            fprintf(stderr, "TWIDX: %u entries loaded\n", twidx.n_entries);
    }

    SidRunnerCtx rc;
    memset(&rc, 0, sizeof(rc));
    rc.loader = &loader;
    rc.cache  = &cache;
    rc.twidx  = &twidx;

    /* ── Init llama backend ── */
    llama_backend_init();
    ggml_backend_load_all();

    struct ggml_init_params ggml_params = { .mem_size = 128 * 1024 * 1024, .mem_buffer = NULL };
    struct ggml_context *ggml_ctx = ggml_init(ggml_params);

    struct gguf_init_params gparams = { .no_alloc = true, .ctx = &ggml_ctx };
    struct gguf_context *gctx = gguf_init_from_file(gguf_path, gparams);
    if (!gctx) { fprintf(stderr, "ERROR: gguf_init\n"); return 1; }

    struct llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = opt_ngl;

    PoglsTime t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    struct llama_model *model = llama_model_init_from_user(gctx, set_tensor_sid_cb, &rc, mparams);
    if (!model) { fprintf(stderr, "ERROR: model init\n"); return 1; }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double load_ms = (t1.tv_sec - t0.tv_sec)*1000.0 + (t1.tv_nsec - t0.tv_nsec)/1e6;

    fprintf(stderr, "\n[load] %llu tensors  %.0f ms  (file=%llu, cache=%llu)\n",
            (unsigned long long)rc.tensors_set, load_ms,
            (unsigned long long)loader.file_hits, rc.cache->hits);

    /* ── Context ── */
    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx           = RUNNER_CTX_SIZE;
    cparams.n_threads       = RUNNER_THREADS;
    cparams.n_threads_batch = RUNNER_THREADS;
    struct llama_context *lctx = llama_init_from_model(model, cparams);
    if (!lctx) { fprintf(stderr, "ERROR: context\n"); return 1; }

    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int n_vocab = llama_vocab_n_tokens(vocab);
    int eos_id = llama_vocab_eos(vocab);
    if (eos_id == -1) eos_id = 151645;

    /* ── Interactive Chat ── */
    if (opt_chat) {
        ChatHistory chat;
        memset(&chat, 0, sizeof(chat));
        chat_push(&chat, "system", "You are a helpful assistant.");

        printf("\n=== SID Chat Mode ===\n");
        printf("Type '/clear' to reset, '/exit' to quit\n\n");

        char line[MAX_LINE];
        while (1) {
            printf(">>> "); fflush(stdout);
            if (!fgets(line, sizeof(line), stdin)) break;
            size_t llen = strlen(line);
            while (llen > 0 && (line[llen-1] == '\n' || line[llen-1] == '\r')) line[--llen] = 0;
            if (llen == 0) continue;
            if (strcmp(line, "/exit") == 0) break;
            if (strcmp(line, "/clear") == 0) {
                chat_free(&chat); memset(&chat, 0, sizeof(chat));
                chat_push(&chat, "system", "You are a helpful assistant.");
                printf("Chat cleared.\n"); continue;
            }

            chat_push(&chat, "user", line);
            char* formatted = chat_format_qwen(&chat);
            if (!formatted) break;

            int n_raw = llama_tokenize(vocab, formatted, strlen(formatted), NULL, 0, false, false);
            int n_tok = n_raw < 0 ? -n_raw : n_raw;
            if (n_tok <= 0 || n_tok > RUNNER_CTX_SIZE - 64) {
                fprintf(stderr, "Prompt too long (%d)\n", n_tok);
                free(formatted); continue;
            }
            int* toks = (int*)malloc(n_tok * sizeof(int));
            llama_tokenize(vocab, formatted, strlen(formatted), toks, n_tok, false, false);
            free(formatted);

            struct llama_batch pb = llama_batch_init(n_tok, 0, 1);
            pb.n_tokens = n_tok;
            for (int j = 0; j < n_tok; j++) {
                pb.token[j] = toks[j];
                pb.pos[j] = j;
                pb.n_seq_id[j] = 1;
                pb.seq_id[j][0] = 0;
                pb.logits[j] = (j == n_tok - 1) ? 1 : 0;
            }

            if (llama_decode(lctx, pb) != 0) {
                llama_batch_free(pb); free(toks);
                fprintf(stderr, "ERROR: decode\n"); break;
            }
            llama_batch_free(pb);

            Sampler gen_sp = sp;
            gen_sp.penalty_count = 0;
            int32_t pos = n_tok;
            int gen_count = 0;

            clock_gettime(CLOCK_MONOTONIC, &t0);
            struct llama_batch gb = llama_batch_init(1, 0, 1);
            gb.n_tokens = 1; gb.n_seq_id[0] = 1; gb.seq_id[0][0] = 0; gb.logits[0] = 1;

            for (int i = 0; i < opt_max_new; i++) {
                const float* logits = llama_get_logits_ith(lctx, -1);
                int token = sample_token(logits, n_vocab, &gen_sp);
                if (token == eos_id || token == 0) break;
                gen_count++;
                char buf[16];
                int len = llama_token_to_piece(vocab, token, buf, sizeof(buf), 0, false);
                if (len > 0) { buf[len>15?15:len] = '\0'; printf("%s", buf); fflush(stdout); }
                gb.token[0] = token; gb.pos[0] = pos++;
                if (llama_decode(lctx, gb) != 0) break;
            }
            llama_batch_free(gb);

            clock_gettime(CLOCK_MONOTONIC, &t1);
            double gen_ms = (t1.tv_sec - t0.tv_sec)*1000.0 + (t1.tv_nsec - t0.tv_nsec)/1e6;
            fprintf(stderr, "\n[gen] %d tokens in %.0f ms (%.1f t/s)\n",
                    gen_count, gen_ms, gen_count/(gen_ms/1000.0));
            printf("\n");
            free(toks);
        }
        chat_free(&chat);
    }

    /* ── Single prompt mode ── */
    if (opt_prompt) {
        int n_raw = llama_tokenize(vocab, opt_prompt, strlen(opt_prompt), NULL, 0, true, false);
        int n_tok = n_raw < 0 ? -n_raw : n_raw;
        if (n_tok <= 0) { fprintf(stderr, "Tokenize failed\n"); return 1; }
        int* toks = (int*)malloc(n_tok * sizeof(int));
        llama_tokenize(vocab, opt_prompt, strlen(opt_prompt), toks, n_tok, true, false);

        struct llama_batch batch = llama_batch_init(n_tok, 0, 1);
        batch.n_tokens = n_tok;
        for (int j = 0; j < n_tok; j++) {
            batch.token[j] = toks[j];
            batch.pos[j] = j;
            batch.n_seq_id[j] = 1;
            batch.seq_id[j][0] = 0;
            batch.logits[j] = (j == n_tok - 1) ? 1 : 0;
        }
        if (llama_decode(lctx, batch) != 0) { llama_batch_free(batch); free(toks); return 1; }
        llama_batch_free(batch);

        Sampler gen_sp = sp;
        gen_sp.penalty_count = 0;
        int32_t pos = n_tok;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        struct llama_batch gb = llama_batch_init(1, 0, 1);
        gb.n_tokens = 1; gb.n_seq_id[0] = 1; gb.seq_id[0][0] = 0; gb.logits[0] = 1;

        for (int i = 0; i < opt_max_new; i++) {
            const float* logits = llama_get_logits_ith(lctx, -1);
            int token = sample_token(logits, n_vocab, &gen_sp);
            if (token == eos_id || token == 0) break;
            char buf[16];
            int len = llama_token_to_piece(vocab, token, buf, sizeof(buf), 0, false);
            if (len > 0) { buf[len>15?15:len] = '\0'; printf("%s", buf); fflush(stdout); }
            gb.token[0] = token; gb.pos[0] = pos++;
            if (llama_decode(lctx, gb) != 0) break;
        }
        llama_batch_free(gb);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double gen_ms = (t1.tv_sec - t0.tv_sec)*1000.0 + (t1.tv_nsec - t0.tv_nsec)/1e6;
        fprintf(stderr, "\n[gen] %.0f ms\n", gen_ms);
        printf("\n");
        free(toks);
    }

    /* ── Cleanup ── */
    fprintf(stderr, "\n[stats] file_reads=%llu cache_hits=%llu pool_used=%llu/%llu\n",
            (unsigned long long)loader.file_hits, cache.hits,
            (unsigned long long)cache.pool_used, (unsigned long long)cache.pool_size);

    llama_free(lctx);
    llama_model_free(model);
    gguf_free(gctx);
    ggml_free(ggml_ctx);
    sid_loader_close(&loader);
    sid_cache_clear(&cache);
    free(rc.read_buf);
    llama_backend_free();
    return 0;
}
