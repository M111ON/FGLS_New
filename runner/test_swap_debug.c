// test_swap_debug.c — Multi-tensor swap test using model-struct scan
// Verifies that ALL model tensors can be found and swapped, and that
// the CPU backend reads the swapped data for inference.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <windows.h>
#include "llama.h"

#define MAX_TENSORS 512
#define TENSOR_NAME_MAX 64

// DLL exports
typedef int (*sid_enum_model_fn)(void*, void**, char(*)[64], void**, size_t*, int);
typedef int (*sid_find_model_fn)(void*, const char*, void**, void**, size_t*);

// Qwen2.5 0.5B has 291 tensors: 3 direct + 24 layers * 12 per layer
static const char *KNOWN_TENSOR_NAMES[] = {
    "token_embd.weight", "output_norm.weight", "output.weight",
    "blk.0.attn_norm.weight", "blk.0.attn_q.weight", "blk.0.attn_k.weight", "blk.0.attn_v.weight",
    "blk.0.attn_q.bias", "blk.0.attn_k.bias", "blk.0.attn_v.bias", "blk.0.attn_output.weight",
    "blk.0.ffn_norm.weight", "blk.0.ffn_gate.weight", "blk.0.ffn_down.weight", "blk.0.ffn_up.weight",
    "blk.1.attn_norm.weight", "blk.1.attn_q.weight", "blk.1.attn_k.weight", "blk.1.attn_v.weight",
    "blk.1.attn_q.bias", "blk.1.attn_k.bias", "blk.1.attn_v.bias", "blk.1.attn_output.weight",
    "blk.1.ffn_norm.weight", "blk.1.ffn_gate.weight", "blk.1.ffn_down.weight", "blk.1.ffn_up.weight",
    "blk.2.attn_norm.weight", "blk.2.attn_q.weight", "blk.2.attn_k.weight", "blk.2.attn_v.weight",
    "blk.2.attn_q.bias", "blk.2.attn_k.bias", "blk.2.attn_v.bias", "blk.2.attn_output.weight",
    "blk.2.ffn_norm.weight", "blk.2.ffn_gate.weight", "blk.2.ffn_down.weight", "blk.2.ffn_up.weight",
    "blk.3.attn_norm.weight", "blk.3.attn_q.weight", "blk.3.attn_k.weight", "blk.3.attn_v.weight",
    "blk.3.attn_q.bias", "blk.3.attn_k.bias", "blk.3.attn_v.bias", "blk.3.attn_output.weight",
    "blk.3.ffn_norm.weight", "blk.3.ffn_gate.weight", "blk.3.ffn_down.weight", "blk.3.ffn_up.weight",
    "blk.4.attn_norm.weight", "blk.4.attn_q.weight", "blk.4.attn_k.weight", "blk.4.attn_v.weight",
    "blk.4.attn_q.bias", "blk.4.attn_k.bias", "blk.4.attn_v.bias", "blk.4.attn_output.weight",
    "blk.4.ffn_norm.weight", "blk.4.ffn_gate.weight", "blk.4.ffn_down.weight", "blk.4.ffn_up.weight",
    "blk.5.attn_norm.weight", "blk.5.attn_q.weight", "blk.5.attn_k.weight", "blk.5.attn_v.weight",
    "blk.5.attn_q.bias", "blk.5.attn_k.bias", "blk.5.attn_v.bias", "blk.5.attn_output.weight",
    "blk.5.ffn_norm.weight", "blk.5.ffn_gate.weight", "blk.5.ffn_down.weight", "blk.5.ffn_up.weight",
    "blk.6.attn_norm.weight", "blk.6.attn_q.weight", "blk.6.attn_k.weight", "blk.6.attn_v.weight",
    "blk.6.attn_q.bias", "blk.6.attn_k.bias", "blk.6.attn_v.bias", "blk.6.attn_output.weight",
    "blk.6.ffn_norm.weight", "blk.6.ffn_gate.weight", "blk.6.ffn_down.weight", "blk.6.ffn_up.weight",
    "blk.7.attn_norm.weight", "blk.7.attn_q.weight", "blk.7.attn_k.weight", "blk.7.attn_v.weight",
    "blk.7.attn_q.bias", "blk.7.attn_k.bias", "blk.7.attn_v.bias", "blk.7.attn_output.weight",
    "blk.7.ffn_norm.weight", "blk.7.ffn_gate.weight", "blk.7.ffn_down.weight", "blk.7.ffn_up.weight",
    "blk.8.attn_norm.weight", "blk.8.attn_q.weight", "blk.8.attn_k.weight", "blk.8.attn_v.weight",
    "blk.8.attn_q.bias", "blk.8.attn_k.bias", "blk.8.attn_v.bias", "blk.8.attn_output.weight",
    "blk.8.ffn_norm.weight", "blk.8.ffn_gate.weight", "blk.8.ffn_down.weight", "blk.8.ffn_up.weight",
    "blk.9.attn_norm.weight", "blk.9.attn_q.weight", "blk.9.attn_k.weight", "blk.9.attn_v.weight",
    "blk.9.attn_q.bias", "blk.9.attn_k.bias", "blk.9.attn_v.bias", "blk.9.attn_output.weight",
    "blk.9.ffn_norm.weight", "blk.9.ffn_gate.weight", "blk.9.ffn_down.weight", "blk.9.ffn_up.weight",
    "blk.10.attn_norm.weight", "blk.10.attn_q.weight", "blk.10.attn_k.weight", "blk.10.attn_v.weight",
    "blk.10.attn_q.bias", "blk.10.attn_k.bias", "blk.10.attn_v.bias", "blk.10.attn_output.weight",
    "blk.10.ffn_norm.weight", "blk.10.ffn_gate.weight", "blk.10.ffn_down.weight", "blk.10.ffn_up.weight",
    "blk.11.attn_norm.weight", "blk.11.attn_q.weight", "blk.11.attn_k.weight", "blk.11.attn_v.weight",
    "blk.11.attn_q.bias", "blk.11.attn_k.bias", "blk.11.attn_v.bias", "blk.11.attn_output.weight",
    "blk.11.ffn_norm.weight", "blk.11.ffn_gate.weight", "blk.11.ffn_down.weight", "blk.11.ffn_up.weight",
    "blk.12.attn_norm.weight", "blk.12.attn_q.weight", "blk.12.attn_k.weight", "blk.12.attn_v.weight",
    "blk.12.attn_q.bias", "blk.12.attn_k.bias", "blk.12.attn_v.bias", "blk.12.attn_output.weight",
    "blk.12.ffn_norm.weight", "blk.12.ffn_gate.weight", "blk.12.ffn_down.weight", "blk.12.ffn_up.weight",
    "blk.13.attn_norm.weight", "blk.13.attn_q.weight", "blk.13.attn_k.weight", "blk.13.attn_v.weight",
    "blk.13.attn_q.bias", "blk.13.attn_k.bias", "blk.13.attn_v.bias", "blk.13.attn_output.weight",
    "blk.13.ffn_norm.weight", "blk.13.ffn_gate.weight", "blk.13.ffn_down.weight", "blk.13.ffn_up.weight",
    "blk.14.attn_norm.weight", "blk.14.attn_q.weight", "blk.14.attn_k.weight", "blk.14.attn_v.weight",
    "blk.14.attn_q.bias", "blk.14.attn_k.bias", "blk.14.attn_v.bias", "blk.14.attn_output.weight",
    "blk.14.ffn_norm.weight", "blk.14.ffn_gate.weight", "blk.14.ffn_down.weight", "blk.14.ffn_up.weight",
    "blk.15.attn_norm.weight", "blk.15.attn_q.weight", "blk.15.attn_k.weight", "blk.15.attn_v.weight",
    "blk.15.attn_q.bias", "blk.15.attn_k.bias", "blk.15.attn_v.bias", "blk.15.attn_output.weight",
    "blk.15.ffn_norm.weight", "blk.15.ffn_gate.weight", "blk.15.ffn_down.weight", "blk.15.ffn_up.weight",
    "blk.16.attn_norm.weight", "blk.16.attn_q.weight", "blk.16.attn_k.weight", "blk.16.attn_v.weight",
    "blk.16.attn_q.bias", "blk.16.attn_k.bias", "blk.16.attn_v.bias", "blk.16.attn_output.weight",
    "blk.16.ffn_norm.weight", "blk.16.ffn_gate.weight", "blk.16.ffn_down.weight", "blk.16.ffn_up.weight",
    "blk.17.attn_norm.weight", "blk.17.attn_q.weight", "blk.17.attn_k.weight", "blk.17.attn_v.weight",
    "blk.17.attn_q.bias", "blk.17.attn_k.bias", "blk.17.attn_v.bias", "blk.17.attn_output.weight",
    "blk.17.ffn_norm.weight", "blk.17.ffn_gate.weight", "blk.17.ffn_down.weight", "blk.17.ffn_up.weight",
    "blk.18.attn_norm.weight", "blk.18.attn_q.weight", "blk.18.attn_k.weight", "blk.18.attn_v.weight",
    "blk.18.attn_q.bias", "blk.18.attn_k.bias", "blk.18.attn_v.bias", "blk.18.attn_output.weight",
    "blk.18.ffn_norm.weight", "blk.18.ffn_gate.weight", "blk.18.ffn_down.weight", "blk.18.ffn_up.weight",
    "blk.19.attn_norm.weight", "blk.19.attn_q.weight", "blk.19.attn_k.weight", "blk.19.attn_v.weight",
    "blk.19.attn_q.bias", "blk.19.attn_k.bias", "blk.19.attn_v.bias", "blk.19.attn_output.weight",
    "blk.19.ffn_norm.weight", "blk.19.ffn_gate.weight", "blk.19.ffn_down.weight", "blk.19.ffn_up.weight",
    "blk.20.attn_norm.weight", "blk.20.attn_q.weight", "blk.20.attn_k.weight", "blk.20.attn_v.weight",
    "blk.20.attn_q.bias", "blk.20.attn_k.bias", "blk.20.attn_v.bias", "blk.20.attn_output.weight",
    "blk.20.ffn_norm.weight", "blk.20.ffn_gate.weight", "blk.20.ffn_down.weight", "blk.20.ffn_up.weight",
    "blk.21.attn_norm.weight", "blk.21.attn_q.weight", "blk.21.attn_k.weight", "blk.21.attn_v.weight",
    "blk.21.attn_q.bias", "blk.21.attn_k.bias", "blk.21.attn_v.bias", "blk.21.attn_output.weight",
    "blk.21.ffn_norm.weight", "blk.21.ffn_gate.weight", "blk.21.ffn_down.weight", "blk.21.ffn_up.weight",
    "blk.22.attn_norm.weight", "blk.22.attn_q.weight", "blk.22.attn_k.weight", "blk.22.attn_v.weight",
    "blk.22.attn_q.bias", "blk.22.attn_k.bias", "blk.22.attn_v.bias", "blk.22.attn_output.weight",
    "blk.22.ffn_norm.weight", "blk.22.ffn_gate.weight", "blk.22.ffn_down.weight", "blk.22.ffn_up.weight",
    "blk.23.attn_norm.weight", "blk.23.attn_q.weight", "blk.23.attn_k.weight", "blk.23.attn_v.weight",
    "blk.23.attn_q.bias", "blk.23.attn_k.bias", "blk.23.attn_v.bias", "blk.23.attn_output.weight",
    "blk.23.ffn_norm.weight", "blk.23.ffn_gate.weight", "blk.23.ffn_down.weight", "blk.23.ffn_up.weight",
};
#define N_KNOWN_TENSORS (sizeof(KNOWN_TENSOR_NAMES) / sizeof(KNOWN_TENSOR_NAMES[0]))

