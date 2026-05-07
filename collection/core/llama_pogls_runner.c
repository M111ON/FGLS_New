/*
 * llama_pogls_runner.c — S83-B4
 *
 * Test runner for two things:
 *   1. layer-by-layer GGUF chunk streaming
 *   2. live llama.cpp token streaming from the same model
 *
 * Build:
 *   gcc -O2 -Wall -I. -I<LLAMA>/include \
 *       llama_pogls_runner.c llama_pogls_backend.c \
 *       gguf_to_model_index.c pogls_recon_file.c \
 *       -L<LLAMA> -lllama -lggml -o pogls_runner
 */

#include "llama_pogls_backend.h"

#include "llama.h"
#include "ggml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <ctype.h>
#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
static uint64_t now_ms(void) { return (uint64_t)GetTickCount(); }
#else
#include <time.h>
static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}
#endif

#define RUNNER_CTX_SIZE      2048u
#define RUNNER_N_GPU_LAYERS  99
#define RUNNER_THREADS       4
#define RUNNER_PROMPT_TOKENS  4096
#define RUNNER_PROMPT_MAX     4096
#define RUNNER_MEMORY_MAX     4096
#define RUNNER_CHAT_INPUT_MAX  1024
#define RUNNER_CHAT_REPLY_MAX  1024
#define RUNNER_CHAT_TURNS      8
#define RUNNER_CHAT_PROMPT_MAX 12288
#define RUNNER_BROWSER_PROMPT_MAX 32768

typedef struct {
    HMODULE ggml_dll;
    HMODULE dll;
    void (*ggml_backend_load_all)(void);
    void (*ggml_backend_load_all_from_path)(const char *);
    void (*backend_init)(void);
    void (*backend_free)(void);
    struct llama_model_params (*model_default_params)(void);
    struct llama_model * (*model_load_from_file)(const char *, struct llama_model_params);
    void (*model_free)(struct llama_model *);
    struct llama_context_params (*context_default_params)(void);
    struct llama_context * (*init_from_model)(struct llama_model *, struct llama_context_params);
    void (*free_ctx)(struct llama_context *);
    void (*set_n_threads)(struct llama_context *, int32_t, int32_t);
    const struct llama_vocab * (*model_get_vocab)(const struct llama_model *);
    int32_t (*tokenize)(const struct llama_vocab *, const char *, int32_t, llama_token *, int32_t, bool, bool);
    int32_t (*token_to_piece)(const struct llama_vocab *, llama_token, char *, int32_t, int32_t, bool);
    struct llama_batch (*batch_init)(int32_t, int32_t, int32_t);
    void (*batch_free)(struct llama_batch);
    int32_t (*decode)(struct llama_context *, struct llama_batch);
    struct llama_sampler * (*sampler_init_greedy)(void);
    void (*sampler_free)(struct llama_sampler *);
    llama_token (*sampler_sample)(struct llama_sampler *, struct llama_context *, int32_t);
    void (*sampler_accept)(struct llama_sampler *, llama_token);
    bool (*vocab_is_eog)(const struct llama_vocab *, llama_token);
} LlamaApi;

typedef struct {
    char user[RUNNER_CHAT_INPUT_MAX];
    char assistant[RUNNER_CHAT_REPLY_MAX];
} ChatTurn;

typedef struct {
    LlamaApi api;
    struct llama_model *model;
    const struct llama_vocab *vocab;
} LlamaSession;

static void trim_newline(char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

static void append_text(char *dst, size_t dst_sz, const char *src)
{
    size_t have = strlen(dst);
    size_t left = (have < dst_sz) ? (dst_sz - have - 1) : 0;
    if (left == 0 || !src || !*src) return;
    strncat(dst, src, left);
}

static const char *path_basename(const char *path)
{
    const char *base = path;
    for (const char *p = path; p && *p; ++p) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    return base;
}

static void derive_model_name(const char *gguf_path, char *out, size_t out_sz)
{
    const char *base = path_basename(gguf_path);
    size_t len = strlen(base);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, base, len);
    out[len] = '\0';
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
}

