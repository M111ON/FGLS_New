/*
 * diamond_geo_pipe_v4.c — Smart multi-strategy + batch dedup
 *
 * Pipeline using geo_smart_encode.h for per-chunk strategy selection:
 *   FLAT (0)   → 1B    (all zeros)
 *   RAW (1)    → 65B   (uncompressible)
 *   DIAMOND (2) → ~53B average (dfield_encode_flat)
 *   BATCH (3)  → 5B    (duplicate of earlier chunk)
 *
 * Container:
 *   [onion_header:64B]  — seed, face_pairs, shell_count, xxh64
 *   [n_chunks:4B]
 *   [strategy_byte + payload...] per chunk in Hilbert order
 *
 * Compile:
 *   gcc -O2 -I. -Inew_diamond_tring -Icore diamond_geo_pipe_v4.c -lm -o diamond_geo_pipe_v4
 *
 * Usage:
 *   diamond_geo_pipe_v4 encode    <in.bin>  [out.dat]
 *   diamond_geo_pipe_v4 decode    <in.dat>  [out.bin]
 *   diamond_geo_pipe_v4 roundtrip <in.bin>
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tring.h"
#include "pogls_fold.h"
#include "geo_diamond_field_v4.h"
#include "geo_onion_shell.h"
#include "geo_smart_encode.h"

#define CHUNK_SZ    64
#define HDR_SZ      64

/* ── xxh64 (for file-level checksum) ─────────────────────────── */
#define _H1 0x9e3779b97f4a7c15ULL
#define _H2 0x6c62272e07bb0142ULL
static inline uint64_t _rot(uint64_t x,int r){return(x<<r)|(x>>(64-r));}
static inline uint64_t _hu(uint64_t a,uint64_t w){
    a^=(w*_H1);a=_rot(a,27);a=a*_H2+0x94d049bb133111ebULL;return a;}
static uint64_t xxh64(const uint8_t*d,size_t n){
    uint64_t a=_H1^n; size_t i=0;
    for(;i+8<=n;i+=8){uint64_t w;memcpy(&w,d+i,8);a=_hu(a,w);}
    if(i<n){uint64_t t=0;memcpy(&t,d+i,n-i);a=_hu(a,t);}
    a^=(a>>33);a*=_H1;a^=(a>>29);a*=_H2;a^=(a>>32);return a;}

/* ══════════════════════════════════════════════════════════════
 * TRAVERSAL — shared by encode + decode
 * ══════════════════════════════════════════════════════════════ */
typedef int (*traverse_fn)(void *ctx, uint32_t chunk_idx, uint32_t seq_pos);

static int traverse_hilbert(OnionShell *os, int n_chunks,
                            traverse_fn fn, void *ctx){
    uint8_t *written = calloc(n_chunks, 1);
    if (!written) return -1;
    int pos = 0;
    uint8_t sc = os->hdr.fpt.shell_count;

    for (uint32_t sh = 1; sh <= (uint32_t)sc && pos < n_chunks; sh++) {
        uint32_t cells = 6 * sh;
        for (uint32_t c = 0; c < cells && pos < n_chunks; c++) {
            uint32_t ci = onion_chunk_at(os, sh, c);
            if (ci < (uint32_t)n_chunks && !written[ci]) {
                written[ci] = 1;
                if (fn(ctx, ci, (uint32_t)pos)) { free(written); return 1; }
                pos++;
            }
        }
    }
    for (int i = 0; i < n_chunks && pos < n_chunks; i++) {
        if (!written[i]) {
            if (fn(ctx, (uint32_t)i, (uint32_t)pos)) { free(written); return 1; }
            pos++;
        }
    }
    free(written);
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 * ENCODE — smart select per chunk during Hilbert traversal
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    FILE        *f;
    DiamondField *df;
    SmartEncCtx  *sec;
    uint8_t      *data;      /* original file data */
    size_t        orig_size;
    int           n_chunks;
    uint32_t      written;   /* bytes written counter */
} EncTraverseCtx;

