#include <stdio.h>
#include <stddef.h>
#include "ggml.h"

int main(void) {
    printf("GGML_TENSOR_SIZE = %zu\n", sizeof(struct ggml_tensor));
    struct ggml_tensor t;
#define OFF(f) printf("  offset %s = %zu\n", #f, offsetof(struct ggml_tensor, f))
    OFF(type);
    OFF(buffer);
    OFF(ne);
    OFF(nb);
    OFF(op);
    OFF(op_params);
    OFF(flags);
    OFF(src);
    OFF(view_src);
    OFF(view_offs);
    OFF(data);
    OFF(name);
    OFF(extra);
    printf("  end ~ %zu\n", sizeof(t));
    return 0;
}
