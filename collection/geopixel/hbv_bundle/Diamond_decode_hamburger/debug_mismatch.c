/*
 * debug_mismatch.c — find first mismatch in Structured pattern
 * Build: gcc -O0 -g -I. -I..\Diamond_shell_encoder -I..\..\..\..\core\pogls_engine\twin_core -o debug_mismatch.exe debug_mismatch.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "diamond_shell_codec.h"

/* copy of bench_shell_realfiles.c encode/decode */
#define SIDX_CAP  (1<<16)
#define SIDX_MASK (SIDX_CAP-1)
typedef struct { uint64_t seed; uint8_t chunk[64]; uint8_t used; } SeedSlot;
typedef struct { SeedSlot *slots; uint64_t ne, nc; } SeedIndex;

static SeedIndex* sx(void) {
    SeedIndex *x = calloc(1, sizeof(SeedIndex));
    x->slots = calloc(SIDX_CAP, sizeof(SeedSlot)); return x;
}
static void sf(SeedIndex *x) { free(x->slots); free(x); }
static void sp(SeedIndex *x, uint64_t seed, const uint8_t *ch) {
    uint32_t s0 = (uint32_t)(seed & SIDX_MASK);
    for (int i = 0; i < SIDX_CAP; i++) {
        uint32_t s = (s0 + i) & SIDX_MASK;
        if (!x->slots[s].used) {
            x->slots[s].used = 1; x->slots[s].seed = seed;
            memcpy(x->slots[s].chunk, ch, 64); x->ne++; return;
        }
        if (x->slots[s].seed == seed) return;
        x->nc++;
    }
}
static const uint8_t* sg_verify(const SeedIndex *x, uint64_t seed, const uint8_t *chunk) {
    uint32_t s0 = (uint32_t)(seed & SIDX_MASK);
    for (int i = 0; i < SIDX_CAP; i++) {
        uint32_t s = (s0 + i) & SIDX_MASK;
        if (!x->slots[s].used) return NULL;
        if (x->slots[s].seed == seed && memcmp(x->slots[s].chunk, chunk, 64) == 0)
            return x->slots[s].chunk;
    }
    return NULL;
}
static const uint8_t* sg(const SeedIndex *x, uint64_t seed) {
    uint32_t s0 = (uint32_t)(seed & SIDX_MASK);
    for (int i = 0; i < SIDX_CAP; i++) {
        uint32_t s = (s0 + i) & SIDX_MASK;
        if (!x->slots[s].used) return NULL;
        if (x->slots[s].seed == seed) return x->slots[s].chunk;
    }
    return NULL;
}

static uint64_t shell_encode_dedup(const uint8_t *data, uint64_t n_chunks,
                                    uint8_t *out, SeedIndex *idx,
                                    uint8_t *flags_out)
{
    uint64_t pos = 0;
    for (uint64_t i = 0; i < n_chunks; i++) {
        const uint8_t *chunk = data + i * SHELL_CHUNK_SZ;
        uint8_t rotbuf[64], best_buf[64];
        uint64_t best_isect = 0;
        uint8_t  best_rot   = 0;
        int      best_pc    = -1;

        for (uint8_t rot = 0; rot < SHELL_ROT_STATES; rot++) {
            _shell_rotate64(rotbuf, chunk, rot);
            DiamondBlock db = _shell_chunk_to_block(rotbuf, rot, (uint32_t)i);
            if (!fold_xor_audit(&db)) {
                db.invert = ~db.core.raw;
                fold_build_quad_mirror(&db);
            }
            uint64_t isect = fold_fibo_intersect(&db);
            int pc = __builtin_popcountll(isect);
            if (pc > best_pc) { best_pc = pc; best_isect = isect; best_rot = rot; memcpy(best_buf, rotbuf, 64); }
        }

        int is_flat = (best_pc <= 0);
        uint64_t seed = _shell_fnv64(best_buf, 64);
        const uint8_t *found = (!is_flat) ? sg_verify(idx, seed, best_buf) : NULL;

        if (is_flat) {
            out[pos++] = 0; out[pos++] = best_rot;
            flags_out[i] = 0;
        } else if (found) {
            out[pos++] = 1; out[pos++] = best_rot;
            memcpy(out + pos, &seed, 8); pos += 8;
            flags_out[i] = 1;
        } else {
            sp(idx, seed, best_buf);
            out[pos++] = 2; out[pos++] = best_rot;
            memcpy(out + pos, best_buf, 64); pos += 64;
            flags_out[i] = 2;
        }
    }
    return pos;
}

