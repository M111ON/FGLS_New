/* ============================================================
 * test_geo1state_roundtrip.c — Full encode→pack→unpack→decode
 *
 * Layout: [mean f32][R f32][max_delta fp16][deltas N-bit×32][k 6-bit×32]
 * Size:   10 + ceil(32×N/8) + ceil(32×6/8) bytes
 *
 * Decode: weight = mean + k×R + dequant_delta
 * Grid:   position[k] = mean + k × R  (R = std of block)
 *
 * CRITICAL: decoder MUST know k + mean.
 * Delta alone is insufficient — it's the RESIDUAL after mean+k×R.
 *
 * Compile: gcc -O2 -Wall -Werror -o test_geo1state_roundtrip.exe \
 *          test_geo1state_roundtrip.c -lm
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define Q8_BLOCK_SZ    32
#define Q8_BLOCK_BYTES 34
#define MAX_GRID_K     16

/* ============================================================
 * fp16 ↔ float — IEEE 754 half-precision
 * ============================================================ */
static float fp16_to_float(uint16_t h) {
    int sign = (h >> 15) & 1;
    int exp  = (h >> 10) & 0x1f;
    int mant = h & 0x3ff;
    if (exp == 0)  return (sign ? -1 : 1) * ldexp(mant, -24);
    if (exp == 31) return (sign ? -1 : 1) * INFINITY;
    return (sign ? -1 : 1) * ldexp(1.0 + mant / 1024.0, exp - 15);
}

static uint16_t float_to_fp16(float f) {
    if (f == 0.0f) return 0;
    int sign = (f < 0) ? 1 : 0;
    if (sign) f = -f;
    int exp;
    float norm = frexpf(f, &exp);
    norm *= 2.0f;
    exp -= 1;
    int biased_exp = exp + 15;
    if (biased_exp <= 0) { norm = ldexp(norm, biased_exp - 1); biased_exp = 0; }
    if (biased_exp >= 31) return (uint16_t)((sign << 15) | 0x7c00);
    int mant = (int)((norm - 1.0f) * 1024.0f) & 0x3ff;
    return (uint16_t)((sign << 15) | (biased_exp << 10) | mant);
}

static float q8_dequant(int8_t q, uint16_t sc16) {
    return q * fp16_to_float(sc16);
}

/* ============================================================
 * Bit pack/unpack — correct cross-byte handling
 * ============================================================ */
static void pack_bits(uint8_t *out, const int *values, int n, int bits) {
    memset(out, 0, (n * bits + 7) / 8);
    for (int i = 0; i < n; i++) {
        int bp = i * bits, bi = bp / 8, bo = bp % 8;
        unsigned int uv = (unsigned int)(values[i]) & ((1u << bits) - 1);
        out[bi] |= (uint8_t)(uv << bo);
        int rem = 8 - bo;
        if (bits > rem && bi + 1 < (n * bits + 7) / 8)
            out[bi + 1] |= (uint8_t)(uv >> rem);
    }
}

static void unpack_bits(int *values, const uint8_t *in, int n, int bits) {
    for (int i = 0; i < n; i++) {
        int bp = i * bits, bi = bp / 8, bo = bp % 8;
        int rem = 8 - bo;
        unsigned int uv;
        if (bits <= rem) {
            uv = ((unsigned int)in[bi] >> bo) & ((1u << bits) - 1);
        } else {
            int mask1 = (1u << rem) - 1;
            int bits2 = bits - rem;
            uv = ((unsigned int)in[bi] >> bo) & mask1;
            if (bi + 1 < 256)
                uv |= ((unsigned int)in[bi + 1] & ((1u << bits2) - 1)) << rem;
        }
        if (uv & (1u << (bits - 1)))
            values[i] = (int)(uv | (~0u << bits));
        else
            values[i] = (int)uv;
    }
}

/* ============================================================
 * Packed block + decode output
 * ============================================================ */
typedef struct {
    uint8_t  data[128];
    int      size;
    int      delta_bits;
} PackedBlockK;

