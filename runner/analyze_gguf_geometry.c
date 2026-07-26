#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* Minimal GGUF reader for weight geometry analysis */

#define GGUF_MAGIC     0x46554747  /* "GGUF" */
#define GGUF_VERSION_3 3

typedef struct {
    char name[128];
    int n_dims;
    uint64_t dims[4];
    uint32_t type;
    uint64_t offset;
    uint64_t size;
} TensorInfo;

/* Q8_0 block: 32 bytes data + 2 bytes scale (fp16) = 34 bytes per 32 weights */
static float q8_dequant_block(const uint8_t *data, const float *scales, int idx) {
    int block = idx / 32;
    int within = idx % 32;
    uint16_t sc16;
    memcpy(&sc16, data + block * 34 + 32, 2);
    /* fp16 to float (simple) */
    int sign = (sc16 >> 15) & 1;
    int exp = (sc16 >> 10) & 0x1f;
    int mantissa = sc16 & 0x3ff;
    float scale;
    if (exp == 0) {
        scale = (sign ? -1 : 1) * ldexp(mantissa, -24);
    } else if (exp == 31) {
        scale = (sign ? -1 : 1) * INFINITY;
    } else {
        scale = (sign ? -1 : 1) * ldexp(1.0 + mantissa / 1024.0, exp - 15);
    }
    int8_t q = (int8_t)data[block * 34 + within];
    return q * scale;
}

/* F32 dequant */
static float f32_read(const uint8_t *data, int idx) {
    float v;
    memcpy(&v, data + idx * 4, 4);
    return v;
}

/* F16 dequant */
static float f16_read(const uint8_t *data, int idx) {
    uint16_t h;
    memcpy(&h, data + idx * 2, 2);
    int sign = (h >> 15) & 1;
    int exp = (h >> 10) & 0x1f;
    int mantissa = h & 0x3ff;
    if (exp == 0) return (sign ? -1 : 1) * ldexp(mantissa, -24);
    if (exp == 31) return (sign ? -1 : 1) * INFINITY;
    return (sign ? -1 : 1) * ldexp(1.0 + mantissa / 1024.0, exp - 15);
}

/* Weight analysis functions */
static void sort_floats(float *arr, int n) {
    for (int i = 0; i < n-1; i++)
        for (int j = i+1; j < n; j++)
            if (arr[i] > arr[j]) { float t = arr[i]; arr[i] = arr[j]; arr[j] = t; }
}

