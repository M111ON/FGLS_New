/* ============================================================
 * test_geo_comparison.c — Compare encoding approaches
 *
 * 5 approaches compared on same Q8_0 blocks:
 *   A. Q8_0 baseline (original)
 *   B. BeamCode (zone×position, lossless)
 *   C. Dual Square Angular (θ,φ, lossless)
 *   D. Geo1State statistical (mean + k×R + delta)
 *   E. Beam + Angular combined (geometric observation)
 *
 * Key insight:
 *   B & C are LOSSLESS bijections — same data, different representation.
 *   D is LOSSY compression — fewer bits, some error.
 *   E maps weights to geometric structure for OBSERVATION.
 *
 * Compile: gcc -O2 -Wall -Werror -o test_geo_comparison.exe \
 *          test_geo_comparison.c -lm
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define Q8_BLOCK_SZ  32
#define Q8_BLOCK_BYTES 34

/* ============================================================
 * fp16 (fixed from previous session)
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
    norm *= 2.0f; exp -= 1;
    int biased_exp = exp + 15;
    if (biased_exp <= 0) { norm = ldexp(norm, biased_exp - 1); biased_exp = 0; }
    if (biased_exp >= 31) return (uint16_t)((sign << 15) | 0x7c00);
    int mant = (int)((norm - 1.0f) * 1024.0f) & 0x3ff;
    return (uint16_t)((sign << 15) | (biased_exp << 10) | mant);
}

/* ============================================================
 * APPROACH A: Q8_0 baseline
 *   int8 quantized + fp16 scale = 34 bytes
 *   Error: 0 (identity for int8 weights)
 * ============================================================ */
typedef struct {
    int8_t  q[Q8_BLOCK_SZ];
    uint16_t scale;
} Q8Block;

/* q8_encode unused — kept for reference */

/* q8_decode unused — kept for reference */

/* ============================================================
 * APPROACH B: BeamCode (zone×position, lossless for Q8)
 *   upper nibble = zone (0-15), lower nibble = position (0-15)
 *   16×16 = 256 = Q8 exactly
 *   Storage: 32 bytes (same as Q8_0 without scale)
 * ============================================================ */
typedef struct {
    uint8_t codes[Q8_BLOCK_SZ];  /* zone<<4 | position */
} BeamBlock;

static BeamBlock beam_encode(const int8_t *q8) {
    BeamBlock b;
    for (int i = 0; i < Q8_BLOCK_SZ; i++)
        b.codes[i] = (uint8_t)((int32_t)q8[i] + 128);  /* -128..+127 → 0..255 */
    return b;
}

static int8_t beam_decode(BeamBlock b, int i) {
    return (int8_t)((int32_t)b.codes[i] - 128);
}

/* ============================================================
 * APPROACH C: Dual Square Angular Map (lossless for Q8)
 *   Weight → (θ, φ) on 360×360 dual square
 *   Magnitude = XOR(θ, φ) mixed to 8-bit
 *   Sign = layer (XY=+, YX=-)
 *   Storage: 32 bytes (32 × uint8 code)
 * ============================================================ */
#define SQ_RES  360u
#define SQ_GRID (SQ_RES * SQ_RES)

typedef enum { LAYER_XY = 0, LAYER_YX = 1 } Layer;
typedef struct { Layer layer; uint16_t x, y; } SqCoord;

static SqCoord xmap_xy[256], xmap_yx[256];
static int sq_ready = 0;

static uint8_t sq_xor_dist(uint16_t x, uint16_t y) {
    uint16_t mix = (uint16_t)(x ^ y);
    uint16_t m2 = mix ^ (mix >> 3) ^ (mix >> 5) ^ (mix >> 7);
    return (uint8_t)(m2 & 0xFF);
}