typedef struct {
    float weights[Q8_BLOCK_SZ];
    int   grid_k[Q8_BLOCK_SZ];
    float grid_values[Q8_BLOCK_SZ];
    float dequant_deltas[Q8_BLOCK_SZ];
} DecodedBlock;

/* ============================================================
 * ENCODE
 * ============================================================ */
static PackedBlockK geo1state_encode(const float *weights, int n, int delta_bits) {
    PackedBlockK pb;
    memset(&pb, 0, sizeof(pb));
    pb.delta_bits = delta_bits;

    float sum = 0;
    for (int i = 0; i < n; i++) sum += weights[i];
    float mean = sum / n;

    float sum_sq = 0;
    for (int i = 0; i < n; i++) {
        float d = weights[i] - mean;
        sum_sq += d * d;
    }
    float R = sqrtf(sum_sq / n);
    if (R < 1e-10f) R = 1.0f;

    int max_val = (1 << delta_bits) - 1;
    float max_delta = 0;
    int k_values[Q8_BLOCK_SZ];
    float raw_deltas[Q8_BLOCK_SZ];

    for (int i = 0; i < n; i++) {
        float k_f = roundf((weights[i] - mean) / R);
        if (k_f < -MAX_GRID_K) k_f = -MAX_GRID_K;
        if (k_f > MAX_GRID_K - 1) k_f = MAX_GRID_K - 1;
        k_values[i] = (int)k_f;
        float ideal = mean + k_values[i] * R;
        raw_deltas[i] = weights[i] - ideal;
        float ad = fabsf(raw_deltas[i]);
        if (ad > max_delta) max_delta = ad;
    }
    float delta_scale = (max_delta > 1e-10f) ? max_delta : 1.0f;

    int quantized[Q8_BLOCK_SZ];
    for (int i = 0; i < n; i++) {
        float norm = raw_deltas[i] / delta_scale;
        int q = (int)roundf(norm * max_val);
        if (q < -max_val) q = -max_val;
        if (q > max_val)  q = max_val;
        quantized[i] = q;
    }

    /* Pack: [mean f32][R f32][max_delta fp16][deltas][k] */
    memcpy(pb.data,      &mean, 4);
    memcpy(pb.data + 4,  &R, 4);
    pb.data[8] = (uint8_t)(float_to_fp16(max_delta) & 0xff);
    pb.data[9] = (uint8_t)((float_to_fp16(max_delta) >> 8) & 0xff);

    int delta_bytes = (n * delta_bits + 7) / 8;
    pack_bits(pb.data + 10, quantized, n, delta_bits);

    int k_offset = 10 + delta_bytes;
    int k_packed[Q8_BLOCK_SZ];
    for (int i = 0; i < n; i++)
        k_packed[i] = k_values[i] + MAX_GRID_K;
    pack_bits(pb.data + k_offset, k_packed, n, 6);

    pb.size = k_offset + (n * 6 + 7) / 8;
    return pb;
}

/* ============================================================
 * DECODE
 * ============================================================ */
static DecodedBlock geo1state_decode(const PackedBlockK *pb, int n) {
    DecodedBlock db;
    memset(&db, 0, sizeof(db));

    float mean_val, R;
    memcpy(&mean_val, pb->data, 4);
    memcpy(&R, pb->data + 4, 4);
    uint16_t md16 = (uint16_t)pb->data[8] | ((uint16_t)pb->data[9] << 8);
    float max_delta = fp16_to_float(md16);
    int max_val = (1 << pb->delta_bits) - 1;
    float delta_scale = (max_delta > 1e-10f) ? max_delta : 1.0f;

    int quantized[Q8_BLOCK_SZ];
    unpack_bits(quantized, pb->data + 10, n, pb->delta_bits);

    int delta_bytes = (n * pb->delta_bits + 7) / 8;
    int k_offset = 10 + delta_bytes;
    int k_packed[Q8_BLOCK_SZ];
    unpack_bits(k_packed, pb->data + k_offset, n, 6);

    for (int i = 0; i < n; i++) {
        int k = k_packed[i] - MAX_GRID_K;
        db.grid_k[i] = k;
        db.grid_values[i] = mean_val + k * R;
        db.dequant_deltas[i] = (float)quantized[i] * delta_scale / max_val;
        db.weights[i] = db.grid_values[i] + db.dequant_deltas[i];
    }

    return db;
}

