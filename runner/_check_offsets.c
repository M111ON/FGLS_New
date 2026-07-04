#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <windows.h>

/* Try to match the DLL's struct layout */
/* From ggml.h (b9733): try different struct layouts and compare sizeof */

/* Attempt 1: GGML_MAX_SRC=10, no view_src/view_offs */
typedef struct {
    void *dummy[8];
    int ne[4];
    size_t nb[4];
    enum { GGML_OP_NONE } op;
    enum { GGML_TYPE_F32 } type;
    int32_t op_params[6];  /* GGML_MAX_OP_PARAMS=6? or 16? */
    int n_dims;
    int is_view;
    int64_t flags;
    void *grad;
    void *src[10];
    void *data;
    char name[64];
    void *extra;
    uint64_t padding[4];
} TensorLayout;

int main(void) {
    printf("sizeof(void*) = %d\n", (int)sizeof(void*));
    printf("sizeof(int) = %d\n", (int)sizeof(int));
    printf("sizeof(size_t) = %d\n", (int)sizeof(size_t));
    printf("sizeof(int64_t) = %d\n\n", (int)sizeof(int64_t));

    TensorLayout tl;
    printf("Layout attempt (GGML_MAX_SRC=10, no view):\n");
    printf("  offsetof(data) = %d\n", (int)offsetof(TensorLayout, data));
    printf("  sizeof(TensorLayout) = %d\n", (int)sizeof(TensorLayout));

    /* Dump hex of first 512 bytes of the struct to see where data might be */
    memset(&tl, 0xAA, sizeof(tl));
    tl.data = (void*)0xDEADBEEFCAFE;
    strcpy(tl.name, "test_tensor_name_12345");

    unsigned char *b = (unsigned char*)&tl;
    for (int i = 0; i < sizeof(TensorLayout); i++) {
        if (i % 16 == 0) printf("\n%03x: ", i);
        printf("%02x ", b[i]);
    }
    printf("\n");
    return 0;
}
