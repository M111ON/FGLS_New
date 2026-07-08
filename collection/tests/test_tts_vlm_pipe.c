/*
 * test_tts_vlm_pipe.c — TTS/VLM Pipe Integration Test Suite
 *
 * Tests:
 *   1. TTS pipe open/close lifecycle
 *   2. VLM pipe open/close lifecycle
 *   3. TTS tensor write→read roundtrip
 *   4. VLM tensor write→read roundtrip
 *   5. TTS tensor classification
 *   6. VLM tensor classification (vision vs text)
 *   7. VLM vision block iterator
 *   8. VLM text block iterator
 *   9. SID config verification
 *  10. Cross-pipe isolation
 *  11. Checkpoint/restore (Void Space)
 *  12. TTS group loader
 *  13. Large tensor (>1MB) stress test
 *  14. Multi-tensor sequential write→read→verify
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "../pipe/pipe_tts.h"
#include "../pipe/pipe_vlm.h"
#include "../pipe/pipe_service.h"

static int n_pass = 0;
static int n_fail = 0;

#define TEST(name) do { \
    printf("  %-40s ", name); \
    fflush(stdout); \
} while(0)

#define PASS() do { \
    printf("PASS\n"); \
    n_pass++; \
} while(0)

#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
    n_fail++; \
} while(0)

/* ── Helper: generate deterministic test data ─────────────────── */
static void fill_pattern(uint8_t *buf, uint32_t nbytes, uint32_t seed) {
    for (uint32_t i = 0; i < nbytes; i++) {
        buf[i] = (uint8_t)((seed + i * 7) & 0xFF);
    }
}