/* ============================================================
 * Unit tests
 * ============================================================ */
static int test_synthetic(void) {
    printf("=== Unit Test: Synthetic Block ===\n");
    int pass = 0, fail = 0;

#define T(expr, msg) do { \
    if (expr) { pass++; printf("  PASS  %s\n", msg); } \
    else      { fail++; printf("  FAIL  %s (line %d)\n", msg, __LINE__); } \
} while(0)

    /* T1: Identical weights → lossless */
    {
        float w[32];
        for (int i = 0; i < 32; i++) w[i] = 7.5f;
        PackedBlockK pb = geo1state_encode(w, 32, 4);
        DecodedBlock db = geo1state_decode(&pb, 32);
        int exact = 1;
        for (int i = 0; i < 32; i++)
            if (fabsf(w[i] - db.weights[i]) > 1e-5f) { exact = 0; break; }
        T(exact, "T1: Identical weights → lossless");
    }

    /* T2: Narrow range */
    {
        float w[32];
        unsigned int seed = 42;
        for (int i = 0; i < 32; i++) {
            seed = seed * 1103515245 + 12345;
            w[i] = 1.0f + (float)((int)(seed % 20) - 10) * 0.01f;
        }
        PackedBlockK pb = geo1state_encode(w, 32, 4);
        DecodedBlock db = geo1state_decode(&pb, 32);
        float max_err = 0;
        for (int i = 0; i < 32; i++) {
            float e = fabsf(w[i] - db.weights[i]);
            if (e > max_err) max_err = e;
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "T2: Narrow range: max_err=%.6f", max_err);
        T(max_err < 0.1f, msg);
    }

    /* T3: Wide symmetric range */
    {
        float w[32];
        for (int i = 0; i < 32; i++) w[i] = (float)(i - 16) * 1.0f;
        PackedBlockK pb = geo1state_encode(w, 32, 4);
        DecodedBlock db = geo1state_decode(&pb, 32);
        float max_err = 0;
        for (int i = 0; i < 32; i++) {
            float e = fabsf(w[i] - db.weights[i]);
            if (e > max_err) max_err = e;
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "T3: Wide range: max_err=%.6f", max_err);
        T(max_err < 6.0f, msg);
    }

    /* T4: k pack/unpack */
    {
        int test_k[] = {-16, -10, -1, 0, 1, 10, 15};
        int n_k = 7, packed[7], unpacked[7];
        for (int i = 0; i < n_k; i++) packed[i] = test_k[i] + MAX_GRID_K;
        uint8_t buf[8];
        pack_bits(buf, packed, n_k, 6);
        unpack_bits(unpacked, buf, n_k, 6);
        int ok = 1;
        for (int i = 0; i < n_k; i++)
            if (unpacked[i] != packed[i]) { ok = 0; break; }
        T(ok, "T4: k pack/unpack roundtrip (6-bit)");
    }

    /* T5: delta pack/unpack */
    {
        int test_d[] = {-7, -3, 0, 3, 7};
        int n_d = 5, packed[5], unpacked[5];
        for (int i = 0; i < n_d; i++) packed[i] = test_d[i];
        uint8_t buf[4];
        pack_bits(buf, packed, n_d, 4);
        unpack_bits(unpacked, buf, n_d, 4);
        int ok = 1;
        for (int i = 0; i < n_d; i++)
            if (unpacked[i] != packed[i]) { ok = 0; break; }
        T(ok, "T5: delta pack/unpack (4-bit)");
    }

    /* T6: 2-bit delta */
    {
        int test_d[] = {-1, 0, 1};
        int packed[3], unpacked[3];
        for (int i = 0; i < 3; i++) packed[i] = test_d[i];
        uint8_t buf[4];
        pack_bits(buf, packed, 3, 2);
        unpack_bits(unpacked, buf, 3, 2);
        T(unpacked[0]==-1 && unpacked[1]==0 && unpacked[2]==1, "T6: 2-bit delta");
    }

    /* T7: 8-bit full roundtrip */
    {
        float w[32];
        unsigned int seed = 99;
        for (int i = 0; i < 32; i++) {
            seed = seed * 1103515245 + 12345;
            w[i] = (float)((int)(seed % 200) - 100) * 0.01f;
        }
        PackedBlockK pb = geo1state_encode(w, 32, 8);
        DecodedBlock db = geo1state_decode(&pb, 32);
        float max_err = 0;
        for (int i = 0; i < 32; i++) {
            float e = fabsf(w[i] - db.weights[i]);
            if (e > max_err) max_err = e;
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "T7: 8-bit: max_err=%.6f", max_err);
        T(max_err < 0.5f, msg);
    }

    /* T8: 4-bit full roundtrip */
    {
        float w[32];
        unsigned int seed = 77;
        for (int i = 0; i < 32; i++) {
            seed = seed * 1103515245 + 12345;
            w[i] = (float)((int)(seed % 200) - 100) * 0.01f;
        }
        PackedBlockK pb = geo1state_encode(w, 32, 4);
        DecodedBlock db = geo1state_decode(&pb, 32);
        float max_err = 0;
        for (int i = 0; i < 32; i++) {
            float e = fabsf(w[i] - db.weights[i]);
            if (e > max_err) max_err = e;
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "T8: 4-bit: max_err=%.6f", max_err);
        T(max_err < 1.0f, msg);
    }

    /* T9: 1-bit full roundtrip */
    {
        float w[32];
        unsigned int seed = 55;
        for (int i = 0; i < 32; i++) {
            seed = seed * 1103515245 + 12345;
            w[i] = (float)((int)(seed % 200) - 100) * 0.01f;
        }
        PackedBlockK pb = geo1state_encode(w, 32, 1);
        DecodedBlock db = geo1state_decode(&pb, 32);
        float max_err = 0;
        for (int i = 0; i < 32; i++) {
            float e = fabsf(w[i] - db.weights[i]);
            if (e > max_err) max_err = e;
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "T9: 1-bit: max_err=%.6f", max_err);
        T(max_err < 5.0f, msg);
    }

    /* T10: Pack sizes */
    {
        float w[32];
        for (int i = 0; i < 32; i++) w[i] = (float)i * 0.1f;
        PackedBlockK pb4 = geo1state_encode(w, 32, 4);
        PackedBlockK pb2 = geo1state_encode(w, 32, 2);
        PackedBlockK pb8 = geo1state_encode(w, 32, 8);
        char msg[128];
        snprintf(msg, sizeof(msg), "T10a: 4-bit: %d bytes (expect 50)", pb4.size);
        T(pb4.size == 50, msg);
        snprintf(msg, sizeof(msg), "T10b: 2-bit: %d bytes (expect 42)", pb2.size);
        T(pb2.size == 42, msg);
        snprintf(msg, sizeof(msg), "T10c: 8-bit: %d bytes (expect 66)", pb8.size);
        T(pb8.size == 66, msg);
    }

    /* T11: fp16 roundtrip precision */
    {
        float vals[] = {0.01f, 0.1f, 0.29f, 1.0f, 10.0f, 100.0f};
        int ok = 1;
        for (int i = 0; i < 6; i++) {
            uint16_t h = float_to_fp16(vals[i]);
            float back = fp16_to_float(h);
            float rel = fabsf(vals[i] - back) / vals[i];
            if (rel > 0.005f) {
                ok = 0;
                printf("    fp16: %.4f -> %.6f (rel=%.4f)\n", vals[i], back, rel);
            }
        }
        T(ok, "T11: fp16 roundtrip <0.5% error");
    }

    printf("  %d PASS / %d FAIL\n\n", pass, fail);
    return fail;
}