static int enc_traverse(void *ctx, uint32_t ci, uint32_t seq_pos){
    EncTraverseCtx *e = (EncTraverseCtx*)ctx;

    uint8_t chunk[CHUNK_SZ] = {0};
    size_t src = (size_t)ci * CHUNK_SZ;
    size_t cp = e->orig_size - src;
    if (cp > CHUNK_SZ) cp = CHUNK_SZ;
    memcpy(chunk, e->data + src, cp);

    e->sec->seq_pos = seq_pos;
    uint32_t esz;
    const uint8_t *enc = smart_encode(e->sec, chunk, &esz);

    fwrite(enc, 1, esz, e->f);
    e->written += esz;
    return 0;
}

static int do_encode(const uint8_t *data, size_t orig_size,
                     const char *out_path){
    int n_chunks = (int)((orig_size + CHUNK_SZ - 1) / CHUNK_SZ);
    uint64_t digest = xxh64(data, orig_size);

    DiamondField df;
    dfield_init(&df, (uint32_t)(n_chunks + 64));

    SmartEncCtx sec;
    smart_enc_init(&sec, &df, (uint32_t)n_chunks);

    OnionShell os;
    onion_init(&os, (uint32_t)n_chunks, digest, 4);

    clock_t t0 = clock();

    /* write header */
    FILE *f = fopen(out_path, "wb");
    if (!f) { perror(out_path); smart_enc_free(&sec); dfield_free(&df); onion_free(&os); return 1; }

    uint8_t hdr[HDR_SZ];
    onion_header_write(&os, hdr);
    memcpy(hdr + 36, &orig_size, 4);
    memcpy(hdr + 40, &digest, 8);
    memcpy(hdr + 48, &n_chunks, 4);
    fwrite(hdr, 1, HDR_SZ, f);
    fwrite(&n_chunks, 4, 1, f);

    /* encode + write via Hilbert traversal */
    EncTraverseCtx ectx = {f, &df, &sec, (uint8_t*)data, orig_size, n_chunks, 0};
    traverse_hilbert(&os, n_chunks, enc_traverse, &ectx);

    fclose(f);
    clock_t t1 = clock();

    /* count strategy breakdown */
    uint32_t cnt[5] = {0,0,0,0,0};
    size_t   bytes[5] = {0,0,0,0,0};
    for (int i = 0; i < n_chunks; i++) {
        uint8_t s = sec.stored_enc[i][0];
        if (s < 5) { cnt[s]++; bytes[s] += sec.stored_sz[i]; }
    }
    double ratio = (double)(n_chunks * CHUNK_SZ) / ectx.written;

    printf("smart    orig=%zuB chunks=%d enc=%uB ratio=%.2fx [%.1fms]\n",
           orig_size, n_chunks, ectx.written, ratio,
           1000.0*(t1-t0)/CLOCKS_PER_SEC);
    printf("  FLAT=%u(%zuB) RAW=%u(%zuB) DIAMOND=%u(%zuB) BATCH=%u(%zuB) GEOM=%u(%zuB)\n",
           cnt[0], bytes[0], cnt[1], bytes[1],
           cnt[2], bytes[2], cnt[3], bytes[3],
           cnt[4], bytes[4]);

    smart_enc_free(&sec);
    dfield_free(&df);
    onion_free(&os);
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 * DECODE — Hilbert traversal + smart decode dispatch
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    FILE         *f;
    DiamondField *df;
    SmartDecCtx  *sdc;
    uint8_t      *out;          /* output buffer [n_chunks * 64], chunk_idx indexed */
    uint8_t      *batch_refs;   /* batch reference buffer [n_chunks * 64], seq_pos indexed */
    int           n_chunks;
    int           ok;
} DecTraverseCtx;

