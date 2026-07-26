#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* SafeTensors Blueprint Integrity Test - Fixed JSON parser + BF16 support */

static void sort_floats(float *arr, int n) {
    for (int i = 0; i < n-1; i++)
        for (int j = i+1; j < n; j++)
            if (arr[i] > arr[j]) { float t = arr[i]; arr[i] = arr[j]; arr[j] = t; }
}

static int find_closest(float val, float *centroids, int n_c) {
    int best = 0;
    float best_d = fabs(val - centroids[0]);
    for (int i = 1; i < n_c; i++) {
        float d = fabs(val - centroids[i]);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

typedef struct {
    float center, radius, centroids[7];
    uint8_t deltas[4096];
    float delta_scale, max_delta, avg_delta;
    int n_weights;
} Blueprint;

Blueprint bp_compress(float *w, int n) {
    Blueprint b;
    memset(&b, 0, sizeof(b));
    b.n_weights = n;
    float sorted[4096];
    for (int i = 0; i < n; i++) sorted[i] = w[i];
    sort_floats(sorted, n);
    b.center = sorted[n/2];
    b.centroids[0] = b.center;
    for (int i = 1; i < 7; i++) {
        int idx = (i * n) / 7;
        if (idx >= n) idx = n - 1;
        b.centroids[i] = sorted[idx];
    }
    float sd = 0;
    for (int i = 1; i < 7; i++) sd += fabs(b.centroids[i] - b.center);
    b.radius = sd / 6.0f;
    b.max_delta = 0; b.avg_delta = 0;
    for (int i = 0; i < n; i++) {
        int c = find_closest(w[i], b.centroids, 7);
        float d = fabs(w[i] - b.centroids[c]);
        b.avg_delta += d;
        if (d > b.max_delta) b.max_delta = d;
    }
    b.avg_delta /= n;
    b.delta_scale = b.max_delta / 255.0f;
    if (b.delta_scale < 1e-10f) b.delta_scale = 1e-10f;
    for (int i = 0; i < n; i++) {
        int c = find_closest(w[i], b.centroids, 7);
        float d = fabs(w[i] - b.centroids[c]);
        float q = d / b.delta_scale;
        if (q > 255) q = 255;
        b.deltas[i] = (uint8_t)q;
    }
    return b;
}

typedef struct { float avg_pct, max_pct, psnr; int exact; } Result;

Result verify(float *orig, Blueprint *b) {
    Result r = {0,0,0,0};
    float mn = orig[0], mx = orig[0];
    for (int i = 1; i < b->n_weights; i++) {
        if (orig[i] < mn) mn = orig[i];
        if (orig[i] > mx) mx = orig[i];
    }
    float range = mx - mn;
    if (range < 1e-10f) range = 1e-10f;
    float sse = 0, sae = 0, mae = 0;
    for (int i = 0; i < b->n_weights; i++) {
        int c = find_closest(orig[i], b->centroids, 7);
        float sign = (orig[i] - b->centroids[c]) >= 0 ? 1 : -1;
        float rec = b->centroids[c] + sign * b->deltas[i] * b->delta_scale;
        float e = fabs(orig[i] - rec);
        sse += e*e; sae += e;
        if (e > mae) mae = e;
        if (e < 1e-10f) r.exact++;
    }
    r.avg_pct = 100.0f * sae / (b->n_weights * range);
    r.max_pct = 100.0f * mae / range;
    float mv = fabs(mx) > fabs(mn) ? fabs(mx) : fabs(mn);
    float mse = sse / b->n_weights;
    r.psnr = (mse > 1e-10f) ? 10.0f * log10f(mv*mv/mse) : 999.0f;
    return r;
}

/* BF16 → float32 */
static float bf16_to_f32(uint16_t h) {
    int sign = (h >> 15) & 1;
    int exp = (h >> 7) & 0xff;
    int mantissa = h & 0x7f;
    if (exp == 0) return (sign ? -1 : 1) * ldexp(mantissa, -132);
    if (exp == 255) return (sign ? -1 : 1) * INFINITY;
    return (sign ? -1 : 1) * ldexp(1.0 + mantissa / 128.0, exp - 127);
}

/* FP32 → float32 */
static float fp32_read(const uint8_t *d) { float v; memcpy(&v, d, 4); return v; }

/* FP16 → float32 */
static float fp16_to_f32(uint16_t h) {
    int sign = (h >> 15) & 1;
    int exp = (h >> 10) & 0x1f;
    int mantissa = h & 0x3ff;
    if (exp == 0) return (sign ? -1 : 1) * ldexp(mantissa, -24);
    if (exp == 31) return (sign ? -1 : 1) * INFINITY;
    return (sign ? -1 : 1) * ldexp(1.0 + mantissa / 1024.0, exp - 15);
}

/* Simple JSON: find "tensor_name": { ... } blocks */
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <model.safetensors>\n", argv[0]); return 1; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", argv[1]); return 1; }

    fprintf(stderr, "=== SafeTensors Blueprint Test ===\n");

    uint64_t hdr_len;
    fread(&hdr_len, 8, 1, f);
    fprintf(stderr, "Header: %lu bytes\n", (unsigned long)hdr_len);

    char *hdr = malloc(hdr_len + 1);
    fread(hdr, 1, hdr_len, f);
    hdr[hdr_len] = 0;
    long data_start = ftell(f);

    /* Find tensor entries: "name": {"dtype":N, "shape":[...], "data_offsets":[S,E]} */
    int analyzed = 0, total_tensors = 0, max_analyze = 20;
    char *p = hdr;

    while (*p && analyzed < max_analyze) {
        /* Find tensor name pattern: "...":  {"dtype": */
        char *name_quote = strchr(p, '"');
        if (!name_quote) break;
        char *name_end = strchr(name_quote + 1, '"');
        if (!name_end) break;

        /* Check if next non-space is :{ */
        char *colon = name_end + 1;
        while (*colon == ' ') colon++;
        if (*colon != ':') { p = name_end + 1; continue; }
        colon++;
        while (*colon == ' ') colon++;
        if (*colon != '{') { p = name_end + 1; continue; }

        /* Extract name */
        int name_len = name_end - name_quote - 1;
        char name[256];
        if (name_len > 0 && name_len < 255) {
            memcpy(name, name_quote + 1, name_len);
            name[name_len] = 0;
        } else { p = name_end + 1; continue; }

        /* Skip __metadata__ */
        if (strcmp(name, "__metadata__") == 0) { p = name_end + 1; continue; }

        /* Find "dtype" within next 2000 chars */
        char *scope_start = colon;
        char *scope_end = scope_start + 2000;
        if (scope_end > hdr + hdr_len) scope_end = hdr + hdr_len;

        char *dtype_p = NULL, *offsets_p = NULL, *shape_p = NULL;
        {
            char *scan = scope_start;
            while (scan < scope_end - 6) {
                if (!dtype_p && memcmp(scan, "\"dtype\"", 7) == 0) dtype_p = scan;
                if (!offsets_p && memcmp(scan, "\"data_offsets\"", 14) == 0) offsets_p = scan;
                if (!shape_p && memcmp(scan, "\"shape\"", 7) == 0) shape_p = scan;
                scan++;
            }
        }

        if (!dtype_p || !offsets_p) { p = name_end + 1; continue; }

        /* Parse dtype - can be string "BF16" or number */
        char *dv = dtype_p + 7;
        while (*dv == ' ' || *dv == ':') dv++;
        int dtype = -1;
        if (*dv == '"') {
            dv++;
            if (dv[0]=='B' && dv[1]=='F' && dv[2]=='1' && dv[3]=='6') dtype = 30;
            else if (dv[0]=='F' && dv[1]=='3' && dv[2]=='2') dtype = 1;
            else if (dv[0]=='F' && dv[1]=='1' && dv[2]=='6') dtype = 6;
            else if (dv[0]=='F' && dv[1]=='6' && dv[2]=='4') dtype = 14;
            else if (dv[0]=='I' && dv[1]=='6' && dv[2]=='4') dtype = 13;
            else if (dv[0]=='I' && dv[1]=='3' && dv[2]=='2') dtype = 11;
            else if (dv[0]=='I' && dv[1]=='1' && dv[2]=='6') dtype = 5;
            else if (dv[0]=='I' && dv[1]=='8' && dv[2]=='\0') dtype = 3;
            else if (dv[0]=='U' && dv[1]=='8' && dv[2]=='\0') dtype = 2;
            else if (dv[0]=='B' && dv[1]=='O' && dv[2]=='O' && dv[3]=='L') dtype = 0;
        } else {
            dtype = atoi(dv);
        }

        /* Parse data_offsets [S,E] */
        char *ov = offsets_p + 14;
        while (*ov == ' ' || *ov == ':') ov++;
        long off_s = -1, off_e = -1;
        if (*ov == '[') {
            ov++;
            off_s = strtol(ov, &ov, 10);
            while (*ov == ',' || *ov == ' ') ov++;
            off_e = strtol(ov, &ov, 10);
        }

        /* Parse shape */
        int n_elements = 1;
        if (shape_p && shape_p < offsets_p) {
            char *sv = shape_p + 7;
            while (*sv == ' ' || *sv == ':') sv++;
            if (*sv == '[') {
                sv++;
                while (*sv && *sv != ']') {
                    if (*sv >= '0' && *sv <= '9') {
                        n_elements *= strtol(sv, &sv, 10);
                    } else sv++;
                }
            }
        }

        if (off_s < 0 || off_e <= off_s) { p = name_end + 1; continue; }

        long tensor_bytes = off_e - off_s;

        fprintf(stderr, "\n[%d] %s dtype=%d shape=%d (%ld bytes)\n",
                total_tensors, name, dtype, n_elements, tensor_bytes);

        total_tensors++;

        /* Read tensor data */
        long saved = ftell(f);
        fseek(f, data_start + off_s, SEEK_SET);

        uint8_t *buf = malloc(tensor_bytes);
        if (buf) {
            fread(buf, 1, tensor_bytes, f);

            /* Convert to float32 based on dtype */
            float *fweights = NULL;
            int n_floats = 0;

            if (dtype == 1 && tensor_bytes >= n_elements * 4) {
                /* FP32 */
                n_floats = n_elements;
                fweights = malloc(n_floats * sizeof(float));
                for (int i = 0; i < n_floats; i++)
                    fweights[i] = fp32_read(buf + i * 4);
            } else if (dtype == 30 && tensor_bytes >= n_elements * 2) {
                /* BF16 */
                n_floats = n_elements;
                fweights = malloc(n_floats * sizeof(float));
                for (int i = 0; i < n_floats; i++) {
                    uint16_t h;
                    memcpy(&h, buf + i * 2, 2);
                    fweights[i] = bf16_to_f32(h);
                }
            } else if (dtype == 6 && tensor_bytes >= n_elements * 2) {
                /* FP16 */
                n_floats = n_elements;
                fweights = malloc(n_floats * sizeof(float));
                for (int i = 0; i < n_floats; i++) {
                    uint16_t h;
                    memcpy(&h, buf + i * 2, 2);
                    fweights[i] = fp16_to_f32(h);
                }
            }

            if (fweights && n_floats >= 4 && n_floats <= 4096) {
                /* Analyze a sample of blocks */
                int block_size = 32;
                int n_blocks = n_floats / block_size;
                if (n_blocks > 8) n_blocks = 8;

                float total_avg = 0, total_max = 0, total_psnr = 0;
                int total_exact = 0, total_w = 0, blocks_ok = 0;

                for (int b = 0; b < n_blocks; b++) {
                    float *bw = fweights + b * block_size;

                    /* Skip constant blocks */
                    float mn = bw[0], mx = bw[0];
                    for (int i = 1; i < block_size; i++) {
                        if (bw[i] < mn) mn = bw[i];
                        if (bw[i] > mx) mx = bw[i];
                    }
                    if (mx - mn < 1e-6f) continue;

                    Blueprint bp = bp_compress(bw, block_size);
                    Result r = verify(bw, &bp);

                    total_avg += r.avg_pct;
                    total_max += r.max_pct;
                    total_psnr += r.psnr;
                    total_exact += r.exact;
                    total_w += block_size;
                    blocks_ok++;
                }

                if (blocks_ok > 0) {
                    fprintf(stderr, "  Blocks: %d  AvgΔ: %.2f%%  MaxΔ: %.2f%%  PSNR: %.1f dB  Exact: %d/%d (%.0f%%)\n",
                            blocks_ok,
                            total_avg / blocks_ok,
                            total_max / blocks_ok,
                            total_psnr / blocks_ok,
                            total_exact, total_w,
                            100.0f * total_exact / total_w);

                    float orig_bytes = (float)tensor_bytes;
                    float bp_bytes = 7 * 4.0f + n_floats * 1.0f + 4.0f;
                    fprintf(stderr, "  Compression: %.0f KB → %.0f KB (%.1f%%)\n",
                            orig_bytes / 1024, bp_bytes / 1024, 100.0f * bp_bytes / orig_bytes);
                    analyzed++;
                } else {
                    fprintf(stderr, "  [SKIP] all constant blocks\n");
                }
            } else {
                const char *reason = "unknown dtype";
                if (dtype == 0) reason = "bool";
                else if (dtype == 2) reason = "u8";
                else if (dtype == 3) reason = "i8";
                else if (dtype == 4) reason = "u16";
                else if (dtype == 5) reason = "i16";
                else if (dtype == 10) reason = "u32";
                else if (dtype == 11) reason = "i32";
                else if (dtype == 12) reason = "u64";
                else if (dtype == 13) reason = "i64";
                else if (dtype == 14) reason = "f64";
                fprintf(stderr, "  [SKIP] dtype=%d (%s)\n", dtype, reason);
            }

            free(fweights);
            free(buf);
        }

        fseek(f, saved, SEEK_SET);
        p = name_end + 1;
    }

    fprintf(stderr, "\n=== Summary ===\n");
    fprintf(stderr, "Total tensors: %d\n", total_tensors);
    fprintf(stderr, "Analyzed: %d\n", analyzed);

    free(hdr);
    fclose(f);
    return 0;
}