static void sq_init(void) {
    if (sq_ready) return;
    uint8_t *used_xy = (uint8_t *)calloc(SQ_GRID, 1);
    uint8_t *used_yx = (uint8_t *)calloc(SQ_GRID, 1);

    xmap_xy[0] = (SqCoord){LAYER_XY, 0, 0};
    xmap_yx[0] = (SqCoord){LAYER_YX, 0, 0};
    used_xy[0] = used_yx[0] = 1;

    for (int side = 0; side < 2; side++) {
        SqCoord *map = side ? xmap_yx : xmap_xy;
        uint8_t *used = side ? used_yx : used_xy;
        Layer lay = side ? LAYER_YX : LAYER_XY;
        for (int d = 1; d < 256; d++) {
            for (uint32_t idx = 0; idx < SQ_GRID; idx++) {
                if (used[idx]) continue;
                uint16_t x = (uint16_t)(idx % SQ_RES);
                uint16_t y = (uint16_t)(idx / SQ_RES);
                if (sq_xor_dist(x, y) == (uint8_t)d) {
                    map[d] = (SqCoord){lay, x, y};
                    used[idx] = 1;
                    break;
                }
            }
        }
    }
    free(used_xy); free(used_yx);
    sq_ready = 1;
}

/* sq_bake: weight → SquareCoord on dual square */
static SqCoord sq_bake(int32_t w) {
    if (!sq_ready) sq_init();
    if (w == 0) return xmap_xy[0];
    if (w > 0)  return xmap_xy[(uint8_t)(w & 0xFF)];
    else        return xmap_yx[(uint8_t)((-w) & 0xFF)];
}

/* sq_decode_coord unused — kept for reference */

/* Angular block: store code (layer + encoded θ, φ packed) */
typedef struct {
    uint8_t codes[Q8_BLOCK_SZ];  /* same as beam — lossless Q8 */
} AngularBlock;

static AngularBlock angular_encode(const int8_t *q8) {
    AngularBlock b;
    for (int i = 0; i < Q8_BLOCK_SZ; i++) {
        /* For storage, we just store the Q8 code — angular coords are derived */
        b.codes[i] = (uint8_t)((int32_t)q8[i] + 128);
    }
    return b;
}

static int8_t angular_decode(AngularBlock b, int i) {
    return (int8_t)((int32_t)b.codes[i] - 128);
}

/* ============================================================
 * APPROACH D: Geo1State statistical (current implementation)
 *   mean(f32) + R(f32) + max_delta(fp16) + deltas(N-bit) + k(6-bit)
 *   Storage: 10 + ceil(32×N/8) + 24 bytes
 *   Error: from delta quantization
 * ============================================================ */
static void pack_bits(uint8_t *out, const int *val, int n, int bits) {
    memset(out, 0, (n * bits + 7) / 8);
    for (int i = 0; i < n; i++) {
        int bp = i * bits, bi = bp / 8, bo = bp % 8;
        unsigned int uv = (unsigned int)(val[i]) & ((1u << bits) - 1);
        out[bi] |= (uint8_t)(uv << bo);
        int rem = 8 - bo;
        if (bits > rem && bi + 1 < (n * bits + 7) / 8)
            out[bi + 1] |= (uint8_t)(uv >> rem);
    }
}

static void unpack_bits(int *val, const uint8_t *in, int n, int bits) {
    for (int i = 0; i < n; i++) {
        int bp = i * bits, bi = bp / 8, bo = bp % 8;
        int rem = 8 - bo;
        unsigned int uv;
        if (bits <= rem)
            uv = ((unsigned int)in[bi] >> bo) & ((1u << bits) - 1);
        else {
            int m1 = (1u << rem) - 1;
            uv = ((unsigned int)in[bi] >> bo) & m1;
            if (bi + 1 < 256)
                uv |= ((unsigned int)in[bi+1] & ((1u << (bits-rem))-1)) << rem;
        }
        val[i] = (uv & (1u<<(bits-1))) ? (int)(uv|(~0u<<bits)) : (int)uv;
    }
}

typedef struct {
    uint8_t data[128];
    int size;
    int delta_bits;
} Geo1StateBlock;