static int dec_traverse(void *ctx, uint32_t ci, uint32_t seq_pos){
    DecTraverseCtx *d = (DecTraverseCtx*)ctx;
    if (seq_pos >= (uint32_t)d->n_chunks) return 1;

    uint8_t tag;
    if (fread(&tag, 1, 1, d->f) != 1) return 1;

    uint8_t decoded[64];
    d->sdc->seq_pos = seq_pos;

    if (tag == 0) {
        memset(decoded, 0, 64);
    } else if (tag == 1) {
        if (fread(decoded, 1, 64, d->f) != 64) return 1;
    } else if (tag == 2) {
        /* DIAMOND: read 4B size, then diamond bytes */
        uint32_t dsz;
        if (fread(&dsz, 4, 1, d->f) != 1 || dsz < 1 || dsz > 64) return 1;
        uint8_t dbuf[64];
        if (fread(dbuf, 1, dsz, d->f) != dsz) return 1;
        uint32_t tick = tring_push(&d->df->tring, dbuf, dsz);
        if (dfield_decode_flat(d->df, tick, decoded)) return 1;
    } else if (tag == 3) {
        uint32_t ref;
        if (fread(&ref, 4, 1, d->f) != 1) return 1;
        if (ref >= (uint32_t)d->n_chunks) return 1;
        memcpy(decoded, d->batch_refs + (size_t)ref * 64, 64);
    } else if (tag == 4) {
        /* GEOMETRIC duplicate: [tag][ref_seq_pos:4] = 5B */
        uint32_t ref;
        if (fread(&ref, 4, 1, d->f) != 1) return 1;
        if (ref >= (uint32_t)d->n_chunks) return 1;
        memcpy(decoded, d->batch_refs + (size_t)ref * 64, 64);
    } else {
        return 1;
    }

    memcpy(d->out + (size_t)ci * 64, decoded, 64);
    memcpy(d->batch_refs + (size_t)seq_pos * 64, decoded, 64);
    d->ok++;
    return 0;
}

