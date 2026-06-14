#ifdef SID_INTEGRATION_TEST_MAIN
#include <stdio.h>
#include <string.h>
#include "../gguf_index.h"
#include "../sid_loader.h"
#include "../sid_cache.h"

static int failed = 0, passed = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { fprintf(stderr, "FAIL: %s\n", name); failed++; } \
    else { passed++; } \
} while(0)

int main(void) {
    printf("Integration test: requires model.gguf\n");
    return 0;
}
#endif