static int path_exists(const char *path)
{
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
#endif
}

static int resolve_memory_search_script(char *out, size_t out_sz)
{
    const char *candidates[] = {
        "goldberg_field_core\\llm_memory_search.py",
        "core\\goldberg_field_core\\llm_memory_search.py",
        NULL
    };

    for (int i = 0; candidates[i]; ++i) {
        if (path_exists(candidates[i])) {
            strncpy(out, candidates[i], out_sz - 1);
            out[out_sz - 1] = '\0';
            return 1;
        }
    }
    return 0;
}

static void shell_quote_path(const char *src, char *dst, size_t dst_sz)
{
    size_t j = 0;
    if (dst_sz == 0) return;
    dst[j++] = '"';
    for (size_t i = 0; src[i] && j + 2 < dst_sz; ++i) {
        if (src[i] == '"') {
            dst[j++] = '\\';
        }
        dst[j++] = src[i];
    }
    if (j + 1 < dst_sz) {
        dst[j++] = '"';
    }
    dst[j < dst_sz ? j : dst_sz - 1] = '\0';
}

static int run_memory_search(const char *query, char *out, size_t out_sz)
{
    if (!query || !*query || !out || out_sz == 0)
        return 0;

    char script_path[256];
    char quoted_path[300];
    char query_buf[1024];
    char command[512];

    if (!resolve_memory_search_script(script_path, sizeof(script_path)))
        return 0;

    strncpy(query_buf, query, sizeof(query_buf) - 1);
    query_buf[sizeof(query_buf) - 1] = '\0';
    shell_quote_path(script_path, quoted_path, sizeof(quoted_path));

    const char *store_path = (strncmp(script_path, "core\\", 5) == 0)
        ? "core\\goldberg_field_core\\memory_store"
        : "goldberg_field_core\\memory_store";

#ifdef _WIN32
    _putenv_s("POGLS_MEMORY_QUERY", query_buf);
    _putenv_s("POGLS_MEMORY_TOP_K", "4");
    _putenv_s("POGLS_MEMORY_STORE", store_path);
#else
    setenv("POGLS_MEMORY_QUERY", query_buf, 1);
    setenv("POGLS_MEMORY_TOP_K", "4", 1);
    setenv("POGLS_MEMORY_STORE", store_path, 1);
#endif

    snprintf(command, sizeof(command), "python %s --top 3 --max-preview 220", quoted_path);

#ifdef _WIN32
    FILE *pipe = _popen(command, "r");
#else
    FILE *pipe = popen(command, "r");
#endif
    if (!pipe)
        return 0;

    size_t used = 0;
    out[0] = '\0';
    while (!feof(pipe) && used + 1 < out_sz) {
        size_t want = out_sz - used - 1;
        size_t got = fread(out + used, 1, want, pipe);
        used += got;
        out[used] = '\0';
        if (got == 0)
            break;
    }

#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return used > 0;
}

static int load_llama_api(LlamaApi *api);

static void llama_session_free(LlamaSession *session)
{
    if (!session)
        return;
    if (session->model) {
        session->api.model_free(session->model);
        session->model = NULL;
    }
    session->vocab = NULL;
    if (session->api.backend_free)
        session->api.backend_free();
    if (session->api.dll) {
        FreeLibrary(session->api.dll);
        session->api.dll = NULL;
    }
    if (session->api.ggml_dll) {
        FreeLibrary(session->api.ggml_dll);
        session->api.ggml_dll = NULL;
    }
}

static int llama_session_init(LlamaSession *session, const char *gguf_path)
{
    if (!session || !gguf_path || !*gguf_path)
        return 0;

    memset(session, 0, sizeof(*session));

    if (!load_llama_api(&session->api))
        return 0;

    session->api.ggml_backend_load_all_from_path("I:\\llama\\llama_cuda124_x64");
    session->api.backend_init();

    struct llama_model_params mparams = session->api.model_default_params();
    mparams.n_gpu_layers = RUNNER_N_GPU_LAYERS;
    mparams.use_mmap = false;

    session->model = session->api.model_load_from_file(gguf_path, mparams);
    if (!session->model) {
        fprintf(stderr, "[llama] model load failed: %s\n", gguf_path);
        llama_session_free(session);
        return 0;
    }

    session->vocab = session->api.model_get_vocab(session->model);
    if (!session->vocab) {
        fprintf(stderr, "[llama] vocab unavailable\n");
        llama_session_free(session);
        return 0;
    }
    return 1;
}

