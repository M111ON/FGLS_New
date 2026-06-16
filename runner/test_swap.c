// test_swap.c — Stage 3: 1 tensor, 1 byte flip, compare logits
// Build:
//   gcc -O2 -std=c11 -I. -II:/llama.cpp/include -II:/llama.cpp/ggml/include \
//       -o test_swap.exe test_swap.c \
//       I:/llama/llama-b9528-bin-win-vulkan-x64/llama.dll \
//       I:/llama/llama-b9528-bin-win-vulkan-x64/ggml.dll \
//       I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-base.dll \
//       I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-x64.dll -lm

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include "llama.h"

typedef int (*sid_find_fn)(void*, const char*, void**, void**, size_t*);

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "Usage: test_swap model.gguf\n"); return 1; }
    const char *model_path = argv[1];
    const char *target = argc > 2 ? argv[2] : "blk.0.attn_q.weight";
    const char *prompt  = argc > 3 ? argv[3] : "The meaning of life is";

    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");

    // --- Load model ---
    struct llama_model_params mp = llama_model_default_params();
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) { fprintf(stderr, "ERROR: model load\n"); return 1; }
    fprintf(stderr, "[swap] model loaded\n");

    // --- Load helper DLL ---
    HMODULE h = LoadLibraryA("sid_tensor_mingw.dll");
    if (!h) { fprintf(stderr, "ERROR: load DLL\n"); return 1; }
    sid_find_fn find_fn = (sid_find_fn)GetProcAddress(h, "sid_tensor_find");
    if (!find_fn) { fprintf(stderr, "ERROR: symbol\n"); return 1; }

    // --- Find target tensor ---
    void *tensor_ptr = NULL, *tensor_data = NULL;
    size_t tensor_nbytes = 0;
    if (find_fn(model, target, &tensor_ptr, &tensor_data, &tensor_nbytes) != 0) {
        fprintf(stderr, "ERROR: tensor '%s' not found\n", target); return 1;
    }
    fprintf(stderr, "[swap] target '%s': ptr=%p data=%p nbytes=%zu\n",
        target, tensor_ptr, tensor_data, tensor_nbytes);

    // --- Tokenize prompt ---
    // NOTE: llama_tokenize in b9528 DLL throws MSVC C++ exceptions.
    // MinGW can't catch these, so we hardcode common token IDs for the prompt.
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    llama_token tokens[1024];
    int n_tokens;
    { // Use a single BOS token or minimal valid token
        n_tokens = 1;
        tokens[0] = 151643; // BOS token for this model
    }
    fprintf(stderr, "[swap] prompt '%s' → %d tokens (hardcoded)\n", prompt, n_tokens);

    // --- Inference parameters ---
    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 512;
    cparams.n_batch = n_tokens;

    // ===== BASELINE RUN =====
    fprintf(stderr, "[swap] === baseline run ===\n");
    struct llama_context *ctx = llama_init_from_model(model, cparams);
    if (!ctx) { fprintf(stderr, "ERROR: context init\n"); return 1; }

    llama_batch batch = llama_batch_get_one(tokens, n_tokens);
    if (llama_decode(ctx, batch) != 0) {
        fprintf(stderr, "ERROR: baseline decode failed\n"); llama_free(ctx); return 1;
    }

    int n_vocab = llama_vocab_n_tokens(vocab);
    float *baseline = (float*)malloc(n_vocab * sizeof(float));
    if (!baseline) { fprintf(stderr, "ERROR: alloc\n"); return 1; }
    {
        float *logits = llama_get_logits_ith(ctx, 0);
        memcpy(baseline, logits, n_vocab * sizeof(float));
    }
    fprintf(stderr, "[swap] baseline logits[0..4] = %.6f %.6f %.6f %.6f %.6f\n",
        baseline[0], baseline[1], baseline[2], baseline[3], baseline[4]);
    llama_free(ctx);

    // ===== FLIP 1 BYTE =====
    uint8_t *target_bytes = (uint8_t*)tensor_data;
    uint8_t orig_byte = target_bytes[0];
    target_bytes[0] ^= 1;  // flip LSB of first byte
    fprintf(stderr, "[swap] flipped byte at data+0: 0x%02x → 0x%02x\n",
        orig_byte, target_bytes[0]);

    // ===== TEST RUN (after flip) =====
    fprintf(stderr, "[swap] === test run (1 byte flipped) ===\n");
    ctx = llama_init_from_model(model, cparams);
    if (!ctx) { fprintf(stderr, "ERROR: context init\n"); return 1; }

    if (llama_decode(ctx, batch) != 0) {
        fprintf(stderr, "ERROR: test decode failed\n"); llama_free(ctx); return 1;
    }

    float *test_logits = (float*)malloc(n_vocab * sizeof(float));
    if (!test_logits) { fprintf(stderr, "ERROR: alloc\n"); return 1; }
    {
        float *logits = llama_get_logits_ith(ctx, 0);
        memcpy(test_logits, logits, n_vocab * sizeof(float));
    }
    fprintf(stderr, "[swap] test    logits[0..4] = %.6f %.6f %.6f %.6f %.6f\n",
        test_logits[0], test_logits[1], test_logits[2], test_logits[3], test_logits[4]);
    llama_free(ctx);

    // ===== COMPARE =====
    int n_diff = 0;
    double max_diff = 0.0, sum_sq = 0.0;
    for (int i = 0; i < n_vocab; i++) {
        double d = (double)test_logits[i] - (double)baseline[i];
        if (fabs(d) > 1e-10) n_diff++;
        if (fabs(d) > max_diff) max_diff = fabs(d);
        sum_sq += d * d;
    }
    double rmse = sqrt(sum_sq / n_vocab);

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "[swap] COMPARISON RESULT\n");
    fprintf(stderr, "  tensor:     %s\n", target);
    fprintf(stderr, "  operation:  byte[0] ^= 1 (%02x → %02x)\n", orig_byte, target_bytes[0]);
    fprintf(stderr, "  vocab size: %d\n", n_vocab);
    fprintf(stderr, "  diff count: %d / %d (%.2f%%)\n", n_diff, n_vocab, 100.0 * n_diff / n_vocab);
    fprintf(stderr, "  max diff:   %g\n", max_diff);
    fprintf(stderr, "  RMSE:       %g\n", rmse);

    if (n_diff > 0) {
        fprintf(stderr, "\n  *** VERDICT: CPU READS tensor->data FRESH ***\n");
        fprintf(stderr, "  Stage 3: PASSED — pointer swap works at runtime\n");
    } else {
        fprintf(stderr, "\n  *** VERDICT: backend CACHED pointer ***\n");
        fprintf(stderr, "  Stage 3: FAILED — need invalidation mechanism\n");
    }
    fprintf(stderr, "========================================\n");

    // ===== RESTORE =====
    target_bytes[0] = orig_byte;
    fprintf(stderr, "[swap] byte restored\n");

    // Cleanup
    free(baseline);
    free(test_logits);
    FreeLibrary(h);
    llama_model_free(model);
    llama_backend_free();
    return n_diff > 0 ? 0 : 1;
}
