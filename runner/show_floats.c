#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: show_floats <file> <nfloats>\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "open failed: %s\n", argv[1]); return 1; }
    int n = atoi(argv[2]);
    uint8_t buf[512];
    int cols = 4;
    int rows = (n + cols - 1) / cols;
    for (int r = 0; r < rows; r++) {
        int off = r * cols * 4;
        fseek(f, off, SEEK_SET);
        int got = fread(buf, 1, cols * 4, f);
        if (got < 4) break;
        printf("  ");
        for (int c = 0; c < cols && r*cols+c < n; c++) {
            float v;
            memcpy(&v, buf + c*4, 4);
            printf("%12.8f ", v);
        }
        printf("\n");
    }
    fclose(f);
    return 0;
}