static int build_turn_prompt(const ChatTurn *turns,
                             size_t turn_count,
                             const char *user_msg,
                             char *out,
                             size_t out_sz)
{
    char memory_context[RUNNER_MEMORY_MAX];
    size_t start = 0;

    if (!out || out_sz == 0)
        return 0;
    out[0] = '\0';

    append_text(out, out_sz,
                "You are a local assistant running inside this repo.\n"
                "Keep answers concise, direct, and grounded in memory only when relevant.\n"
                "If memory is unrelated, ignore it.\n\n");

    memset(memory_context, 0, sizeof(memory_context));
    if (run_memory_search(user_msg, memory_context, sizeof(memory_context))) {
        append_text(out, out_sz, memory_context);
        append_text(out, out_sz, "\n\n");
    }

    if (turn_count > RUNNER_CHAT_TURNS)
        start = turn_count - RUNNER_CHAT_TURNS;

    if (turn_count > start) {
        append_text(out, out_sz, "Conversation history:\n");
        for (size_t i = start; i < turn_count; ++i) {
            append_text(out, out_sz, "User: ");
            append_text(out, out_sz, turns[i].user);
            append_text(out, out_sz, "\nAssistant: ");
            append_text(out, out_sz, turns[i].assistant);
            append_text(out, out_sz, "\n");
        }
        append_text(out, out_sz, "\n");
    }

    append_text(out, out_sz, "User: ");
    append_text(out, out_sz, user_msg);
    append_text(out, out_sz, "\nAssistant:");
    return 1;
}

static int run_streamed_turn(LlamaSession *session,
                             const char *prompt,
                             int n_tokens,
                             char *reply,
                             size_t reply_sz)
{
    int rc = 0;
    struct llama_context *ctx = NULL;
    struct llama_sampler *smpl = NULL;
    const struct llama_vocab *vocab = session->vocab;

    if (!session || !session->model || !vocab || !prompt || !reply || reply_sz == 0)
        return 1;

    reply[0] = '\0';

    struct llama_context_params cparams = session->api.context_default_params();
    cparams.n_ctx = RUNNER_CTX_SIZE;
    cparams.n_threads = RUNNER_THREADS;

    ctx = session->api.init_from_model(session->model, cparams);
    if (!ctx) {
        fprintf(stderr, "[llama] context init failed\n");
        return 1;
    }
    session->api.set_n_threads(ctx, RUNNER_THREADS, RUNNER_THREADS);

    llama_token prompt_tokens[RUNNER_PROMPT_TOKENS];
    int32_t n_prompt = session->api.tokenize(
        vocab, prompt, (int32_t)strlen(prompt),
        prompt_tokens, (int32_t)(sizeof(prompt_tokens) / sizeof(prompt_tokens[0])),
        true, true);
    if (n_prompt < 0) {
        fprintf(stderr, "[llama] tokenization failed\n");
        rc = 1;
        goto done;
    }
    if (n_prompt >= (int32_t)(sizeof(prompt_tokens) / sizeof(prompt_tokens[0]))) {
        fprintf(stderr, "[llama] prompt too long for token buffer\n");
        rc = 1;
        goto done;
    }

    struct llama_batch prompt_batch = session->api.batch_init(n_prompt, 0, 1);
    prompt_batch.n_tokens = n_prompt;
    for (int32_t i = 0; i < n_prompt; ++i) {
        prompt_batch.token[i] = prompt_tokens[i];
        prompt_batch.pos[i] = i;
        prompt_batch.n_seq_id[i] = 1;
        prompt_batch.seq_id[i][0] = 0;
        prompt_batch.logits[i] = (i + 1 == n_prompt);
    }

    if (session->api.decode(ctx, prompt_batch) < 0) {
        fprintf(stderr, "[llama] prompt decode failed\n");
        session->api.batch_free(prompt_batch);
        rc = 1;
        goto done;
    }
    session->api.batch_free(prompt_batch);

    smpl = session->api.sampler_init_greedy();
    if (!smpl) {
        fprintf(stderr, "[llama] sampler init failed\n");
        rc = 1;
        goto done;
    }

    fprintf(stdout, "[stream] ");
    fflush(stdout);

    int32_t pos = n_prompt;
    for (int i = 0; i < n_tokens; ++i) {
        llama_token id = session->api.sampler_sample(smpl, ctx, -1);
        if (session->api.vocab_is_eog(vocab, id))
            break;

        char piece[128];
        int32_t n_piece = session->api.token_to_piece(vocab, id, piece, (int32_t)sizeof(piece), 0, false);
        if (n_piece > 0) {
            fwrite(piece, 1, (size_t)n_piece, stdout);
            fflush(stdout);
            append_text(reply, reply_sz, piece);
        }

        session->api.sampler_accept(smpl, id);

        struct llama_batch b = session->api.batch_init(1, 0, 1);
        b.n_tokens = 1;
        b.token[0] = id;
        b.pos[0] = pos++;
        b.n_seq_id[0] = 1;
        b.seq_id[0][0] = 0;
        b.logits[0] = 1;
        if (session->api.decode(ctx, b) < 0) {
            session->api.batch_free(b);
            fprintf(stderr, "\n[llama] decode failed during stream\n");
            rc = 1;
            goto done;
        }
        session->api.batch_free(b);
    }
    fprintf(stdout, "\n");

done:
    if (smpl) session->api.sampler_free(smpl);
    if (ctx) session->api.free_ctx(ctx);
    return rc;
}