/* ── Helper: verify data integrity ───────────────────────────── */
static int verify_pattern(const uint8_t *buf, uint32_t nbytes, uint32_t seed) {
    for (uint32_t i = 0; i < nbytes; i++) {
        uint8_t expected = (uint8_t)((seed + i * 7) & 0xFF);
        if (buf[i] != expected) {
            printf("mismatch at byte %u: got %02X expected %02X\n",
                   i, buf[i], expected);
            return 0;
        }
    }
    return 1;
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 1: TTS pipe open/close lifecycle
 * ═══════════════════════════════════════════════════════════════ */
static void test_tts_lifecycle(void) {
    TEST("TTS lifecycle");

    PipeContext *tts = pipe_open_tts("test-kokoro", NULL);
    if (!tts) { FAIL("open returned NULL"); return; }
    if (pipe_count(tts) != 0) { FAIL("pipe not empty on open"); pipe_close(tts); return; }
    if (strcmp(pipe_name(tts), "test-kokoro") != 0) {
        FAIL("name mismatch"); pipe_close(tts); return;
    }

    pipe_close(tts);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 2: VLM pipe open/close lifecycle
 * ═══════════════════════════════════════════════════════════════ */
static void test_vlm_lifecycle(void) {
    TEST("VLM lifecycle");

    PipeContext *vlm = pipe_open_vlm("test-moondream2", NULL);
    if (!vlm) { FAIL("open returned NULL"); return; }
    if (pipe_count(vlm) != 0) { FAIL("pipe not empty on open"); pipe_close(vlm); return; }
    if (strcmp(pipe_name(vlm), "test-moondream2") != 0) {
        FAIL("name mismatch"); pipe_close(vlm); return;
    }

    pipe_close(vlm);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 3: TTS tensor write→read roundtrip
 * ═══════════════════════════════════════════════════════════════ */
static void test_tts_write_read(void) {
    TEST("TTS write/read");

    PipeContext *tts = pipe_open_tts("tts-rw", NULL);
    if (!tts) { FAIL("open failed"); return; }

    /* Write a simulated decoder tensor */
    uint32_t nbytes = 512 * 80 * 4;  /* 512x80 f32 weights */
    uint8_t *data = (uint8_t *)malloc(nbytes);
    fill_pattern(data, nbytes, 0xABCD);

    int rc = pipe_write_tts_tensor(tts, "decoder.istft.res_weight", data, nbytes);
    if (rc != PIPE_OK) { FAIL("write failed"); free(data); pipe_close(tts); return; }

    /* Read back */
    uint32_t got_bytes = 0;
    const void *ptr = pipe_read_tts_tensor(tts, "decoder.istft.res_weight", &got_bytes);
    if (!ptr) { FAIL("read returned NULL"); free(data); pipe_close(tts); return; }
    if (got_bytes != nbytes) { FAIL("size mismatch"); free(data); pipe_close(tts); return; }
    if (!verify_pattern((const uint8_t *)ptr, nbytes, 0xABCD)) {
        FAIL("data corruption"); free(data); pipe_close(tts); return;
    }

    /* Also test find */
    if (!pipe_find(tts, "decoder.istft.res_weight")) {
        FAIL("find failed after write"); free(data); pipe_close(tts); return;
    }
    if (pipe_find(tts, "nonexistent")) {
        FAIL("find false positive"); free(data); pipe_close(tts); return;
    }

    free(data);
    pipe_close(tts);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 4: VLM tensor write→read roundtrip
 * ═══════════════════════════════════════════════════════════════ */
static void test_vlm_write_read(void) {
    TEST("VLM write/read");

    PipeContext *vlm = pipe_open_vlm("vlm-rw", NULL);
    if (!vlm) { FAIL("open failed"); return; }

    /* Write a vision attention weight [1152, 3456] = ~7.9MB at BF16 = ~3.9MB f32 */
    uint32_t nbytes = 1152 * 3456 * 4;  /* f32 */
    uint8_t *data = (uint8_t *)malloc(nbytes);
    fill_pattern(data, nbytes, 0x1234);

    int rc = pipe_write_vlm_tensor(vlm, "model.vision.blocks.0.attn.qkv.weight",
                                    data, nbytes);
    if (rc != PIPE_OK) { FAIL("write failed"); free(data); pipe_close(vlm); return; }

    /* Write a text embedding tensor */
    uint32_t wte_bytes = 51200 * 2048 * 4;  /* large, but we'll simulate small */
    uint8_t *wte = (uint8_t *)malloc(8192);
    fill_pattern(wte, 8192, 0x5678);
    rc = pipe_write_vlm_tensor(vlm, "model.text.wte", wte, 8192);
    if (rc != PIPE_OK) { FAIL("wte write failed"); free(wte); free(data); pipe_close(vlm); return; }
    free(wte);

    /* Read back vision weight */
    uint32_t got = 0;
    const void *ptr = pipe_read_vlm_tensor(vlm, "model.vision.blocks.0.attn.qkv.weight", &got);
    if (!ptr) { FAIL("vision read NULL"); free(data); pipe_close(vlm); return; }
    if (got != nbytes) { FAIL("vision size mismatch"); free(data); pipe_close(vlm); return; }
    if (!verify_pattern((const uint8_t *)ptr, nbytes, 0x1234)) {
        FAIL("vision data corruption"); free(data); pipe_close(vlm); return;
    }

    /* Read back text wte */
    got = 0;
    ptr = pipe_read_vlm_tensor(vlm, "model.text.wte", &got);
    if (!ptr) { FAIL("text read NULL"); free(data); pipe_close(vlm); return; }
    if (got != 8192) { FAIL("text size mismatch"); free(data); pipe_close(vlm); return; }
    if (!verify_pattern((const uint8_t *)ptr, 8192, 0x5678)) {
        FAIL("text data corruption"); free(data); pipe_close(vlm); return;
    }

    free(data);
    pipe_close(vlm);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 5: TTS tensor classification
 * ═══════════════════════════════════════════════════════════════ */
static void test_tts_classify(void) {
    TEST("TTS classify");

    if (!tts_is_tts_tensor("bert.encoder.layer.0.attention.self.query.weight"))
        { FAIL("bert prefix missed"); return; }
    if (!tts_is_tts_tensor("bert_encoder.pos_embed"))
        { FAIL("bert_encoder prefix missed"); return; }
    if (!tts_is_tts_tensor("predictor.duration_proj.weight"))
        { FAIL("predictor prefix missed"); return; }
    if (!tts_is_tts_tensor("decoder.istft.res_weight"))
        { FAIL("decoder prefix missed"); return; }
    if (!tts_is_tts_tensor("text_encoder.embedding.weight"))
        { FAIL("text_encoder prefix missed"); return; }
    if (tts_is_tts_tensor("model.vision.blocks.0.attn.qkv.weight"))
        { FAIL("VLM tensor falsely classified as TTS"); return; }
    if (tts_is_tts_tensor(NULL))
        { FAIL("NULL falsely classified"); return; }

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 6: VLM tensor classification
 * ═══════════════════════════════════════════════════════════════ */
static void test_vlm_classify(void) {
    TEST("VLM classify");

    if (vlm_classify_tensor("model.vision.blocks.0.attn.qkv.weight") != 1)
        { FAIL("vision not classified"); return; }
    if (vlm_classify_tensor("model.text.blocks.0.attn.qkv.weight") != 0)
        { FAIL("text not classified"); return; }
    if (vlm_classify_tensor("decoder.istft.res_weight") != -1)
        { FAIL("TTS tensor falsely classified as VLM"); return; }
    if (vlm_classify_tensor(NULL) != -1)
        { FAIL("NULL not -1"); return; }
    if (!vlm_is_vlm_tensor("model.vision.patch_emb.weight"))
        { FAIL("vlm_is_vlm_tensor missed vision"); return; }
    if (vlm_is_vlm_tensor("bert.encoder.layer.0.weight"))
        { FAIL("TTS tensor falsely vlm"); return; }

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 7: VLM vision block iterator
 * ═══════════════════════════════════════════════════════════════ */
static int vision_count_cb(int block_idx, const char *name, void *user) {
    (void)name;
    (*(int *)user)++;
    return 0;
}

static void test_vlm_vision_iterator(void) {
    TEST("VLM vision iterator");

    int count = 0;
    int total = 0;
    for (int i = 0; i < VLM_VISION_N_LAYERS; i++) {
        int n = vlm_foreach_vision_block(i, vision_count_cb, &count);
        if (n < 0) { FAIL("vision iterator failed"); return; }
        total += n;
    }
    /* Each block has 12 sub-tensors (ln1 w/b, attn qkv w/b, attn proj w/b,
       ln2 w/b, mlp fc1 w/b, mlp fc2 w/b) */
    int expected = VLM_VISION_N_LAYERS * 12;
    if (total != expected) {
        printf("FAIL: got %d expected %d\n", total, expected);
        return;
    }
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 8: VLM text block iterator
 * ═══════════════════════════════════════════════════════════════ */
static int text_count_cb(int block_idx, const char *name, void *user) {
    (void)name;
    (*(int *)user)++;
    return 0;
}

static void test_vlm_text_iterator(void) {
    TEST("VLM text iterator");

    int count = 0;
    int total = 0;
    for (int i = 0; i < VLM_TEXT_N_LAYERS; i++) {
        int n = vlm_foreach_text_block(i, text_count_cb, &count);
        if (n < 0) { FAIL("text iterator failed"); return; }
        total += n;
    }
    /* Each block has 10 sub-tensors (ln w/b, attn qkv w/b, attn proj w/b,
       mlp fc1 w/b, mlp fc2 w/b) */
    int expected = VLM_TEXT_N_LAYERS * 10;
    if (total != expected) {
        printf("FAIL: got %d expected %d\n", total, expected);
        return;
    }
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 9: SID config verification
 * ═══════════════════════════════════════════════════════════════ */
static void test_sid_config(void) {
    TEST("SID config");

    TTS_SIDConfig tts = tts_sid_config();
    if (tts.n_faces != 4) {
        printf("FAIL: TTS n_faces=%d expected 4\n", tts.n_faces);
        return;
    }

    VLM_SIDConfig vlm = vlm_sid_config();
    if (vlm.n_faces != 8) {
        printf("FAIL: VLM n_faces=%d expected 8\n", vlm.n_faces);
        return;
    }

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 10: Cross-pipe isolation
 * ═══════════════════════════════════════════════════════════════ */
static void test_cross_pipe_isolation(void) {
    TEST("Cross-pipe isolation");

    PipeContext *tts = pipe_open_tts("iso-tts", NULL);
    PipeContext *vlm = pipe_open_vlm("iso-vlm", NULL);
    if (!tts || !vlm) { FAIL("open failed"); pipe_close(tts); pipe_close(vlm); return; }

    uint8_t data[32];
    fill_pattern(data, 32, 0xAABB);

    /* Write TTS tensor to TTS pipe */
    if (pipe_write_tts_tensor(tts, "text_encoder.embedding.weight", data, 32) != PIPE_OK)
        { FAIL("tts write failed"); pipe_close(tts); pipe_close(vlm); return; }

    /* Write VLM tensor to VLM pipe */
    if (pipe_write_vlm_tensor(vlm, "model.text.wte", data, 32) != PIPE_OK)
        { FAIL("vlm write failed"); pipe_close(tts); pipe_close(vlm); return; }

    /* Verify TTS tensor NOT in VLM pipe */
    if (pipe_find(vlm, "text_encoder.embedding.weight"))
        { FAIL("VLM pipe leaked TTS tensor"); pipe_close(tts); pipe_close(vlm); return; }

    /* Verify VLM tensor NOT in TTS pipe */
    if (pipe_find(tts, "model.text.wte"))
        { FAIL("TTS pipe leaked VLM tensor"); pipe_close(tts); pipe_close(vlm); return; }

    /* Verify each has its own tensor */
    if (!pipe_find(tts, "text_encoder.embedding.weight"))
        { FAIL("TTS missing its tensor"); pipe_close(tts); pipe_close(vlm); return; }
    if (!pipe_find(vlm, "model.text.wte"))
        { FAIL("VLM missing its tensor"); pipe_close(tts); pipe_close(vlm); return; }

    pipe_close(tts);
    pipe_close(vlm);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 11: Checkpoint/restore (Void Space)
 * ═══════════════════════════════════════════════════════════════ */
static void test_checkpoint_restore(void) {
    TEST("Checkpoint/restore");

    const char *snap_path = "test_tts_vlm_snap.bin";

    /* Phase 1: write data and checkpoint */
    PipeContext *ctx = pipe_open_tts("snap-test", NULL);
    if (!ctx) { FAIL("open failed"); return; }

    uint8_t data[128];
    fill_pattern(data, 128, 0xCAFE);
    if (pipe_write_tts_tensor(ctx, "predictor.duration_proj.weight", data, 128) != PIPE_OK)
        { FAIL("write failed"); pipe_close(ctx); return; }

    if (pipe_checkpoint(ctx, snap_path) != PIPE_OK)
        { FAIL("checkpoint failed"); pipe_close(ctx); return; }

    pipe_close(ctx);

    /* Phase 2: restore in new context */
    ctx = pipe_open_tts("snap-test", NULL);
    if (!ctx) { FAIL("reopen failed"); remove(snap_path); return; }

    if (pipe_restore(ctx, snap_path) != PIPE_OK)
        { FAIL("restore failed"); pipe_close(ctx); remove(snap_path); return; }

    /* Verify data survived */
    uint32_t got = 0;
    const void *ptr = pipe_read_tts_tensor(ctx, "predictor.duration_proj.weight", &got);
    if (!ptr) { FAIL("read after restore failed"); pipe_close(ctx); remove(snap_path); return; }
    if (got != 128) { FAIL("size after restore mismatch"); pipe_close(ctx); remove(snap_path); return; }
    if (!verify_pattern((const uint8_t *)ptr, 128, 0xCAFE)) {
        FAIL("data after restore corrupt"); pipe_close(ctx); remove(snap_path); return;
    }

    pipe_close(ctx);
    remove(snap_path);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 12: TTS group loader
 * ═══════════════════════════════════════════════════════════════ */
static int tts_group_cb(const char *group, void *user) {
    (*(int *)user)++;
    return 0;
}

static void test_tts_group_loader(void) {
    TEST("TTS group loader");

    PipeContext *tts = pipe_open_tts("group-test", NULL);
    if (!tts) { FAIL("open failed"); return; }

    int count = 0;
    int total = tts_load_groups(tts, tts_group_cb, &count);
    if (total != 5) {
        printf("FAIL: loaded %d groups, expected 5\n", total);
        pipe_close(tts); return;
    }

    pipe_close(tts);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 13: Large tensor stress test (>1MB)
 * ═══════════════════════════════════════════════════════════════ */
static void test_large_tensor(void) {
    TEST("Large tensor stress");

    PipeContext *vlm = pipe_open_vlm("large-test", NULL);
    if (!vlm) { FAIL("open failed"); return; }

    /* 2MB random data */
    uint32_t nbytes = 2 * 1024 * 1024;
    uint8_t *data = (uint8_t *)malloc(nbytes);
    fill_pattern(data, nbytes, 0xDEAD);

    int rc = pipe_write_vlm_tensor(vlm, "model.vision.blocks.0.mlp.fc1.weight",
                                    data, nbytes);
    if (rc != PIPE_OK) { FAIL("write failed"); free(data); pipe_close(vlm); return; }

    uint32_t got = 0;
    const void *ptr = pipe_read_vlm_tensor(vlm, "model.vision.blocks.0.mlp.fc1.weight", &got);
    if (!ptr) { FAIL("read failed"); free(data); pipe_close(vlm); return; }
    if (got != nbytes) { FAIL("size mismatch"); free(data); pipe_close(vlm); return; }
    if (!verify_pattern((const uint8_t *)ptr, nbytes, 0xDEAD)) {
        FAIL("data corrupt"); free(data); pipe_close(vlm); return;
    }

    free(data);
    pipe_close(vlm);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 14: Multi-tensor sequential write→read→verify
 * ═══════════════════════════════════════════════════════════════ */
static void test_multi_tensor(void) {
    TEST("Multi-tensor sequence");

    PipeContext *ctx = pipe_open_tts("multi-test", NULL);
    if (!ctx) { FAIL("open failed"); return; }

    /* Write 10 TTS tensors with different patterns */
    const char *names[] = {
        "text_encoder.embedding.weight",
        "text_encoder.conv1.weight",
        "text_encoder.conv1.bias",
        "text_encoder.conv2.weight",
        "text_encoder.conv2.bias",
        "text_encoder.lstm.weight_ih_l0",
        "text_encoder.lstm.weight_hh_l0",
        "text_encoder.lstm.bias_ih_l0",
        "text_encoder.lstm.bias_hh_l0",
        "text_encoder.linear.weight"
    };
    int n = sizeof(names) / sizeof(names[0]);
    uint32_t seeds[] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    uint32_t sizes[] = {64, 128, 256, 512, 1024, 2048, 4096, 8192, 256, 512};

    for (int i = 0; i < n; i++) {
        uint8_t *buf = (uint8_t *)malloc(sizes[i]);
        fill_pattern(buf, sizes[i], seeds[i]);
        int rc = pipe_write_tts_tensor(ctx, names[i], buf, sizes[i]);
        free(buf);
        if (rc != PIPE_OK) {
            printf("FAIL: write %d failed\n", i);
            pipe_close(ctx); return;
        }
    }

    /* Verify count matches */
    if (pipe_count(ctx) != (uint32_t)n) {
        printf("FAIL: count=%u expected %d\n", pipe_count(ctx), n);
        pipe_close(ctx); return;
    }

    /* Read back and verify each */
    for (int i = 0; i < n; i++) {
        uint32_t got = 0;
        const void *ptr = pipe_read_tts_tensor(ctx, names[i], &got);
        if (!ptr) { printf("FAIL: read %d NULL\n", i); pipe_close(ctx); return; }
        if (got != sizes[i]) {
            printf("FAIL: size %d: got %u expected %u\n", i, got, sizes[i]);
            pipe_close(ctx); return;
        }
        if (!verify_pattern((const uint8_t *)ptr, sizes[i], seeds[i])) {
            printf("FAIL: corruption at %d\n", i);
            pipe_close(ctx); return;
        }
    }

    pipe_close(ctx);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 15: Unified service interface — open/close lifecycle
 * ═══════════════════════════════════════════════════════════════ */
static void test_svc_lifecycle(void) {
    TEST("SVC lifecycle");

    PipeContext *svc = pipe_service_open(PIPE_SVC_TTS, "svc-tts", NULL);
    if (!svc) { FAIL("open TTS failed"); return; }
    pipe_service_close(svc);

    svc = pipe_service_open(PIPE_SVC_VLM, "svc-vlm", NULL);
    if (!svc) { FAIL("open VLM failed"); return; }
    pipe_service_close(svc);

    svc = pipe_service_open(PIPE_SVC_LLM, "svc-llm", NULL);
    if (!svc) { FAIL("open LLM failed"); return; }
    pipe_service_close(svc);

    svc = pipe_service_open(PIPE_SVC_AUTO, "svc-auto", NULL);
    if (!svc) { FAIL("open AUTO failed"); return; }
    pipe_service_close(svc);

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 16: Unified service — classify tensor names
 * ═══════════════════════════════════════════════════════════════ */
static void test_svc_classify(void) {
    TEST("SVC classify");

    if (pipe_service_classify("text_encoder.embedding.weight") != PIPE_SVC_TTS)
        { FAIL("TTS not detected"); return; }
    if (pipe_service_classify("bert.encoder.layer.0.weight") != PIPE_SVC_TTS)
        { FAIL("BERT not detected"); return; }
    if (pipe_service_classify("model.vision.blocks.0.attn.qkv.weight") != PIPE_SVC_VLM)
        { FAIL("VLM vision not detected"); return; }
    if (pipe_service_classify("model.text.wte") != PIPE_SVC_VLM)
        { FAIL("VLM text not detected"); return; }
    if (pipe_service_classify("model.proj_mlp.fc1.weight") != PIPE_SVC_VLM)
        { FAIL("VLM proj not detected"); return; }
    if (pipe_service_classify("token_embd.weight") != PIPE_SVC_LLM)
        { FAIL("LLM not detected"); return; }
    if (pipe_service_classify(NULL) != PIPE_SVC_LLM)
        { FAIL("NULL not LLM"); return; }

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 17: Unified service — write/read roundtrip
 * ═══════════════════════════════════════════════════════════════ */
static void test_svc_write_read(void) {
    TEST("SVC write/read");

    PipeContext *svc = pipe_service_open(PIPE_SVC_TTS, "svc-rw", NULL);
    if (!svc) { FAIL("open failed"); return; }

    uint8_t data[256];
    fill_pattern(data, 256, 0xFEED);
    if (pipe_service_write(svc, "text_encoder.embedding.weight", data, 256) != PIPE_OK)
        { FAIL("write failed"); pipe_service_close(svc); return; }

    uint32_t got = 0;
    const void *ptr = pipe_service_read(svc, "text_encoder.embedding.weight", &got);
    if (!ptr) { FAIL("read NULL"); pipe_service_close(svc); return; }
    if (got != 256) { FAIL("size mismatch"); pipe_service_close(svc); return; }
    if (!verify_pattern((const uint8_t *)ptr, 256, 0xFEED))
        { FAIL("corrupt"); pipe_service_close(svc); return; }

    /* Convenience helpers */
    if (!pipe_service_has(svc, "text_encoder.embedding.weight"))
        { FAIL("has failed"); pipe_service_close(svc); return; }
    if (pipe_service_count(svc) != 1)
        { FAIL("count wrong"); pipe_service_close(svc); return; }

    pipe_service_close(svc);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 18: Unified service — write file / export file
 * ═══════════════════════════════════════════════════════════════ */
static void test_svc_file_io(void) {
    TEST("SVC file I/O");

    PipeContext *svc = pipe_service_open(PIPE_SVC_TTS, "svc-file", NULL);
    if (!svc) { FAIL("open failed"); return; }

    /* Write test data as file */
    const char *tmp_in  = "test_svc_input.bin";
    const char *tmp_out = "test_svc_output.bin";

    /* Create input file */
    uint8_t data[64];
    fill_pattern(data, 64, 0xABCD);
    FILE *f = fopen(tmp_in, "wb");
    fwrite(data, 1, 64, f);
    fclose(f);

    /* pipe_service_write_file */
    int n = pipe_service_write_file(svc, "predictor.duration_proj.weight", tmp_in);
    if (n < 0) { FAIL("write_file failed"); remove(tmp_in); pipe_service_close(svc); return; }

    /* pipe_service_export_file */
    n = pipe_service_export_file(svc, "predictor.duration_proj.weight", tmp_out);
    if (n < 0) { FAIL("export_file failed"); remove(tmp_in); pipe_service_close(svc); return; }

    /* Verify exported file matches */
    f = fopen(tmp_out, "rb");
    uint8_t verify[64];
    size_t r = fread(verify, 1, 64, f);
    fclose(f);

    if (r != 64 || memcmp(data, verify, 64) != 0)
        { FAIL("export corrupt"); }
    else
        PASS();

    remove(tmp_in);
    remove(tmp_out);
    pipe_service_close(svc);
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 19: Unified service — checkpoint/restore via service API
 * ═══════════════════════════════════════════════════════════════ */
static void test_svc_checkpoint(void) {
    TEST("SVC checkpoint");

    const char *snap = "test_svc_snap.bin";

    PipeContext *svc = pipe_service_open(PIPE_SVC_TTS, "svc-cp", NULL);
    if (!svc) { FAIL("open failed"); return; }

    uint8_t data[128];
    fill_pattern(data, 128, 0x789A);
    pipe_service_write(svc, "decoder.istft.res_weight", data, 128);

    if (pipe_service_checkpoint(svc, snap) != PIPE_OK)
        { FAIL("checkpoint failed"); pipe_service_close(svc); return; }
    pipe_service_close(svc);

    /* Restore */
    svc = pipe_service_open(PIPE_SVC_TTS, "svc-cp", NULL);
    if (!svc) { FAIL("reopen failed"); remove(snap); return; }

    if (pipe_service_restore(svc, snap) != PIPE_OK)
        { FAIL("restore failed"); pipe_service_close(svc); remove(snap); return; }

    uint32_t got = 0;
    const void *ptr = pipe_service_read(svc, "decoder.istft.res_weight", &got);
    if (!ptr || got != 128 || !verify_pattern((const uint8_t *)ptr, 128, 0x789A))
        { FAIL("data verify failed"); }
    else
        PASS();

    pipe_service_close(svc);
    remove(snap);
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 20: Unified service — AUTO mode auto-detect
 * ═══════════════════════════════════════════════════════════════ */
static void test_svc_auto_detect(void) {
    TEST("SVC auto-detect");

    PipeContext *svc = pipe_service_open(PIPE_SVC_AUTO, "auto", NULL);
    if (!svc) { FAIL("open failed"); return; }

    /* Write VLM tensor → should classify as VLM */
    uint8_t data[64];
    fill_pattern(data, 64, 0xBEEF);
    if (pipe_service_write(svc, "model.vision.blocks.5.attn.qkv.weight", data, 64) != PIPE_OK)
        { FAIL("VLM write failed"); pipe_service_close(svc); return; }
    if (pipe_service_classify("model.vision.blocks.5.attn.qkv.weight") != PIPE_SVC_VLM)
        { FAIL("auto-detect missed VLM"); pipe_service_close(svc); return; }

    /* Write TTS tensor → should classify as TTS */
    fill_pattern(data, 64, 0xFACE);
    if (pipe_service_write(svc, "bert.encoder.layer.2.attention.self.query.weight", data, 64) != PIPE_OK)
        { FAIL("TTS write failed"); pipe_service_close(svc); return; }
    if (pipe_service_classify("bert.encoder.layer.2.attention.self.query.weight") != PIPE_SVC_TTS)
        { FAIL("auto-detect missed TTS"); pipe_service_close(svc); return; }

    /* Both should be findable */
    if (!pipe_service_has(svc, "model.vision.blocks.5.attn.qkv.weight"))
        { FAIL("VLM tensor gone after TTS write"); pipe_service_close(svc); return; }
    if (!pipe_service_has(svc, "bert.encoder.layer.2.attention.self.query.weight"))
        { FAIL("TTS tensor not found"); pipe_service_close(svc); return; }

    PASS();
    pipe_service_close(svc);
}

/* ═══════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════ */
int main(void) {
    printf("═══ TTS/VLM Pipe Integration Test Suite ═══\n\n");

    /* Phase 1: Lifecycle */
    printf("── Phase 1: Lifecycle ──\n");
    test_tts_lifecycle();
    test_vlm_lifecycle();
    printf("\n");

    /* Phase 2: Write/Read */
    printf("── Phase 2: Write/Read ──\n");
    test_tts_write_read();
    test_vlm_write_read();
    printf("\n");

    /* Phase 3: Classification */
    printf("── Phase 3: Classification ──\n");
    test_tts_classify();
    test_vlm_classify();
    printf("\n");

    /* Phase 4: Iteration */
    printf("── Phase 4: Iteration ──\n");
    test_vlm_vision_iterator();
    test_vlm_text_iterator();
    printf("\n");

    /* Phase 5: Configuration */
    printf("── Phase 5: Configuration ──\n");
    test_sid_config();
    printf("\n");

    /* Phase 6: Isolation */
    printf("── Phase 6: Isolation ──\n");
    test_cross_pipe_isolation();
    printf("\n");

    /* Phase 7: Persistence (Void Space) */
    printf("── Phase 7: Persistence ──\n");
    test_checkpoint_restore();
    printf("\n");

    /* Phase 8: Bulk Operations */
    printf("── Phase 8: Bulk Operations ──\n");
    test_tts_group_loader();
    test_large_tensor();
    test_multi_tensor();
    printf("\n");

    /* Phase 9: Unified Service Interface */
    printf("── Phase 9: Unified Service Interface ──\n");
    test_svc_lifecycle();
    test_svc_classify();
    test_svc_write_read();
    test_svc_file_io();
    test_svc_checkpoint();
    test_svc_auto_detect();
    printf("\n");

    printf("═══ Results: %d PASS, %d FAIL ═══\n", n_pass, n_fail);
    return n_fail > 0 ? 1 : 0;
}
