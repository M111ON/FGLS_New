/*
 * pogls_bermuda.c — Implementation
 *
 * Stride-37 Hilbert routing with face traversal (orbit/chiral/cross/hub).
 * Diamond Shell: 3D rotation + fibo_intersect rotation discriminator.
 * RLE: run-length encoding with 3-byte minimum run.
 * Shadow bond: FNV-1a keyed ring buffer (144 entries).
 */

#include "pogls_bermuda.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════
   INTERNAL HELPERS
   ═══════════════════════════════════════════════════════════════════ */

/* Portable popcount for 64-bit */
static uint32_t _popcount64(uint64_t x)
{
    uint32_t c = 0;
    while (x) { c += (uint32_t)(x & 1U); x >>= 1; }
    return c;
}

/* Modular inverse of 37 modulo m (m must be coprime to 37) */
static uint32_t _modinv37(uint32_t m)
{
    int32_t g = (int32_t)m, x = 0, a0 = 37, x0 = 1;
    while (a0 != 0) {
        int32_t q = g / a0;
        int32_t t = a0; a0 = g - q * a0; g = t;
        t = x0; x0 = x - q * x0; x = t;
    }
    int32_t r = x % (int32_t)m;
    return (uint32_t)(r < 0 ? r + (int32_t)m : r);
}

/* Compute walk_len: smallest multiple of 12 >= slots, coprime to 37 */
static uint32_t _walk_len(uint32_t slots)
{
    uint32_t wl = slots;
    while (1) {
        if (wl % 12 == 0 && wl % 37 != 0) return wl;
        wl++;
    }
}

/* Hilbert encode via stride-37 */
static uint32_t _hilbert_enc(uint32_t pos, uint32_t N)
{
    return ((uint32_t)pos * POGLS_BERMUDA_STRIDE) % N;
}

/* Hilbert decode via modular inverse */
static uint32_t _hilbert_dec(uint32_t idx, uint32_t N, uint32_t inv37)
{
    return ((uint32_t)idx * inv37) % N;
}

/* ═══════════════════════════════════════════════════════════════════
   ROUTING
   ═══════════════════════════════════════════════════════════════════ */

/*
 * pogls_bermuda_route: stride-37 routing from source address to target face.
 *
 * stride=1 → 512-slot gear, stride=2 → 1024, stride=3 → 2048, stride=4 → 4096.
 * Face 0-11 selects the zone to route into.
 * Uses orbit/chiral/cross/hub modes based on face group.
 */
uint32_t pogls_bermuda_route(uint32_t from, uint32_t face, uint32_t stride)
{
    from   = from % POGLS_BERMUDA_MAX_ADDR;
    face   = face % POGLS_BERMUDA_N_ZONES;
    stride = (stride < 1) ? 1 : (stride > 4 ? 4 : stride);

    uint32_t slots = (uint32_t)512 << (stride - 1);
    uint32_t inv37 = _modinv37(slots);
    uint32_t wl    = _walk_len(slots);
    uint32_t fs    = wl / 12;

    uint32_t enc     = _hilbert_enc(from, wl);
    uint32_t zone    = enc / fs;
    uint32_t offset  = enc % fs;

    /* mode = face / 3, clamped to 0-3 (orbit/chiral/cross/hub) */
    uint32_t mode    = (face / 3) & 3;
    uint32_t sub     = face % 3;
    uint32_t tgt_zone;

    switch (mode) {
        case 0: /* ORBIT: step forward by sub+1 zones */
            tgt_zone = (zone + 1 + sub) % 12;
            break;
        case 1: /* CHIRAL: jump by 6+sub*2 zones */
            tgt_zone = (zone + 6 + sub * 2) % 12;
            break;
        case 2: /* CROSS: cross-hemisphere mirror via CROSS LUT + offset */
            { const uint8_t cross[] = {9,10,11,6,7,8,3,4,5,0,1,2};
              tgt_zone = (cross[zone % 12] + sub) % 12; }
            break;
        default: /* HUB: sub determines pole (0-2→south, rest→north) */
            tgt_zone = sub < 2 ? 0 : 6;
            break;
    }

    uint32_t new_enc = tgt_zone * fs + offset;
    return _hilbert_dec(new_enc, slots, inv37);
}

/*
 * pogls_bermuda_face_scores: computes distribution of stride-37 steps
 * across all 12 zones, normalized to [0,1].
 */