static int store_turn(ChatTurn *turns, size_t *turn_count, const char *user_msg, const char *assistant_msg)
{
    if (!turns || !turn_count || !user_msg || !assistant_msg)
        return 0;

    if (*turn_count < RUNNER_CHAT_TURNS) {
        strncpy(turns[*turn_count].user, user_msg, sizeof(turns[*turn_count].user) - 1);
        turns[*turn_count].user[sizeof(turns[*turn_count].user) - 1] = '\0';
        strncpy(turns[*turn_count].assistant, assistant_msg, sizeof(turns[*turn_count].assistant) - 1);
        turns[*turn_count].assistant[sizeof(turns[*turn_count].assistant) - 1] = '\0';
        (*turn_count)++;
        return 1;
    }

    memmove(&turns[0], &turns[1], sizeof(ChatTurn) * (RUNNER_CHAT_TURNS - 1));
    strncpy(turns[RUNNER_CHAT_TURNS - 1].user, user_msg, sizeof(turns[RUNNER_CHAT_TURNS - 1].user) - 1);
    turns[RUNNER_CHAT_TURNS - 1].user[sizeof(turns[RUNNER_CHAT_TURNS - 1].user) - 1] = '\0';
    strncpy(turns[RUNNER_CHAT_TURNS - 1].assistant, assistant_msg, sizeof(turns[RUNNER_CHAT_TURNS - 1].assistant) - 1);
    turns[RUNNER_CHAT_TURNS - 1].assistant[sizeof(turns[RUNNER_CHAT_TURNS - 1].assistant) - 1] = '\0';
    return 1;
}

