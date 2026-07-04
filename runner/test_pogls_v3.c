/*
 * test_pogls_v3.c — 7 tests for POGLS v3 geopixel header
 * Build: gcc -O2 -std=c11 -I. -o test_pogls_v3.exe test_pogls_v3.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "pogls_v3_geopixel.h"
#include "addr_space.h"

static int T = 0, P = 0;
#define TEST(n) do { T++; printf("  [%d] %-50s ", T, n); fflush(stdout); } while(0)
#define PASS() do { P++; printf("PASS\n"); } while(0)
#define FAIL(m) do { printf("FAIL: %s\n", m); } while(0)

static PoglsV3Entry *build(const char **names, int n, uint64_t *off) {
    PoglsV3Entry *e = calloc(n, sizeof(PoglsV3Entry));
    for (int i = 0; i < n; i++) {
        e[i].addr = addr_from_tensor_name(names[i], 0);
        e[i].gguf_offset = off ? off[i] : (uint64_t)i * 0x1000;
        strncpy(e[i].name, names[i], sizeof(e[i].name)-1);
    }
    qsort(e, n, sizeof(PoglsV3Entry), pogls_v3_cmp_name);
    return e;
}

static void t1(void) {
    TEST("header init");
    PoglsV3Header h; pogls_v3_header_init(&h);
    if (h.magic != POGLS_V3_MAGIC) { FAIL("magic"); return; }
    if (h.version != 3) { FAIL("ver"); return; }
    PASS();
}

static void t2(void) {
    TEST("binary search seek");
    const char *n[] = {"a.weight","b.weight","c.weight"};
    uint64_t o[] = {100,200,300};
    PoglsV3Entry *e = build(n, 3, o);
    const PoglsV3Entry *f = pogls_v3_seek(e, 3, "b.weight");
    if (!f || f->gguf_offset != 200) { FAIL("offset"); free(e); return; }
    free(e); PASS();
}

static void t3(void) {
    TEST("missing returns NULL");
    const char *n[] = {"x.weight"};
    PoglsV3Entry *e = build(n, 1, NULL);
    const PoglsV3Entry *f = pogls_v3_seek(e, 1, "y.weight");
    free(e);
    if (f) { FAIL("not null"); return; }
    PASS();
}

static void t4(void) {
    TEST("multi-tensor roundtrip");
    const char *n[] = {"token_embd.weight","blk.0.ffn_up.weight","blk.5.attn_q.weight","output_norm.weight"};
    uint64_t o[] = {0x1000,0x5000,0x9000,0xD000};
    PoglsV3Entry *e = build(n, 4, o);
    for (int i = 0; i < 4; i++) {
        const PoglsV3Entry *f = pogls_v3_seek(e, 4, n[i]);
        if (!f || f->gguf_offset != o[i]) { FAIL(n[i]); free(e); return; }
    }
    free(e); PASS();
}

static void t5(void) {
    TEST("addr_space deterministic");
    const char *n[] = {"token_embd.weight","blk.0.ffn_up.weight","output_norm.weight"};
    PoglsV3Entry *e = build(n, 3, NULL);
    for (int i = 0; i < 3; i++) {
        const PoglsV3Entry *f = pogls_v3_seek(e, 3, n[i]);
        if (!f || f->addr != addr_from_tensor_name(n[i], 0)) { FAIL(n[i]); free(e); return; }
    }
    free(e); PASS();
}

static void t6(void) {
    TEST("file roundtrip");
    const char *tmp = "test_v3_rt.pogls";
    const char *n[] = {"a.weight","b.weight"};
    uint64_t o[] = {0x1000,0x3000};
    PoglsV3Entry *entries = build(n, 2, o);
    PoglsV3Header h; pogls_v3_header_init(&h);
    h.n_tensors = 2; h.flags = POGLS_V3_FLAG_HAS_GGUF;
    h.entries_off = sizeof(PoglsV3Header);
    h.gguf_path_off = sizeof(PoglsV3Header) + 2*POGLS_V3_ENTRY_SZ;
    h.gguf_path_sz = 10;
    FILE *f = fopen(tmp, "wb");
    fwrite(&h, sizeof(h), 1, f);
    fwrite(entries, POGLS_V3_ENTRY_SZ, 2, f);
    fwrite("test.gguf", 10, 1, f);
    fclose(f);
    FILE *fi = fopen(tmp, "rb");
    PoglsV3Header h2; fread(&h2, sizeof(h2), 1, fi);
    PoglsV3Entry *e2 = malloc(2*POGLS_V3_ENTRY_SZ);
    fseek(fi, h2.entries_off, SEEK_SET);
    fread(e2, POGLS_V3_ENTRY_SZ, 2, fi);
    fclose(fi);
    int ok = 1;
    for (int i = 0; i < 2; i++) {
        const PoglsV3Entry *f2 = pogls_v3_seek(e2, 2, n[i]);
        if (!f2 || f2->gguf_offset != o[i]) ok = 0;
    }
    free(entries); free(e2); remove(tmp);
    if (!ok) { FAIL("lookup"); return; } PASS();
}

static void t7(void) {
    TEST("benchmark bs vs linear");
    int N = 256;
    PoglsV3Entry *e = calloc(N, sizeof(PoglsV3Entry));
    char (*names)[48] = calloc(N, 48);
    for (int i = 0; i < N; i++) {
        snprintf(names[i], 48, "blk.%d.ffn_up.weight", i%16);
        e[i].addr = addr_from_tensor_name(names[i], 0);
        e[i].gguf_offset = (uint64_t)i * 0x1000;
        strncpy(e[i].name, names[i], 47);
    }
    qsort(e, N, sizeof(PoglsV3Entry), pogls_v3_cmp_name);
    int I = 1000000;
    clock_t s; volatile uint64_t sum;
    s = clock(); sum = 0;
    for (int i = 0; i < I; i++) { const PoglsV3Entry *f = pogls_v3_seek(e, N, names[i%N]); if (f) sum += f->gguf_offset; }
    double bs = (double)(clock()-s)/CLOCKS_PER_SEC*1e6;
    s = clock(); sum = 0;
    for (int i = 0; i < I; i++) { for (int j = 0; j < N; j++) { if (strcmp(e[j].name, names[i%N])==0) { sum += e[j].gguf_offset; break; } } }
    double ls = (double)(clock()-s)/CLOCKS_PER_SEC*1e6;
    printf("\n    BS: %.0f ns/lookup  Linear: %.0f ns/lookup  Speedup: %.0fx\n",
           bs*1000/I, ls*1000/I, ls/bs);
    free(e); free(names); PASS();
}

int main(void) {
    printf("═══ POGLS v3 Geopixel Store Tests ═══\n\n");
    t1(); t2(); t3(); t4(); t5(); t6(); t7();
    printf("\n═══ Results: %d/%d PASS ═══\n", P, T);
    return P == T ? 0 : 1;
}
