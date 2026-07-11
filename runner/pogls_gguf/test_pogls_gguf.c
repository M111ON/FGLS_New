#include "pogls_gguf.h"
#include <string.h>
#include <assert.h>

static int pass = 0, fail = 0;
#define TEST(name) do { printf("  [%s]\n", name); } while(0)
#define CHECK(cond, msg) do { if (cond) { pass++; printf("    PASS: %s\n", msg); } else { fail++; printf("    FAIL: %s\n", msg); } } while(0)

int main(void) {
    printf("═══ POGLS GGUF Library Test ═══\n\n");

    TEST("Type sizes");
    CHECK(pogls_gguf_type_size(0) == 4, "F32 = 4B");
    CHECK(pogls_gguf_type_size(1) == 2, "F16 = 2B");
    CHECK(pogls_gguf_type_size(12) == 144, "Q4_K = 144B");
    CHECK(pogls_gguf_block_size(12) == 256, "Q4_K block = 256");

    TEST("Type queries");
    CHECK(pogls_gguf_type_is_kquant(12), "Q4_K is k-quant");
    CHECK(!pogls_gguf_type_is_kquant(0), "F32 not k-quant");

    TEST("Index open (file must exist)");
    PoglsGgufReader r;
    int ok = pogls_gguf_open("pogls_core/test_pogls_core.c", &r);
    CHECK(ok != 0, "non-gguf file returns error");

    TEST("Constants");
    CHECK(POGLS_GGUF_MAGIC == 0x46554747u, "GGUF_MAGIC correct");
    CHECK(POGLS_GGUF_ALIGN == 32, "GGUF_ALIGN = 32");

    printf("\n═══ Results: %d pass, %d fail ═══\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