static int load_llama_api(LlamaApi *api)
{
    const char *dll_path = "I:\\llama\\llama_cuda124_x64\\llama.dll";
    const char *ggml_path = "I:\\llama\\llama_cuda124_x64\\ggml.dll";
    memset(api, 0, sizeof(*api));

#ifdef _WIN32
    SetDllDirectoryA("I:\\llama\\llama_cuda124_x64");
#endif

    api->ggml_dll = LoadLibraryA(ggml_path);
    if (!api->ggml_dll) {
        fprintf(stderr, "[llama] failed to load %s\n", ggml_path);
        return 0;
    }

    api->ggml_backend_load_all = (void *)GetProcAddress(api->ggml_dll, "ggml_backend_load_all");
    api->ggml_backend_load_all_from_path = (void *)GetProcAddress(api->ggml_dll, "ggml_backend_load_all_from_path");
    if (!api->ggml_backend_load_all || !api->ggml_backend_load_all_from_path) {
        fprintf(stderr, "[llama] missing symbol: ggml_backend_load_all\n");
        FreeLibrary(api->ggml_dll);
        memset(api, 0, sizeof(*api));
        return 0;
    }

    api->dll = LoadLibraryA(dll_path);
    if (!api->dll) {
        fprintf(stderr, "[llama] failed to load %s\n", dll_path);
        FreeLibrary(api->ggml_dll);
        return 0;
    }

#define LOAD_SYM(field, sym) do { \
    api->field = (void *)GetProcAddress(api->dll, sym); \
    if (!api->field) { \
        fprintf(stderr, "[llama] missing symbol: %s\n", sym); \
        FreeLibrary(api->dll); \
        FreeLibrary(api->ggml_dll); \
        memset(api, 0, sizeof(*api)); \
        return 0; \
    } \
} while (0)

    LOAD_SYM(backend_init, "llama_backend_init");
    LOAD_SYM(backend_free, "llama_backend_free");
    LOAD_SYM(model_default_params, "llama_model_default_params");
    LOAD_SYM(model_load_from_file, "llama_model_load_from_file");
    LOAD_SYM(model_free, "llama_model_free");
    LOAD_SYM(context_default_params, "llama_context_default_params");
    LOAD_SYM(init_from_model, "llama_init_from_model");
    LOAD_SYM(free_ctx, "llama_free");
    LOAD_SYM(set_n_threads, "llama_set_n_threads");
    LOAD_SYM(model_get_vocab, "llama_model_get_vocab");
    LOAD_SYM(tokenize, "llama_tokenize");
    LOAD_SYM(token_to_piece, "llama_token_to_piece");
    LOAD_SYM(batch_init, "llama_batch_init");
    LOAD_SYM(batch_free, "llama_batch_free");
    LOAD_SYM(decode, "llama_decode");
    LOAD_SYM(sampler_init_greedy, "llama_sampler_init_greedy");
    LOAD_SYM(sampler_free, "llama_sampler_free");
    LOAD_SYM(sampler_sample, "llama_sampler_sample");
    LOAD_SYM(sampler_accept, "llama_sampler_accept");
    LOAD_SYM(vocab_is_eog, "llama_vocab_is_eog");

#undef LOAD_SYM
    return 1;
}

static int progress_cb(uint32_t layer_id,
                       uint32_t total,
                       const ModelLayerRecord *rec,
                       void *user_data)
{
    (void)user_data;
    if (layer_id % 8 == 0 || layer_id + 1 == total) {
        uint32_t pct = (layer_id + 1u) * 100u / (total ? total : 1u);
        fprintf(stderr, "\r[pogls] stream %3u%%  layer %4u/%-4u  %s   ",
                pct, layer_id + 1u, total, (const char *)rec->name);
        fflush(stderr);
    }
    return 0;
}

static int run_llama_stream_demo(const char *gguf_path, int n_tokens, const char *base_prompt)
{
    int rc = 0;
    const char *prompt_input = (base_prompt && *base_prompt)
        ? base_prompt
        : "Say one short sentence about chunk streaming.";
    char final_prompt[RUNNER_PROMPT_MAX];
    char reply[RUNNER_CHAT_REPLY_MAX];
    LlamaSession session;

    if (!llama_session_init(&session, gguf_path))
        return 1;

    if (!build_turn_prompt(NULL, 0, prompt_input, final_prompt, sizeof(final_prompt))) {
        llama_session_free(&session);
        return 1;
    }

    rc = run_streamed_turn(&session, final_prompt, n_tokens, reply, sizeof(reply));
    llama_session_free(&session);
    return rc;
}

