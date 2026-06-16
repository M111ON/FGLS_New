#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: compare_logits <baseline.bin> <test.bin>\n"); return 1; }
    FILE *fb = fopen(argv[1], "rb");
    FILE *ft = fopen(argv[2], "rb");
    if (!fb || !ft) { fprintf(stderr, "ERROR: open\n"); return 1; }
    int nb, nt;
    fread(&nb, sizeof(nb), 1, fb);
    fread(&nt, sizeof(nt), 1, ft);
    if (nb != nt) { fprintf(stderr, "ERROR: size mismatch %d vs %d\n", nb, nt); return 1; }
    float *base = malloc(nb * sizeof(float));
    float *test = malloc(nt * sizeof(float));
    fread(base, sizeof(float), nb, fb);
    fread(test, sizeof(float), nt, ft);
    fclose(fb); fclose(ft);
    int nd = 0; double md = 0.0, mse = 0.0, sum_b = 0, sum_t = 0;
    for (int i = 0; i < nb; i++) {
        double d = (double)test[i] - (double)base[i];
        if (fabs(d) > 1e-10) { nd++; if (fabs(d) > md) md = fabs(d); mse += d * d; }
        sum_b += base[i]; sum_t += test[i];
    }
    mse = sqrt(mse / nb);
    fprintf(stderr, "\n===== LOGIT COMPARISON =====\n");
    fprintf(stderr, "file1: %s (baseline)\n", argv[1]);
    fprintf(stderr, "file2: %s (test)\n", argv[2]);
    fprintf(stderr, "vocab: %d\n", nb);
    fprintf(stderr, "diff:  %d / %d (%.2f%%)\n", nd, nb, 100.0 * nd / nb);
    fprintf(stderr, "max_diff: %.8f\n", md);
    fprintf(stderr, "RMSE:  %.8f\n", mse);
    fprintf(stderr, "mean baseline: %.4f  test: %.4f\n", sum_b/nb, sum_t/nb);
    fprintf(stderr, "============================\n");
    free(base); free(test);
    return 0;
}
