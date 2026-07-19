/* atomic_reshape_standalone.c — standalone test runner */
#include <stdio.h>
extern int atomic_reshape_demo(const char *out_path);
int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : NULL;
    return atomic_reshape_demo(out);
}