/* ============================================================
 * GGUF roundtrip test
 * ============================================================ */
static int test_gguf_roundtrip(const char *filename, int max_blocks) {
    printf("=== GGUF Roundtrip Test ===\n");
    printf("File: %s\n", filename);

    FILE *f = fopen(filename, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", filename); return 1; }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    fprintf(stderr, "Size: %.1f MB\n", file_size / 1048576.0f);

    fseek(f, 4096, SEEK_SET);

    int blocks_tested = 0, consecutive = 0;
    int delta_bits_list[] = {8, 6, 4, 2, 1};
    int n_bits = 5;
    long total_packed[5] = {0};
    float total_error[5] = {0};
    int block_count[5] = {0};
    uint8_t buf[34];

    while (blocks_tested < max_blocks) {
        if (fread(buf, 1, 34, f) != 34) break;

        uint16_t sc16;
        memcpy(&sc16, buf + 32, 2);
        int exp = (sc16 >> 10) & 0x1f;
        int valid = (sc16 != 0 && sc16 != 0x7c00 && sc16 != 0xfc00
                     && exp > 0 && exp < 30);
        if (valid) { consecutive++; if (consecutive < 4) continue; }
        else { consecutive = 0; continue; }

        float weights[Q8_BLOCK_SZ];
        for (int i = 0; i < Q8_BLOCK_SZ; i++)
            weights[i] = q8_dequant((int8_t)buf[i], sc16);

        float wmin = weights[0], wmax = weights[0];
        for (int i = 1; i < Q8_BLOCK_SZ; i++) {
            if (weights[i] < wmin) wmin = weights[i];
            if (weights[i] > wmax) wmax = weights[i];
        }
        float range = wmax - wmin;
        /* Skip non-weight blocks: zero range or extreme range */
        if (range < 1e-10f || range > 1000.0f) continue;

        for (int b = 0; b < n_bits; b++) {
            PackedBlockK pb = geo1state_encode(weights, Q8_BLOCK_SZ, delta_bits_list[b]);
            DecodedBlock decoded = geo1state_decode(&pb, Q8_BLOCK_SZ);

            float sum_err = 0;
            for (int i = 0; i < Q8_BLOCK_SZ; i++)
                sum_err += fabsf(weights[i] - decoded.weights[i]);

            total_packed[b] += pb.size;
            total_error[b] += 100.0f * (sum_err / Q8_BLOCK_SZ) / range;
            block_count[b]++;
        }
        blocks_tested++;
        if (blocks_tested % 500 == 0)
            fprintf(stderr, "  %d blocks...\r", blocks_tested);
    }
    fclose(f);

    printf("\nBlocks tested: %d\n\n", blocks_tested);
    printf("%-8s  %8s  %8s  %10s  %10s\n", "Bits", "Bytes", "Ratio", "Error%", "Status");
    printf("%-8s  %8s  %8s  %10s  %10s\n", "----", "-----", "-----", "------", "------");

    int any_fail = 0;
    for (int b = 0; b < n_bits; b++) {
        if (block_count[b] == 0) continue;
        float avg_bytes = (float)total_packed[b] / block_count[b];
        float avg_error = total_error[b] / (float)block_count[b];
        float ratio = avg_bytes / 34.0f;
        const char *status;
        if (avg_error < 0.01f) status = "EXACT";
        else if (avg_error < 0.2f) status = "OK";
        else if (avg_error < 1.0f) status = "GOOD";
        else { status = "WARN"; any_fail = 1; }
        printf("%-8s  %7.1fB  %7.2fx  %9.2f%%  %10s\n",
               (b==0?"8-bit":b==1?"6-bit":b==2?"4-bit":b==3?"2-bit":"1-bit"),
               avg_bytes, ratio, avg_error, status);
    }
    printf("\n");
    return any_fail;
}

int main(int argc, char **argv) {
    int fail = test_synthetic();
    if (argc >= 2) {
        int max_blocks = (argc > 2) ? atoi(argv[2]) : 2000;
        fail += test_gguf_roundtrip(argv[1], max_blocks);
    } else {
        printf("Usage: %s <model.gguf> [max_blocks]\n\n", argv[0]);
    }
    printf("=== FINAL: %s ===\n", fail ? "FAIL" : "ALL PASS");
    return fail ? 1 : 0;
}