// Read/write tensor->data at offset 248 (ggml_tensor layout)
static void* read_tensor_data(void *tensor) {
    void *data = NULL;
    memcpy(&data, (char*)tensor + 248, sizeof(void*));
    return data;
}
static void write_tensor_data(void *tensor, void *new_data) {
    memcpy((char*)tensor + 248, &new_data, sizeof(void*));
}

// Error codes
#define ERR_OK        0
#define ERR_MODEL    -1
#define ERR_DLL      -2
#define ERR_DECODE   -3
#define ERR_NOMEM    -4

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <model.gguf> [--enum-only] [--swap-tensor name]\n", argv[0]);
        return 1;
    }
    const char *model_path = argv[1];
    int mode_enum_only = 0;
    const char *swap_single = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--enum-only") == 0) mode_enum_only = 1;
        else if (strcmp(argv[i], "--swap-tensor") == 0 && i + 1 < argc)
            swap_single = argv[++i];
        else if (swap_single == NULL && argv[i][0] != '-')
            swap_single = argv[i];
    }

    // ----- Init -----
    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");
    struct llama_model_params mp = llama_model_default_params();
    fprintf(stderr, "[test] calling llama_model_load_from_file...\n"); fflush(stderr);
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    fprintf(stderr, "[test] model=%p\n", (void*)model); fflush(stderr);
    if (!model) { fprintf(stderr, "ERROR: model load\n"); return ERR_MODEL; }

    fprintf(stderr, "[test] loading DLL...\n"); fflush(stderr);
    HMODULE h = LoadLibraryA("sid_tensor_helper.dll");
    fprintf(stderr, "[test] DLL=%p\n", (void*)h); fflush(stderr);
    if (!h) { fprintf(stderr, "ERROR: DLL load\n"); return ERR_DLL; }

    sid_enum_model_fn enum_fn = (sid_enum_model_fn)GetProcAddress(h, "sid_tensor_enum_model");
    if (!enum_fn) { fprintf(stderr, "ERROR: sid_tensor_enum_model not found\n"); return ERR_DLL; }

    // ----- Use sid_tensor_find_in_model to locate ALL model-struct tensors -----
    sid_find_model_fn find_fn = (sid_find_model_fn)GetProcAddress(h, "sid_tensor_find_in_model");
    if (!find_fn) { fprintf(stderr, "ERROR: sid_tensor_find_in_model not found\n"); return ERR_DLL; }
    fprintf(stderr, "[test] find_fn=%p\n", (void*)find_fn); fflush(stderr);

    void *tensor_ptrs[MAX_TENSORS] = {0};
    void *tensor_datas[MAX_TENSORS] = {0};
    size_t tensor_nbytes[MAX_TENSORS] = {0};
    char tensor_names[MAX_TENSORS][TENSOR_NAME_MAX] = {{0}};
    int n_tensors = 0;

    for (int i = 0; i < N_KNOWN_TENSORS && n_tensors < MAX_TENSORS; i++) {
        void *ptr = NULL, *data = NULL; size_t nbytes = 0;
        int ret = find_fn(model, KNOWN_TENSOR_NAMES[i], &ptr, &data, &nbytes);
        if (ret == 0 && ptr && data) {
            tensor_ptrs[n_tensors] = ptr;
            tensor_datas[n_tensors] = data;
            tensor_nbytes[n_tensors] = nbytes;
            strncpy(tensor_names[n_tensors], KNOWN_TENSOR_NAMES[i], TENSOR_NAME_MAX - 1);
            tensor_names[n_tensors][TENSOR_NAME_MAX - 1] = 0;
            n_tensors++;
        }
    }
    fprintf(stderr, "[test] enumerated %d tensors\n", n_tensors);
    if (n_tensors <= 0) { fprintf(stderr, "ERROR: no tensors found\n"); return ERR_DLL; }

    // Print enumeration
    size_t total_bytes = 0;
    for (int i = 0; i < n_tensors && i < 10; i++) {
        // Sanitize name for display
        char name_clean[65]; memcpy(name_clean, tensor_names[i], 64); name_clean[64]=0;
        for (int c = 0; name_clean[c]; c++)
            if (name_clean[c] < 32 || name_clean[c] > 126) name_clean[c] = '.';
        fprintf(stderr, "[test]   [%d] ptr=%p data=%p bytes=%zu name=%s\n",
            i, tensor_ptrs[i], tensor_datas[i], tensor_nbytes[i], name_clean);
    }
    if (n_tensors > 10)
        fprintf(stderr, "[test]   ... and %d more\n", n_tensors - 10);
    for (int i = 0; i < n_tensors; i++) total_bytes += tensor_nbytes[i];
    fprintf(stderr, "[test] total tensor data: %.2f MB\n", total_bytes / 1048576.0);

    if (mode_enum_only) {
        fprintf(stderr, "\n[test] enum-only mode, exiting\n");
        FreeLibrary(h); llama_model_free(model); llama_backend_free();
        return ERR_OK;
    }

    // ----- Prepare swap — ALL tensors -----
    int swap_indices[MAX_TENSORS];
    int swap_count = 0;
    for (int i = 0; i < n_tensors; i++) {
        if (tensor_nbytes[i] > 0 && tensor_nbytes[i] <= (size_t)2 * 1024 * 1024 * 1024 &&
            tensor_datas[i] != NULL) {
            swap_indices[swap_count++] = i;
        }
    }
    fprintf(stderr, "[test] will swap %d/%d tensors\n", swap_count, n_tensors);

    // Allocate copies with modification
    void **copies = (void**)malloc(swap_count * sizeof(void*));
    void **old_datas = (void**)malloc(swap_count * sizeof(void*));
    if (!copies || !old_datas) { fprintf(stderr, "ERROR: no mem\n"); return ERR_NOMEM; }

    size_t total_copy_bytes = 0;
    for (int si = 0; si < swap_count; si++) {
        int idx = swap_indices[si];
        copies[si] = malloc(tensor_nbytes[idx]);
        if (!copies[si]) { fprintf(stderr, "ERROR: malloc %zu failed\n", tensor_nbytes[idx]); return ERR_NOMEM; }
        memcpy(copies[si], tensor_datas[idx], tensor_nbytes[idx]);
        old_datas[si] = tensor_datas[idx];
        total_copy_bytes += tensor_nbytes[idx];
    }
    fprintf(stderr, "[test] copies allocated: %.2f MB total\n", total_copy_bytes / 1048576.0);

    // Modify first byte of last swap target to be detectably different
    int last_si = swap_count - 1;
    unsigned char orig_last = ((unsigned char*)copies[last_si])[0];
    ((unsigned char*)copies[last_si])[0] ^= 1;
    fprintf(stderr, "[test] last swap target (%s): flipped first byte 0x%02x->0x%02x\n",
        tensor_names[swap_indices[last_si]], orig_last, ((unsigned char*)copies[last_si])[0]);

    // ----- SWAP BEFORE any context creation -----
    for (int si = 0; si < swap_count; si++) {
        int idx = swap_indices[si];
        write_tensor_data(tensor_ptrs[idx], copies[si]);
    }

    // Verify first swap
    void *verify = read_tensor_data(tensor_ptrs[swap_indices[0]]);
    fprintf(stderr, "[test] swapped before context: old=%p -> new=%p\n",
        old_datas[0], verify);

    // ----- Baseline decode -----
    llama_token tokens[1] = {0};
    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 256;
    cparams.n_batch = 1;

    struct llama_context *ctx = llama_init_from_model(model, cparams);
    if (!ctx) { fprintf(stderr, "ERROR: context\n"); return ERR_DECODE; }

    llama_batch batch = llama_batch_get_one(tokens, 1);
    if (llama_decode(ctx, batch) != 0) { fprintf(stderr, "ERROR: baseline decode\n"); return ERR_DECODE; }

    int n_vocab = llama_vocab_n_tokens(llama_model_get_vocab(model));
    float *baseline = (float*)malloc(n_vocab * sizeof(float));
    {
        float *lp = llama_get_logits_ith(ctx, 0);
        memcpy(baseline, lp, n_vocab * sizeof(float));
    }
    fprintf(stderr, "[test] baseline logits[0]=%f logits[%d]=%f\n",
        baseline[0], n_vocab - 1, baseline[n_vocab - 1]);
    llama_free(ctx);

    // ----- Restore original data pointers -----
    for (int si = 0; si < swap_count; si++) {
        int idx = swap_indices[si];
        write_tensor_data(tensor_ptrs[idx], old_datas[si]);
    }
    fprintf(stderr, "[test] restored original data pointers\n");

    // ----- Test decode (with restored data) -----
    ctx = llama_init_from_model(model, cparams);
    if (!ctx) { fprintf(stderr, "ERROR: context test\n"); return ERR_DECODE; }
    if (llama_decode(ctx, batch) != 0) { fprintf(stderr, "ERROR: test decode\n"); return ERR_DECODE; }

    float *test = (float*)malloc(n_vocab * sizeof(float));
    {
        float *lp = llama_get_logits_ith(ctx, 0);
        memcpy(test, lp, n_vocab * sizeof(float));
    }
    fprintf(stderr, "[test] test     logits[0]=%f logits[%d]=%f\n",
        test[0], n_vocab - 1, test[n_vocab - 1]);

    // ----- Compare -----
    // If baseline differs from test: context reads tensor->data at init time
    // If baseline equals test: context caches data somewhere else
    int n_diff = 0;
    double max_diff = 0.0;
    for (int i = 0; i < n_vocab; i++) {
        double d = (double)test[i] - (double)baseline[i];
        if (fabs(d) > 1e-10) { n_diff++; if (fabs(d) > max_diff) max_diff = fabs(d); }
    }

    fprintf(stderr, "\n*** VERDICT: %s ***\n",
        n_diff > 0 ? "SWAP BEFORE CONTEXT WORKS" : "CONTEXT CACHES (swap before context fails)");
    fprintf(stderr, "diff: %d / %d, max_diff: %g\n", n_diff, n_vocab, max_diff);

    // ----- Cleanup -----
    llama_free(ctx);
    free(test); free(baseline);
    for (int si = 0; si < swap_count; si++) free(copies[si]);
    free(copies); free(old_datas);
    FreeLibrary(h);
    llama_model_free(model);
    llama_backend_free();
    return n_diff > 0 ? ERR_OK : 1;
}