static uint64_t shell_decode_dedup(const uint8_t *in, uint64_t n_chunks,
                                    uint8_t *out, const SeedIndex *idx)
{
    uint64_t pos = 0;
    for (uint64_t i = 0; i < n_chunks; i++) {
        uint8_t flag = in[pos++];
        uint8_t rot  = in[pos++];
        uint8_t *chunk_out = out + i * SHELL_CHUNK_SZ;

        if (flag == 0) {
            memset(chunk_out, 0, SHELL_CHUNK_SZ);
        } else if (flag == 1) {
            uint64_t seed;
            memcpy(&seed, in + pos, 8); pos += 8;
            const uint8_t *found = sg(idx, seed);
            if (found) {
                _shell_inverse_rotate64(chunk_out, found, rot);
            } else {
                memset(chunk_out, 0, SHELL_CHUNK_SZ);
            }
        } else {
            uint8_t rotbuf[64];
            memcpy(rotbuf, in + pos, 64); pos += 64;
            _shell_inverse_rotate64(chunk_out, rotbuf, rot);
        }
    }
    return pos;
}

int main(void) {
    const uint64_t N = 4096;
    uint8_t *orig = malloc(N * 64);
    uint8_t *enc  = malloc(N * 66);
    uint8_t *dec  = malloc(N * 64);
    uint8_t *flags = malloc(N);

    /* generate Structured pattern (pattern 1) */
    for (size_t i = 0; i < N * 64; i++)
        orig[i] = (uint8_t)((i % 256) ^ (i >> 8));

    SeedIndex *idx = sx();
    uint64_t enc_sz = shell_encode_dedup(orig, N, enc, idx, flags);
    uint64_t dec_sz = shell_decode_dedup(enc, N, dec, idx);

    int first_fail = -1;
    for (uint64_t i = 0; i < N; i++) {
        if (memcmp(orig + i*64, dec + i*64, 64) != 0) {
            first_fail = (int)i;
            break;
        }
    }

    if (first_fail >= 0) {
        int fi = first_fail;
        printf("FIRST FAIL at chunk %d (flag=%d)\n", fi, flags[fi]);
        printf("Original  [0..15]: ");
        for (int j = 0; j < 16; j++) printf("%02x ", orig[fi*64+j]);
        printf("\nDecoded   [0..15]: ");
        for (int j = 0; j < 16; j++) printf("%02x ", dec[fi*64+j]);
        printf("\n");

        /* Re-encode this chunk manually to verify rotation */
        uint8_t rotbuf[64], best_buf[64];
        uint8_t best_rot = 0;
        int best_pc = -1;
        for (uint8_t rot = 0; rot < SHELL_ROT_STATES; rot++) {
            _shell_rotate64(rotbuf, orig + fi*64, rot);
            DiamondBlock db = _shell_chunk_to_block(rotbuf, rot, fi);
            if (!fold_xor_audit(&db)) { db.invert = ~db.core.raw; fold_build_quad_mirror(&db); }
            uint64_t isect = fold_fibo_intersect(&db);
            int pc = __builtin_popcountll(isect);
            if (pc > best_pc) { best_pc = pc; best_rot = rot; memcpy(best_buf, rotbuf, 64); }
        }

        /* What does the encode think the best_rot is for this chunk? */
        /* Re-read from encoded stream */
        uint64_t epos = 0;
        for (int ci = 0; ci < fi; ci++) {
            uint8_t f = enc[epos++]; uint8_t r = enc[epos++];
            if (f == 0) { }
            else if (f == 1) { epos += 8; }
            else { epos += 64; }
        }
        uint8_t enc_flag = enc[epos];
        uint8_t enc_rot  = enc[epos+1];
        printf("Encoded flag=%d rot=%d  Re-computed best_rot=%d best_pc=%d\n",
               enc_flag, enc_rot, best_rot, best_pc);

        /* Test: inverse_rotate on the best_buf */
        uint8_t test_dec[64];
        _shell_inverse_rotate64(test_dec, best_buf, best_rot);
        printf("Inverse direct match: %s\n",
               memcmp(orig + fi*64, test_dec, 64) == 0 ? "PASS" : "FAIL");

        /* Check: is the seed in the index with the SAME content? */
        uint64_t seed = _shell_fnv64(best_buf, 64);
        const uint8_t *idx_data = sg(idx, seed);
        if (idx_data) {
            printf("Seed 0x%llx found in idx, content match: %s\n",
                   (unsigned long long)seed,
                   memcmp(idx_data, best_buf, 64) == 0 ? "PASS" : "CONTENT MISMATCH!");
            /* If content mismatch, print the stored data */
            if (memcmp(idx_data, best_buf, 64) != 0) {
                printf("Idx data   [0..15]: ");
                for (int j = 0; j < 16; j++) printf("%02x ", idx_data[j]);
                printf("\nBest buf   [0..15]: ");
                for (int j = 0; j < 16; j++) printf("%02x ", best_buf[j]);
                printf("\n");
            }
        } else {
            printf("Seed 0x%llx NOT found in idx\n", (unsigned long long)seed);
        }
    } else {
        printf("All %llu chunks PASS\n", (unsigned long long)N);
    }

    sf(idx); free(orig); free(enc); free(dec); free(flags);
    return 0;
}