void pogls_bermuda_face_scores(uint32_t base, float scores[12])
{
    uint32_t counts[12];
    memset(counts, 0, sizeof(counts));

    uint32_t wl = _walk_len(POGLS_BERMUDA_MAX_ADDR);
    uint32_t fs = wl / 12;
    uint32_t a  = base % POGLS_BERMUDA_MAX_ADDR;

    for (uint32_t i = 0; i < POGLS_BERMUDA_N_ZONES; i++) {
        uint32_t enc  = _hilbert_enc(a, wl);
        uint32_t zone = enc / fs;
        counts[zone]++;
        a = (a + 1) % POGLS_BERMUDA_MAX_ADDR;
    }

    uint32_t total = 0;
    for (uint32_t i = 0; i < POGLS_BERMUDA_N_ZONES; i++)
        total += counts[i];

    if (total == 0) {
        for (uint32_t i = 0; i < POGLS_BERMUDA_N_ZONES; i++)
            scores[i] = 0.0f;
    } else {
        for (uint32_t i = 0; i < POGLS_BERMUDA_N_ZONES; i++)
            scores[i] = (float)counts[i] / (float)total;
    }
}

/* ═══════════════════════════════════════════════════════════════════
   DIAMOND SHELL — 3D rotation + fibo_intersect discriminator
   ═══════════════════════════════════════════════════════════════════ */

/* 3D cube rotation (4×4×4 = 64 bytes, 6 orientations) */
static void _rotate64(uint8_t out[64], const uint8_t in[64], uint8_t rot)
{
    for (uint8_t z = 0; z < 4; z++) {
        for (uint8_t y = 0; y < 4; y++) {
            for (uint8_t x = 0; x < 4; x++) {
                uint8_t sx, sy, sz;
                switch (rot % POGLS_BERMUDA_ROT_STATES) {
                    case 0: sx=x;   sy=y;   sz=z;   break;
                    case 1: sx=y;   sy=z;   sz=x;   break;
                    case 2: sx=z;   sy=x;   sz=y;   break;
                    case 3: sx=x;   sy=z;   sz=3-y; break;
                    case 4: sx=z;   sy=y;   sz=3-x; break;
                    case 5: sx=3-y; sy=x;   sz=z;   break;
                    default: sx=x; sy=y; sz=z; break;
                }
                out[z*16 + y*4 + x] = in[sz*16 + sy*4 + sx];
            }
        }
    }
}

/* Inverse rotation */
static void _inv_rotate64(uint8_t out[64], const uint8_t in[64], uint8_t rot)
{
    uint8_t tmp[64];
    memset(tmp, 0, 64);
    for (uint8_t z = 0; z < 4; z++) {
        for (uint8_t y = 0; y < 4; y++) {
            for (uint8_t x = 0; x < 4; x++) {
                uint8_t sx, sy, sz;
                switch (rot % POGLS_BERMUDA_ROT_STATES) {
                    case 0: sx=x;   sy=y;   sz=z;   break;
                    case 1: sx=y;   sy=z;   sz=x;   break;
                    case 2: sx=z;   sy=x;   sz=y;   break;
                    case 3: sx=x;   sy=z;   sz=3-y; break;
                    case 4: sx=z;   sy=y;   sz=3-x; break;
                    case 5: sx=3-y; sy=x;   sz=z;   break;
                    default: sx=x; sy=y; sz=z; break;
                }
                tmp[sz*16 + sy*4 + sx] = in[z*16 + y*4 + x];
            }
        }
    }
    memcpy(out, tmp, 64);
}

/* Build 4 rotated copies (quad mirror) for fibo_intersect */
static void _build_quad_mirror(uint64_t quad[4], const uint8_t core[8])
{
    memcpy(&quad[0], core, 8);
    uint8_t *q = (uint8_t *)quad;
    q[8]  = core[1]; q[9]  = core[2]; q[10] = core[3];
    q[11] = core[4]; q[12] = core[5]; q[13] = core[6];
    q[14] = core[7]; q[15] = core[0];
    q[16] = core[2]; q[17] = core[3]; q[18] = core[4];
    q[19] = core[5]; q[20] = core[6]; q[21] = core[7];
    q[22] = core[0]; q[23] = core[1];
    q[24] = core[3]; q[25] = core[4]; q[26] = core[5];
    q[27] = core[6]; q[28] = core[7]; q[29] = core[0];
    q[30] = core[1]; q[31] = core[2];
}

/* Fibonacci intersect: C0 & C1 & C2 & C3 */
static uint64_t _fibo_intersect(const uint8_t rotbuf[64])
{
    uint64_t quad[4];
    _build_quad_mirror(quad, rotbuf);
    return quad[0] & quad[1] & quad[2] & quad[3];
}