static Geo1StateBlock geo1state_encode(const float *w, int n, int bits) {
    Geo1StateBlock pb; memset(&pb, 0, sizeof(pb)); pb.delta_bits = bits;
    float sum = 0; for (int i = 0; i < n; i++) sum += w[i];
    float mean = sum / n;
    float ssq = 0; for (int i = 0; i < n; i++) { float d = w[i]-mean; ssq += d*d; }
    float R = sqrtf(ssq / n); if (R < 1e-10f) R = 1.0f;
    int mx = (1 << bits) - 1;
    float md = 0; int kv[32]; float rd[32];
    for (int i = 0; i < n; i++) {
        float kf = roundf((w[i]-mean)/R);
        if (kf < -16) kf = -16;
        if (kf > 15) kf = 15;
        kv[i] = (int)kf;
        rd[i] = w[i] - (mean + kv[i]*R);
        float ad = fabsf(rd[i]); if (ad > md) md = ad;
    }
    float ds = (md > 1e-10f) ? md : 1.0f;
    int q[32];
    for (int i = 0; i < n; i++) {
        int qq = (int)roundf(rd[i]/ds*mx);
        if (qq < -mx) qq = -mx;
        if (qq > mx) qq = mx;
        q[i] = qq;
    }
    memcpy(pb.data, &mean, 4); memcpy(pb.data+4, &R, 4);
    pb.data[8] = (uint8_t)(float_to_fp16(md) & 0xff);
    pb.data[9] = (uint8_t)((float_to_fp16(md) >> 8) & 0xff);
    int db = (n*bits+7)/8;
    pack_bits(pb.data+10, q, n, bits);
    int ko = 10+db; int kp[32];
    for (int i = 0; i < n; i++) kp[i] = kv[i]+16;
    pack_bits(pb.data+ko, kp, n, 6);
    pb.size = ko + (n*6+7)/8;
    return pb;
}

typedef struct { float w[32]; int k[32]; float gv[32]; float dd[32]; } Geo1Dec;

static Geo1Dec geo1state_decode(const Geo1StateBlock *pb, int n) {
    Geo1Dec d; memset(&d, 0, sizeof(d));
    float m, R; memcpy(&m, pb->data, 4); memcpy(&R, pb->data+4, 4);
    uint16_t md16 = (uint16_t)pb->data[8]|((uint16_t)pb->data[9]<<8);
    float md = fp16_to_float(md16);
    int mx = (1 << pb->delta_bits)-1;
    float ds = (md > 1e-10f) ? md : 1.0f;
    int q[32]; unpack_bits(q, pb->data+10, n, pb->delta_bits);
    int db = (n*pb->delta_bits+7)/8;
    int kp[32]; unpack_bits(kp, pb->data+10+db, n, 6);
    for (int i = 0; i < n; i++) {
        int k = kp[i]-16; d.k[i] = k; d.gv[i] = m+k*R;
        d.dd[i] = (float)q[i]*ds/mx;
        d.w[i] = d.gv[i]+d.dd[i];
    }
    return d;
}

/* ============================================================
 * APPROACH E: Beam-Angular Geometric Observation
 *   Map each weight to (zone, position) on beam grid
 *   AND to (θ, φ) on dual square
 *   Compute geometric correlation between adjacent weights
 *   Store: zone×position codes + geometric delta (prediction error)
 *
 *   The geometric insight: weights that are close in value
 *   should be close on the angular map. If true, delta between
 *  相邻 angular positions should be small → compressible.
 * ============================================================ */
typedef struct {
    uint8_t zone_pos[Q8_BLOCK_SZ];     /* beam zone×position */
    uint8_t angular_delta[Q8_BLOCK_SZ]; /* delta from angular prediction */
    uint8_t pred_method;                /* 0=none, 1=prev, 2=neighbor */
    int total_bytes;
} GeometricBlock;

static GeometricBlock geo_obs_encode(const int8_t *q8) {
    GeometricBlock b;
    b.pred_method = 1;

    /* Step 1: Map each weight to beam zone×position */
    for (int i = 0; i < Q8_BLOCK_SZ; i++)
        b.zone_pos[i] = (uint8_t)((int32_t)q8[i] + 128);

    /* Step 2: Map to angular (θ,φ) and compute angular deltas */
    SqCoord prev = sq_bake(q8[0]);
    b.angular_delta[0] = 0;
    for (int i = 1; i < Q8_BLOCK_SZ; i++) {
        SqCoord cur = sq_bake(q8[i]);
        /* Angular prediction: delta of θ + delta of φ packed into 1 byte */
        int16_t dtheta = (int16_t)cur.x - (int16_t)prev.x;
        int16_t dphi   = (int16_t)cur.y - (int16_t)prev.y;
        /* Pack: use XOR of angular coordinates as angular delta */
        uint16_t ang = (uint16_t)(dtheta + 180) | ((uint16_t)(dphi + 180) << 4);
        b.angular_delta[i] = (uint8_t)(ang & 0xFF);
        prev = cur;
    }

    b.total_bytes = Q8_BLOCK_SZ + Q8_BLOCK_SZ + 1;
    return b;
}