static int do_decode(const char *in_path, const char *out_path){
    FILE *f = fopen(in_path, "rb");
    if (!f) { perror(in_path); return 1; }

    uint8_t hdr[HDR_SZ];
    if (fread(hdr, 1, HDR_SZ, f) != HDR_SZ) { fclose(f); return 1; }

    OnionShell os;
    memset(&os, 0, sizeof(os));
    if (onion_header_read(&os, hdr) != 0) {
        fprintf(stderr, "shell header corrupt\n"); fclose(f); return 1;
    }

    size_t orig_size = 0; memcpy(&orig_size, hdr + 36, 4);
    uint64_t stored_dig = 0; memcpy(&stored_dig, hdr + 40, 8);
    int n_chunks = 0; memcpy(&n_chunks, hdr + 48, 4);

    os.chunk_seq = malloc((size_t)n_chunks * sizeof(uint32_t));
    os.n_chunks = (uint32_t)n_chunks;
    for (int i = 0; i < n_chunks; i++) os.chunk_seq[i] = (uint32_t)i;

    int file_nc;
    if (fread(&file_nc, 4, 1, f) != 1 || file_nc != n_chunks) {
        fprintf(stderr, "chunk count mismatch\n"); fclose(f); return 1;
    }

    clock_t t0 = clock();

    DiamondField df;
    dfield_init(&df, (uint32_t)(n_chunks + 64));

    SmartDecCtx sdc;
    smart_dec_init(&sdc, &df);

    uint8_t *out = calloc((size_t)n_chunks * 64, 1);
    uint8_t *batch_refs = calloc((size_t)n_chunks * 64, 1);

    DecTraverseCtx dctx = {f, &df, &sdc, out, batch_refs, n_chunks, 0};
    traverse_hilbert(&os, n_chunks, dec_traverse, &dctx);

    fclose(f);
    clock_t t1 = clock();

    /* verify */
    uint64_t got = xxh64(out, orig_size);
    if (got != stored_dig) {
        fprintf(stderr, "FAIL xxh64 got=%016llX stored=%016llX\n",
                (unsigned long long)got, (unsigned long long)stored_dig);
        free(out); free(batch_refs);
        smart_dec_free(&sdc); dfield_free(&df); onion_free(&os);
        return 2;
    }

    FILE *g = fopen(out_path, "wb");
    if (!g) { perror(out_path); free(out); free(batch_refs); smart_dec_free(&sdc); dfield_free(&df); onion_free(&os); return 1; }
    fwrite(out, 1, orig_size, g);
    fclose(g);

    printf("decode   ok=%d/%d orig=%zuB xxh64 PASS [%.1fms]\n",
           dctx.ok, n_chunks, orig_size,
           1000.0*(t1-t0)/CLOCKS_PER_SEC);

    free(out); free(batch_refs);
    smart_dec_free(&sdc);
    dfield_free(&df);
    onion_free(&os);
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════════════ */
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
            "Usage:\n"
            "  %s encode    <in.bin>  [out.dat]\n"
            "  %s decode    <in.dat>  [out.bin]\n"
            "  %s roundtrip <in.bin>\n",
            argv[0], argv[0], argv[0]);
        return 1;
    }
    const char *cmd = argv[1], *src = argv[2];

    if (strcmp(cmd, "encode") == 0) {
        char def[512]; snprintf(def, sizeof(def), "%s.v4", src);
        const char *dst = (argc >= 4) ? argv[3] : def;
        FILE *f = fopen(src, "rb");
        if (!f) { perror(src); return 1; }
        fseek(f, 0, SEEK_END); size_t sz = ftell(f); rewind(f);
        uint8_t *data = malloc(sz);
        if (fread(data, 1, sz, f) != sz) { free(data); fclose(f); return 1; }
        fclose(f);
        int r = do_encode(data, sz, dst);
        free(data);
        return r;
    }

    if (strcmp(cmd, "decode") == 0) {
        char def[512]; snprintf(def, sizeof(def), "%s.out", src);
        const char *dst = (argc >= 4) ? argv[3] : def;
        return do_decode(src, dst);
    }

    if (strcmp(cmd, "roundtrip") == 0) {
        char dat[512], out[512];
        snprintf(dat, sizeof(dat), "%s.v4", src);
        snprintf(out, sizeof(out), "%s.rt.bin", src);

        printf("=== Roundtrip v4 (smart select): %s ===\n", src);

        FILE *f = fopen(src, "rb");
        if (!f) { perror(src); return 1; }
        fseek(f, 0, SEEK_END); size_t sz = ftell(f); rewind(f);
        uint8_t *data = malloc(sz);
        if (fread(data, 1, sz, f) != sz) { free(data); fclose(f); return 1; }
        fclose(f);

        printf("[1] binary → smart encode + onion → .v4\n");
        if (do_encode(data, sz, dat)) { free(data); return 1; }

        printf("[2] .v4 → onion + smart decode → binary\n");
        int r = do_decode(dat, out);

        if (r == 0) {
            FILE *g = fopen(out, "rb");
            if (!g) { free(data); return 1; }
            fseek(g, 0, SEEK_END); size_t sz2 = ftell(g); rewind(g);
            uint8_t *d2 = malloc(sz2);
            if (fread(d2, 1, sz2, g) != sz2) { free(d2); fclose(g); free(data); return 3; }
            fclose(g);

            int match = (sz == sz2 && memcmp(data, d2, sz) == 0);
            printf("\n%s  %zuB in → %zuB out\n",
                   match ? "✓ ROUNDTRIP PASS" : "✗ ROUNDTRIP FAIL", sz, sz2);
            free(d2);
        }
        free(data);
        return r;
    }

    fprintf(stderr, "unknown cmd: %s\n", cmd);
    return 1;
}