/* Classify chunk: pick best rotation, return encoded form */
static uint32_t _diamond_classify(const uint8_t chunk[64],
                                   uint8_t *out_flag, uint8_t *out_rot,
                                   uint8_t out_data[64])
{
    int chunk_zero = 1;
    for (int i = 0; i < 64; i++) {
        if (chunk[i]) { chunk_zero = 0; break; }
    }
    if (chunk_zero) {
        *out_flag = POGLS_BERMUDA_FLAG_FLAT;
        *out_rot  = 0;
        memset(out_data, 0, 64);
        return 2;
    }

    uint8_t  rotbuf[64];
    uint8_t  best_buf[64];
    uint8_t  best_rot = 0;
    int      best_pc  = -1;

    for (uint8_t rot = 0; rot < POGLS_BERMUDA_ROT_STATES; rot++) {
        _rotate64(rotbuf, chunk, rot);
        uint64_t isect = _fibo_intersect(rotbuf);
        int pc = (int)_popcount64(isect);
        if (pc > best_pc) {
            best_pc = pc;
            best_rot = rot;
            memcpy(best_buf, rotbuf, 64);
        }
    }

    *out_flag = POGLS_BERMUDA_FLAG_DENSE;
    *out_rot  = best_rot;
    memcpy(out_data, best_buf, 64);
    return 66; /* 2 header + 64 data */
}

/*
 * pogls_bermuda_diamond_compress: compress src_sz bytes from src.
 * Processes in 64-byte chunks. Each chunk written as [flag:1][rot:1][data:64]
 * for non-zero data, or [flag:1][rot:1] for all-zero.
 * Returns total bytes written, or 0 on dst_cap overflow.
 */
uint32_t pogls_bermuda_diamond_compress(uint8_t *dst, size_t dst_cap,
                                         const uint8_t *src, size_t src_sz)
{
    uint32_t total = 0;
    size_t   remaining = src_sz;

    while (remaining > 0) {
        uint8_t chunk[64];
        size_t this_sz = remaining < 64 ? remaining : 64;
        memcpy(chunk, src, this_sz);
        if (this_sz < 64)
            memset(chunk + this_sz, 0, 64 - this_sz);

        uint8_t flag, rot, data[64];
        uint32_t enc_sz = _diamond_classify(chunk, &flag, &rot, data);

        if (total + enc_sz > dst_cap) return 0;

        dst[total] = flag;
        dst[total+1] = rot;
        if (flag == POGLS_BERMUDA_FLAG_DENSE)
            memcpy(dst + total + 2, data, 64);
        total += enc_sz;
        src += this_sz;
        remaining -= this_sz;
    }
    return total;
}

/*
 * pogls_bermuda_diamond_decompress: reverse of compress.
 * Returns bytes written to dst, or 0 on error.
 */
uint32_t pogls_bermuda_diamond_decompress(uint8_t *dst, size_t dst_cap,
                                           const uint8_t *src, size_t src_sz)
{
    uint32_t total = 0;
    size_t   pos   = 0;

    while (pos < src_sz && total < dst_cap) {
        uint8_t flag = src[pos];
        uint8_t rot  = src[pos+1];

        if (flag == POGLS_BERMUDA_FLAG_FLAT) {
            size_t copy = (total + 64 <= dst_cap) ? 64 : dst_cap - total;
            memset(dst + total, 0, copy);
            total += (uint32_t)copy;
            pos += 2;
        } else if (flag == POGLS_BERMUDA_FLAG_DENSE) {
            if (pos + 66 > src_sz) return 0;
            uint8_t rotbuf[64];
            memcpy(rotbuf, src + pos + 2, 64);
            uint8_t chunk[64];
            _inv_rotate64(chunk, rotbuf, rot);
            size_t copy = (total + 64 <= dst_cap) ? 64 : dst_cap - total;
            memcpy(dst + total, chunk, copy);
            total += (uint32_t)copy;
            pos += 66;
        } else {
            return 0;
        }
    }
    return total;
}

/* ═══════════════════════════════════════════════════════════════════
   RLE COMPRESSION
   ═══════════════════════════════════════════════════════════════════ */

#define RLE_MIN_RUN 3

/*
 * pogls_bermuda_rle_compress: run-length encoding.
 * Format: [literal_count:2][literals...][run_count:2][value][run_length:2]...
 * Minimum run = 3 bytes.
 */