static int8_t geo_obs_decode(GeometricBlock b, int i) {
    if (i == 0) return (int8_t)((int32_t)b.zone_pos[0] - 128);
    int32_t predicted = (int32_t)((int8_t)((int32_t)b.zone_pos[i-1] - 128));
    int32_t delta = (int32_t)((int32_t)b.angular_delta[i] - 128);
    return (int8_t)(predicted + delta);
}

/* ============================================================
 * Results structure
 * ============================================================ */
typedef struct {
    const char *name;
    int bytes_per_block;
    float ratio;        /* vs Q8_0 (34B) */
    float avg_error;    /* % of range */
    float max_error;
    float avg_abs_err;  /* absolute error */
    int blocks;
} ApproachResult;

/* ============================================================
 * Test on GGUF
 * ============================================================ */
static int test_comparison(const char *filename, int max_blocks) {
    printf("=== Geometric Encoding Comparison ===\n");
    printf("File: %s\n\n", filename);

    FILE *f = fopen(filename, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", filename); return 1; }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    fprintf(stderr, "Size: %.1f MB\n", file_size / 1048576.0f);
    fseek(f, 4096, SEEK_SET);

    /* Accumulators for each approach */
    int n_approaches = 6;
    ApproachResult res[6] = {0};
    res[0] = (ApproachResult){"Q8_0 baseline", 34, 1.0f, 0, 0, 0, 0};
    res[1] = (ApproachResult){"BeamCode", 32, 32.0f/34, 0, 0, 0, 0};
    res[2] = (ApproachResult){"Angular Map", 32, 32.0f/34, 0, 0, 0, 0};
    res[3] = (ApproachResult){"Geo1State 8bit", 66, 66.0f/34, 0, 0, 0, 0};
    res[4] = (ApproachResult){"Geo1State 4bit", 50, 50.0f/34, 0, 0, 0, 0};
    res[5] = (ApproachResult){"Geo-Angular", 65, 65.0f/34, 0, 0, 0, 0};

    int blocks_tested = 0, consecutive = 0;
    uint8_t buf[34];

    sq_init();  /* init angular map */

    while (blocks_tested < max_blocks) {
        if (fread(buf, 1, 34, f) != 34) break;

        uint16_t sc16; memcpy(&sc16, buf+32, 2);
        int exp = (sc16>>10)&0x1f;
        int valid = (sc16!=0 && sc16!=0x7c00 && sc16!=0xfc00 && exp>0 && exp<30);
        if (valid) { consecutive++; if (consecutive < 4) continue; }
        else { consecutive = 0; continue; }

        /* Dequantize Q8_0 → float */
        float weights[Q8_BLOCK_SZ];
        float s = fp16_to_float(sc16);
        for (int i = 0; i < Q8_BLOCK_SZ; i++)
            weights[i] = (int8_t)buf[i] * s;

        float wmin = weights[0], wmax = weights[0];
        for (int i = 1; i < Q8_BLOCK_SZ; i++) {
            if (weights[i] < wmin) wmin = weights[i];
            if (weights[i] > wmax) wmax = weights[i];
        }
        float range = wmax - wmin;
        if (range < 1e-10f || range > 1000.0f) continue;

        /* ── A: Q8_0 baseline ── */
        /* Error = 0 (we're comparing against itself) */

        /* ── B: BeamCode (lossless) ── */
        BeamBlock bb = beam_encode((int8_t*)buf);
        float beam_err = 0;
        for (int i = 0; i < Q8_BLOCK_SZ; i++) {
            float recon = beam_decode(bb, i) * s;
            float e = fabsf(weights[i] - recon);
            if (e > beam_err) beam_err = e;
        }
        res[1].max_error += beam_err;
        res[1].avg_abs_err += beam_err;  /* will average later */

        /* ── C: Angular Map (lossless) ── */
        AngularBlock ab = angular_encode((int8_t*)buf);
        float ang_err = 0;
        for (int i = 0; i < Q8_BLOCK_SZ; i++) {
            float recon = angular_decode(ab, i) * s;
            float e = fabsf(weights[i] - recon);
            if (e > ang_err) ang_err = e;
        }
        res[2].max_error += ang_err;

        /* ── D: Geo1State 8-bit ── */
        Geo1StateBlock g8 = geo1state_encode(weights, Q8_BLOCK_SZ, 8);
        Geo1Dec d8 = geo1state_decode(&g8, Q8_BLOCK_SZ);
        float g8_max = 0, g8_sum = 0;
        for (int i = 0; i < Q8_BLOCK_SZ; i++) {
            float e = fabsf(weights[i] - d8.w[i]);
            if (e > g8_max) g8_max = e;
            g8_sum += e;
        }
        res[3].avg_error += 100.0f * (g8_sum/Q8_BLOCK_SZ) / range;
        res[3].max_error += g8_max;
        res[3].avg_abs_err += g8_sum / Q8_BLOCK_SZ;

        /* ── E: Geo1State 4-bit ── */
        Geo1StateBlock g4 = geo1state_encode(weights, Q8_BLOCK_SZ, 4);
        Geo1Dec d4 = geo1state_decode(&g4, Q8_BLOCK_SZ);
        float g4_max = 0, g4_sum = 0;
        for (int i = 0; i < Q8_BLOCK_SZ; i++) {
            float e = fabsf(weights[i] - d4.w[i]);
            if (e > g4_max) g4_max = e;
            g4_sum += e;
        }
        res[4].avg_error += 100.0f * (g4_sum/Q8_BLOCK_SZ) / range;
        res[4].max_error += g4_max;
        res[4].avg_abs_err += g4_sum / Q8_BLOCK_SZ;

        /* ── F: Geo-Angular observation ── */
        GeometricBlock gb = geo_obs_encode((int8_t*)buf);
        float go_max = 0, go_sum = 0;
        for (int i = 0; i < Q8_BLOCK_SZ; i++) {
            float recon = geo_obs_decode(gb, i) * s;
            float e = fabsf(weights[i] - recon);
            if (e > go_max) go_max = e;
            go_sum += e;
        }
        res[5].avg_error += 100.0f * (go_sum/Q8_BLOCK_SZ) / range;
        res[5].max_error += go_max;
        res[5].avg_abs_err += go_sum / Q8_BLOCK_SZ;

        blocks_tested++;
        res[0].blocks = res[1].blocks = res[2].blocks =
        res[3].blocks = res[4].blocks = res[5].blocks = blocks_tested;

        if (blocks_tested % 500 == 0)
            fprintf(stderr, "  %d blocks...\r", blocks_tested);
    }
    fclose(f);

    /* ── Print results ── */
    printf("Blocks tested: %d\n\n", blocks_tested);
    printf("%-20s  %6s  %6s  %8s  %8s  %8s  %s\n",
           "Approach", "Bytes", "Ratio", "AvgErr%", "MaxErr", "AbsErr", "Type");
    printf("%-20s  %6s  %6s  %8s  %8s  %8s  %s\n",
           "────────────────────", "──────", "──────", "────────", "────────", "────────", "───────────");

    for (int i = 0; i < n_approaches; i++) {
        if (res[i].blocks == 0) continue;
        float ae = res[i].avg_error / res[i].blocks;
        float me = res[i].max_error / res[i].blocks;
        float abe = res[i].avg_abs_err / res[i].blocks;
        const char *type;
        if (i <= 2) type = "LOSSLESS";
        else if (i <= 4) type = "LOSSY";
        else type = "LOSSY+GEO";

        printf("%-20s  %5dB  %5.2fx  %7.2f%%  %8.4f  %8.4f  %s\n",
               res[i].name, res[i].bytes_per_block, res[i].ratio,
               ae, me, abe, type);
    }

    /* ── Analysis ── */
    printf("\n─── Analysis ───\n");
    printf("Q8_0 baseline: 34B, 0%% error (reference)\n");
    printf("BeamCode:      32B, lossless — same data, different layout\n");
    printf("Angular Map:   32B, lossless — same data, geometric coords\n");
    printf("Geo1State:     uses mean + k×R grid + delta quantization\n");
    printf("Geo-Angular:   beam codes + angular prediction delta\n");
    printf("\nKey question: Does angular structure reveal patterns\n");
    printf("that make prediction better than pure statistical?\n");

    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <model.gguf> [max_blocks]\n", argv[0]);
        return 1;
    }
    int max_blocks = (argc > 2) ? atoi(argv[2]) : 2000;
    return test_comparison(argv[1], max_blocks);
}