static void analyze_tensor(const char *name, float *weights, int n) {
    if (n < 4) return;

    float sorted[4096];
    int sn = n < 4096 ? n : 4096;
    for (int i = 0; i < sn; i++) sorted[i] = weights[i];
    sort_floats(sorted, sn);

    float min_val = sorted[0], max_val = sorted[sn-1];
    float range = max_val - min_val;
    if (range < 1e-10f) range = 1e-10f;

    /* Statistics */
    float sum = 0, sum_sq = 0;
    for (int i = 0; i < sn; i++) {
        sum += sorted[i];
        sum_sq += sorted[i] * sorted[i];
    }
    float mean = sum / sn;
    float variance = sum_sq / sn - mean * mean;
    float stddev = sqrt(variance);

    /* Circle packing: 1 center + 6 outer */
    float center = sorted[sn / 2];  /* median */
    float outer[6];
    for (int i = 0; i < 6; i++) {
        int idx = (i + 1) * sn / 7;
        if (idx >= sn) idx = sn - 1;
        outer[i] = sorted[idx];
    }
    float radius = 0;
    for (int i = 0; i < 6; i++) radius += fabs(outer[i] - center);
    radius /= 6.0f;

    /* Delta analysis */
    float max_delta = 0, sum_delta = 0;
    for (int i = 0; i < sn; i++) {
        float min_dist = fabs(weights[i] - center);
        for (int c = 0; c < 6; c++) {
            float d = fabs(weights[i] - outer[c]);
            if (d < min_dist) min_dist = d;
        }
        sum_delta += min_dist;
        if (min_dist > max_delta) max_delta = min_dist;
    }
    float avg_delta = sum_delta / sn;

    /* Fibonacci clustering: how well do weights fit into F(n) buckets */
    int fibs[] = {2, 3, 5, 8, 13, 21};
    int nfibs = 6;

    fprintf(stderr, "\n  %s (n=%d, range=[%.4f, %.4f], stddev=%.4f)\n",
            name, n, min_val, max_val, stddev);
    fprintf(stderr, "    Circle: center=%.4f radius=%.4f maxΔ=%.4f%% avgΔ=%.4f%%\n",
            center, radius,
            100.0f * max_delta / range,
            100.0f * avg_delta / range);

    /* Autocorrelation in sorted order (local clustering) */
    float autocorr = 0;
    float var_diff = 0;
    for (int i = 0; i < sn - 1; i++) {
        float d = sorted[i+1] - sorted[i];
        var_diff += d * d;
        autocorr += sorted[i] * sorted[i+1];
    }
    autocorr /= (var_diff + 1e-10f);

    fprintf(stderr, "    Autocorr(sorted): %.2f  Variance(sorted_diff): %.6f\n",
            autocorr, var_diff / (sn - 1));

    fprintf(stderr, "    Fibonacci fit: ");
    for (int f = 0; f < nfibs; f++) {
        int nbuckets = fibs[f];
        float bucket_size = range / nbuckets;
        int hit = 0;
        for (int i = 0; i < sn; i++) {
            int b = (int)((sorted[i] - min_val) / bucket_size);
            if (b >= nbuckets) b = nbuckets - 1;
            hit++;
        }
        fprintf(stderr, "F(%d)=%d%% ", nbuckets, 100 * hit / sn);
    }
    fprintf(stderr, "\n");

    /* Compression potential */
    float blueprint = 7 * 4.0f;
    float original = n * 4.0f;
    float delta_quant = n * 1.0f;  /* uint8 deltas */
    float total = blueprint + delta_quant;
    fprintf(stderr, "    Compression: %.0f → %.0f bytes (%.1f%%) [blueprint+delta]\n",
            original, total, 100.0f * total / original);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <model.gguf> [max_tensors]\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    int max_tensors = argc > 2 ? atoi(argv[2]) : 20;

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return 1; }

    /* Read header */
    uint32_t magic, version;
    fread(&magic, 4, 1, f);
    if (magic != GGUF_MAGIC) {
        fprintf(stderr, "Bad magic: 0x%08x\n", magic);
        fclose(f);
        return 1;
    }
    fread(&version, 4, 1, f);
    fprintf(stderr, "GGUF version: %u\n", version);

    uint64_t n_tensors;
    fread(&n_tensors, 8, 1, f);
    fprintf(stderr, "Tensors: %lu\n", (unsigned long)n_tensors);

    /* Skip metadata (read key-value pairs to advance file pointer) */
    uint64_t n_kv;
    fread(&n_kv, 8, 1, f);

    for (uint64_t i = 0; i < n_kv; i++) {
        /* key: uint32 len + string */
        uint32_t klen;
        fread(&klen, 4, 1, f);
        fseek(f, klen, SEEK_CUR);

        /* value type */
        uint32_t vtype;
        fread(&vtype, 4, 1, f);

        /* skip value based on type */
        switch (vtype) {
            case 0: fseek(f, 1, SEEK_CUR); break;   /* uint8 */
            case 1: fseek(f, 1, SEEK_CUR); break;   /* int8 */
            case 2: fseek(f, 2, SEEK_CUR); break;   /* uint16 */
            case 3: fseek(f, 2, SEEK_CUR); break;   /* int16 */
            case 4: fseek(f, 4, SEEK_CUR); break;   /* uint32 */
            case 5: fseek(f, 4, SEEK_CUR); break;   /* int32 */
            case 6: fseek(f, 8, SEEK_CUR); break;   /* float32 */
            case 7: fseek(f, 8, SEEK_CUR); break;   /* boolean */
            case 8: { /* string */
                uint32_t slen;
                fread(&slen, 4, 1, f);
                fseek(f, slen, SEEK_CUR);
                break;
            }
            case 9: fseek(f, 8, SEEK_CUR); break;   /* array */
            case 10: fseek(f, 8, SEEK_CUR); break;  /* uint64 */
            case 11: fseek(f, 8, SEEK_CUR); break;  /* int64 */
            case 12: fseek(f, 8, SEEK_CUR); break;  /* float64 */
            default:
                fprintf(stderr, "Unknown metadata type %u at KV %lu, aborting\n", vtype, (unsigned long)i);
                fclose(f);
                return 1;
        }
    }

    /* Read tensor infos */
    long data_offset_pos = ftell(f);
    uint64_t data_offset;
    fread(&data_offset, 8, 1, f);

    fprintf(stderr, "Data offset: %lu\n", (unsigned long)data_offset);
    fprintf(stderr, "\nAnalyzing up to %d tensors...\n", max_tensors);
    fprintf(stderr, "================================================\n");

    /* Q8_0 type constant (typically 8 or 7 in GGUF v3) */
    int analyzed = 0;

    for (uint64_t i = 0; i < n_tensors && analyzed < max_tensors; i++) {
        /* name */
        uint32_t nlen;
        fread(&nlen, 4, 1, f);
        char name[256];
        if (nlen < 256) {
            fread(name, nlen, 1, f);
            name[nlen] = 0;
        } else {
            fread(name, 255, 1, f);
            name[255] = 0;
        }

        /* n_dims */
        uint32_t n_dims;
        fread(&n_dims, 4, 1, f);

        /* dims */
        uint64_t dims[4] = {1,1,1,1};
        for (int d = 0; d < (int)n_dims && d < 4; d++) {
            fread(&dims[d], 8, 1, f);
        }

        /* type */
        uint32_t type;
        fread(&type, 4, 1, f);

        /* offset */
        uint64_t offset;
        fread(&offset, 8, 1, f);

        /* Calculate total elements */
        uint64_t total_elements = 1;
        for (int d = 0; d < 4; d++) total_elements *= dims[d];

        /* Calculate weight size */
        uint64_t weight_bytes = 0;
        if (type == 8 || type == 7) {
            /* Q8_0: 32 weights per block, each block = 32 bytes + 2 bytes scale = 34 bytes */
            weight_bytes = (total_elements / 32) * 34;
            if (total_elements % 32) weight_bytes += (total_elements % 32) + 2;
        } else if (type == 1) {
            /* F32 */
            weight_bytes = total_elements * 4;
        } else if (type == 10) {
            /* F16 */
            weight_bytes = total_elements * 2;
        } else {
            /* Skip unknown types */
            fprintf(stderr, "  [SKIP] %s type=%u n_dims=%u\n", name, type, n_dims);
            continue;
        }

        if (total_elements < 4 || total_elements > 4096) {
            fprintf(stderr, "  [SKIP] %s %lux%lux%lu = %lu elements (too %s)\n",
                    name, dims[0], dims[1], dims[2], dims[3], total_elements < 4 ? "few" : "many");
            continue;
        }

        /* Read weight data */
        long saved_pos = ftell(f);
        long abs_offset = (long)data_offset + (long)offset;

        /* Align to 32 bytes */
        if (abs_offset % 32 != 0) {
            abs_offset = ((abs_offset + 31) / 32) * 32;
        }

        fseek(f, abs_offset, SEEK_SET);

        uint8_t *buf = malloc(weight_bytes > 0 ? weight_bytes : 1);
        if (!buf) { fprintf(stderr, "OOM\n"); break; }
        fread(buf, weight_bytes, 1, f);

        /* Dequantize to float */
        float *fweights = malloc(total_elements * sizeof(float));
        if (!fweights) { free(buf); continue; }

        if (type == 8 || type == 7) {
            /* Q8_0 */
            for (uint64_t e = 0; e < total_elements; e++) {
                fweights[e] = q8_dequant_block(buf, NULL, e);
            }
        } else if (type == 1) {
            /* F32 */
            for (uint64_t e = 0; e < total_elements; e++) {
                fweights[e] = f32_read(buf, e);
            }
        } else if (type == 10) {
            /* F16 */
            for (uint64_t e = 0; e < total_elements; e++) {
                fweights[e] = f16_read(buf, e);
            }
        }

        fprintf(stderr, "\n[%lu] %s (%lux%lux%lu = %lu, type=%u, %lu bytes)",
                (unsigned long)i, name, dims[0], dims[1], dims[2], dims[3],
                (unsigned long)total_elements, type, (unsigned long)weight_bytes);

        analyze_tensor(name, fweights, (int)total_elements);

        free(fweights);
        free(buf);
        fseek(f, saved_pos, SEEK_SET);
        analyzed++;
    }

    fprintf(stderr, "\n================================================\n");
    fprintf(stderr, "Analyzed: %d tensors\n", analyzed);

    fclose(f);
    return 0;
}