uint32_t pogls_bermuda_rle_compress(uint8_t *dst, size_t dst_cap,
                                     const uint8_t *src, size_t src_sz)
{
    size_t in_pos  = 0;
    size_t out_pos = 0;

    while (in_pos < src_sz) {
        /* Find run */
        uint8_t val = src[in_pos];
        size_t run  = 1;
        while (in_pos + run < src_sz && src[in_pos + run] == val && run < 65535)
            run++;

        if (run >= RLE_MIN_RUN) {
            /* Emit run */
            if (out_pos + 4 > dst_cap) return 0;
            dst[out_pos]     = 0; /* marker */
            dst[out_pos + 1] = 0;
            dst[out_pos + 2] = val;
            dst[out_pos + 3] = (uint8_t)(run & 0xFF);
            dst[out_pos + 4] = (uint8_t)((run >> 8) & 0xFF);
            out_pos += 5;
            in_pos += run;
        } else {
            /* Emit literal */
            size_t lit_start = in_pos;
            size_t lit_len   = 0;

            while (in_pos < src_sz) {
                val = src[in_pos];
                run = 1;
                while (in_pos + run < src_sz && src[in_pos + run] == val && run < 65535)
                    run++;

                if (run >= RLE_MIN_RUN && lit_len >= 1)
                    break;

                if (run >= RLE_MIN_RUN)
                    break;

                size_t copy = run < RLE_MIN_RUN ? run : 1;
                if (lit_len + copy > 65535) break;

                in_pos += copy;
                lit_len += copy;
            }

            if (out_pos + 2 + lit_len > dst_cap) return 0;
            dst[out_pos]     = (uint8_t)(lit_len & 0xFF);
            dst[out_pos + 1] = (uint8_t)((lit_len >> 8) & 0xFF);
            memcpy(dst + out_pos + 2, src + lit_start, lit_len);
            out_pos += 2 + lit_len;
        }
    }
    return (uint32_t)out_pos;
}

/*
 * pogls_bermuda_rle_decompress: reverse of RLE compress.
 */
uint32_t pogls_bermuda_rle_decompress(uint8_t *dst, size_t dst_cap,
                                       const uint8_t *src, size_t src_sz)
{
    size_t in_pos  = 0;
    size_t out_pos = 0;

    while (in_pos < src_sz && out_pos < dst_cap) {
        uint16_t cnt = (uint16_t)src[in_pos] | ((uint16_t)src[in_pos + 1] << 8);
        in_pos += 2;

        if (cnt == 0) {
            /* Run */
            if (in_pos + 3 > src_sz) return 0;
            uint8_t val = src[in_pos];
            uint16_t run = (uint16_t)src[in_pos + 1] | ((uint16_t)src[in_pos + 2] << 8);
            in_pos += 3;
            size_t copy = run;
            if (out_pos + copy > dst_cap) copy = dst_cap - out_pos;
            memset(dst + out_pos, val, copy);
            out_pos += copy;
        } else {
            /* Literal */
            if (in_pos + cnt > src_sz) return 0;
            size_t copy = cnt;
            if (out_pos + copy > dst_cap) copy = dst_cap - out_pos;
            memcpy(dst + out_pos, src + in_pos, copy);
            in_pos += cnt;
            out_pos += copy;
        }
    }
    return (uint32_t)out_pos;
}

/* ═══════════════════════════════════════════════════════════════════
   SHADOW BOND — FNV-1a hashed key-value ring
   ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    char     key[32];
    uint8_t  data[POGLS_BERMUDA_CHUNK_SZ];
    size_t   sz;
    uint8_t  used;
} ShadowEntry;

static ShadowEntry _shadow_ring[POGLS_BERMUDA_SHADOW_CAP];
static int         _shadow_inited = 0;

static uint64_t _fnv1a(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    while (*s) { h ^= (uint64_t)(unsigned char)*s; h *= 1099511628211ULL; s++; }
    return h;
}

static void _shadow_init(void)
{
    if (!_shadow_inited) {
        memset(_shadow_ring, 0, sizeof(_shadow_ring));
        _shadow_inited = 1;
    }
}

int pogls_bermuda_shadow_write(const char *key, const uint8_t *data, size_t sz)
{
    _shadow_init();
    uint64_t hash = _fnv1a(key);
    size_t   idx  = (size_t)(hash % POGLS_BERMUDA_SHADOW_CAP);
    size_t   copy_sz = sz < POGLS_BERMUDA_CHUNK_SZ ? sz : POGLS_BERMUDA_CHUNK_SZ;

    size_t klen = strlen(key);
    if (klen >= sizeof(_shadow_ring[0].key))
        klen = sizeof(_shadow_ring[0].key) - 1;

    memset(&_shadow_ring[idx], 0, sizeof(ShadowEntry));
    memcpy(_shadow_ring[idx].key, key, klen);
    _shadow_ring[idx].key[klen] = '\0';
    memcpy(_shadow_ring[idx].data, data, copy_sz);
    _shadow_ring[idx].sz  = copy_sz;
    _shadow_ring[idx].used = 1;
    return 1;
}

int pogls_bermuda_shadow_read(const char *key, uint8_t *data, size_t cap)
{
    _shadow_init();
    uint64_t hash = _fnv1a(key);
    size_t   idx  = (size_t)(hash % POGLS_BERMUDA_SHADOW_CAP);

    if (!_shadow_ring[idx].used)
        return 0;
    if (strcmp(_shadow_ring[idx].key, key) != 0)
        return 0;

    size_t copy_sz = _shadow_ring[idx].sz < cap ? _shadow_ring[idx].sz : cap;
    memcpy(data, _shadow_ring[idx].data, copy_sz);
    return 1;
}