static int run_chat_mode(const char *gguf_path)
{
    int rc = 0;
    LlamaSession session;
    ChatTurn turns[RUNNER_CHAT_TURNS];
    size_t turn_count = 0;

    memset(turns, 0, sizeof(turns));

    if (!llama_session_init(&session, gguf_path))
        return 1;

    fprintf(stdout, "[chat] ready. Type 'exit' or 'quit' to stop.\n");
    fflush(stdout);

    for (;;) {
        char user[RUNNER_CHAT_INPUT_MAX];
        char prompt[RUNNER_CHAT_PROMPT_MAX];
        char reply[RUNNER_CHAT_REPLY_MAX];

        fprintf(stdout, "\n[user] ");
        fflush(stdout);
        if (!fgets(user, sizeof(user), stdin))
            break;
        trim_newline(user);

        char *p = user;
        while (*p && isspace((unsigned char)*p))
            ++p;
        if (p != user)
            memmove(user, p, strlen(p) + 1);

        size_t user_len = strlen(user);
        while (user_len > 0 && isspace((unsigned char)user[user_len - 1]))
            user[--user_len] = '\0';

        if (!*user)
            continue;
        if (strcmp(user, "exit") == 0 || strcmp(user, "quit") == 0)
            break;

        if (!build_turn_prompt(turns, turn_count, user, prompt, sizeof(prompt))) {
            fprintf(stderr, "[chat] prompt build failed\n");
            rc = 1;
            break;
        }

        reply[0] = '\0';
        if (run_streamed_turn(&session, prompt, 128, reply, sizeof(reply)) != 0) {
            rc = 1;
            break;
        }

        if (!store_turn(turns, &turn_count, user, reply)) {
            fprintf(stderr, "[chat] failed to store conversation turn\n");
            rc = 1;
            break;
        }
    }

    llama_session_free(&session);
    return rc;
}

static int run_browser_chat_mode(const char *gguf_path)
{
    int rc = 0;
    LlamaSession session;

    if (!llama_session_init(&session, gguf_path))
        return 1;

    fprintf(stdout, "[browser-chat] ready\n");
    fflush(stdout);

    for (;;) {
        char header[128];
        int n_tokens = 0;
        unsigned long long prompt_len = 0;
        char *endptr = NULL;

        if (!fgets(header, sizeof(header), stdin))
            break;

        trim_newline(header);
        if (!*header)
            continue;
        if (strcmp(header, "exit") == 0 || strcmp(header, "quit") == 0)
            break;
        if (strncmp(header, "TURN ", 5) != 0) {
            fprintf(stderr, "[browser-chat] bad header: %s\n", header);
            fprintf(stdout, "[[[TURN_ERROR]]]\n");
            fflush(stdout);
            continue;
        }
        char *cursor = header + 5;
        n_tokens = (int)strtol(cursor, &endptr, 10);
        if (!endptr || endptr == cursor) {
            fprintf(stderr, "[browser-chat] bad token count: %s\n", header);
            fprintf(stdout, "[[[TURN_ERROR]]]\n");
            fflush(stdout);
            continue;
        }
        while (*endptr && isspace((unsigned char)*endptr))
            ++endptr;
        char *len_start = endptr;
        prompt_len = strtoull(endptr, &endptr, 10);
        if (endptr == NULL || endptr == len_start) {
            fprintf(stderr, "[browser-chat] bad prompt length: %s\n", header);
            fprintf(stdout, "[[[TURN_ERROR]]]\n");
            fflush(stdout);
            continue;
        }
        if (n_tokens <= 0)
            n_tokens = 128;
        if (prompt_len == 0 || prompt_len > RUNNER_BROWSER_PROMPT_MAX) {
            fprintf(stderr, "[browser-chat] invalid prompt length: %llu\n", prompt_len);
            fprintf(stdout, "[[[TURN_ERROR]]]\n");
            fflush(stdout);
            continue;
        }

        char *prompt = (char *)malloc((size_t)prompt_len + 1u);
        if (!prompt) {
            fprintf(stderr, "[browser-chat] malloc failed for prompt\n");
            fprintf(stdout, "[[[TURN_ERROR]]]\n");
            fflush(stdout);
            continue;
        }
        if (fread(prompt, 1, (size_t)prompt_len, stdin) != (size_t)prompt_len) {
            fprintf(stderr, "[browser-chat] prompt read failed\n");
            free(prompt);
            fprintf(stdout, "[[[TURN_ERROR]]]\n");
            fflush(stdout);
            break;
        }
        prompt[(size_t)prompt_len] = '\0';

        char reply[RUNNER_CHAT_REPLY_MAX];
        reply[0] = '\0';
        rc = run_streamed_turn(&session, prompt, n_tokens, reply, sizeof(reply));
        free(prompt);

        fprintf(stdout, "[[[TURN_END]]]\n");
        fflush(stdout);

        if (rc != 0)
            fprintf(stderr, "[browser-chat] turn failed rc=%d\n", rc);
    }

    llama_session_free(&session);
    return rc;
}

