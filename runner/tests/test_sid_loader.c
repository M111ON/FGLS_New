/* test_sid_loader.c — Verify GGUF tensor index + loader (Module A + B)
 * Reads Qwen 0.5B GGUF, verifies all 291 tensors are detected.
 * No llama dependency — standalone test.
 */
#include <stdio.h>
#include <string.h>
#include "../gguf_index.h"
#include "../sid_loader.h"

static int n_pass = 0, n_fail = 0;
#define TEST(name, cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", name); n_fail++; } \
    else { printf("PASS: %s\n", name); n_pass++; } \
} while(0)

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";

    /* Module A: GGUF index reader */
    GGUFTensorIndex idx;
    TEST("gguf_idx_open", gguf_idx_open(path, &idx) == 0);
    TEST("n_tensors > 0", idx.n_tensors > 0);
    printf("  Tensors: %llu\n", (unsigned long long)idx.n_tensors);

    /* Check all tensors have names, valid offsets */
    uint64_t n_q80 = 0, n_f32 = 0;
    for (uint64_t i = 0; i < idx.n_tensors; i++) {
        TEST("name not null", idx.names[i] != NULL && idx.names[i][0] != 0);
        TEST("valid offset", idx.offsets[i] > 0);
        TEST("valid size", idx.sizes[i] > 0);
        if (idx.dtypes[i] == GGUF_Q8_0) n_q80++;
        else if (idx.dtypes[i] == GGUF_F32) n_f32++;
    }
    printf("  Q8_0: %llu, F32: %llu\n", (unsigned long long)n_q80, (unsigned long long)n_f32);

    TEST("q80 count", n_q80 == 170);
    TEST("f32 count", n_f32 == 121);

    /* Module B: sid_loader */
    SIDCache cache;
    sid_cache_init(&cache, 64 * 1024 * 1024);

    SIDLoaderCtx loader;
    TEST("sid_loader_open", sid_loader_open(&loader, path, &cache) == 0);
    TEST("loader n_tensors", loader.n_tensors == idx.n_tensors);

    /* Verify find + info for known tensors */
    SIDLoaderTensor info;
    TEST("find blk.0.attn_q.weight",
         sid_loader_info(&loader, "blk.0.attn_q.weight", &info) == 0);
    TEST("blk.0 dtype q80", info.dtype == GGUF_Q8_0);

    TEST("find blk.0.attn_norm.weight",
         sid_loader_info(&loader, "blk.0.attn_norm.weight", &info) == 0);
    TEST("norm dtype f32", info.dtype == GGUF_F32);

    /* Verify read */
    uint8_t *buf = (uint8_t*)malloc(1024);
    TEST("read by name", sid_loader_read_by_name(&loader, "token_embd.weight", buf) == 0);
    free(buf);

    sid_loader_close(&loader);
    gguf_idx_close(&idx);

    printf("\nResults: %d/%d pass, %d fail\n", n_pass, n_pass + n_fail, n_fail);
    return n_fail ? 1 : 0;
}