int pogls_runner_main(const char *gguf_path, int n_stream_tokens, const char *prompt_override)
{
    char model_name[64];
    derive_model_name(gguf_path, model_name, sizeof(model_name));

    uint64_t win_sz = 0;
    uint32_t n_layers = 0;
    int pf = pogls_backend_preflight(gguf_path, model_name, 0, &win_sz, &n_layers);
    if (pf != POGLS_BACK_OK && pf != POGLS_BACK_ERR_WINSZ) {
        fprintf(stderr, "[runner] preflight failed rc=%d\n", pf);
        return 1;
    }

    fprintf(stderr, "[runner] %u layers, max layer = %" PRIu64 " KB\n",
            n_layers, win_sz / 1024ull);

    uint8_t *window = (uint8_t *)malloc((size_t)win_sz);
    if (!window) {
        fprintf(stderr, "[runner] malloc failed (%" PRIu64 " bytes)\n",
                win_sz);
        return 1;
    }

    PoglsBackendCfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.gguf_path = gguf_path;
    cfg.model_name = model_name;
    cfg.ram_window = window;
    cfg.ram_window_size = win_sz;
    cfg.layer_cb = progress_cb;
    cfg.layer_start = 0;
    cfg.layer_end = 0;

    PoglsBackendStats st;
    uint64_t t0 = now_ms();
    int run_rc = pogls_backend_run(&cfg, &st);
    uint64_t t1 = now_ms();
    fprintf(stderr, "\n");

    if (run_rc != POGLS_BACK_OK) {
        fprintf(stderr, "[runner] backend_run failed rc=%d\n", run_rc);
        free(window);
        return 1;
    }

    fprintf(stderr, "[runner] stream done: %" PRIu64 " ms  ",
            t1 - t0);
    pogls_backend_print_stats(&st);

    if (n_stream_tokens > 0) {
        fprintf(stderr, "[runner] llama stream demo: %d tokens\n", n_stream_tokens);
        if (run_llama_stream_demo(gguf_path, n_stream_tokens, prompt_override) != 0) {
            free(window);
            return 1;
        }
    }

    free(window);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <model.gguf> [stream_tokens=32] [prompt]\n", argv[0]);
        fprintf(stderr, "       %s <model.gguf> --chat\n", argv[0]);
        fprintf(stderr, "       %s <model.gguf> --browser-chat\n", argv[0]);
        return 1;
    }

    if (argc >= 3 && (strcmp(argv[2], "--chat") == 0 || strcmp(argv[2], "-c") == 0))
        return run_chat_mode(argv[1]);
    if (argc >= 3 && strcmp(argv[2], "--browser-chat") == 0)
        return run_browser_chat_mode(argv[1]);

    int stream_tokens = (argc >= 3) ? atoi(argv[2]) : 32;
    const char *prompt = (argc >= 4) ? argv[3] : NULL;
    return pogls_runner_main(argv[1], stream_tokens, prompt);
}
