/*
 * diamond_geo_pipe_v3.c
 * ══════════════════════
 * Pipeline — flat binary, no BMP/RGB/color:
 *
 * encode: binary → diamond(all chunks) → direct Hilbert shell traversal → flat file
 * decode: flat file → onion header → same Hilbert traversal → diamond → binary
 *
 * Container:
 *   [onion_header:64B]       — seed, face_pairs, shell_count, xxh64
 *   [n_chunks:4B]
 *   [chunk_stream...]        — diamond bytes in Hilbert order,
 *                               each prefixed by [sz:4B]
 *
 * Virtual summoning:
 *   Shell N has 6N+1 cells, but NONE are stored.
 *   onion_chunk_at(shell,cell) = chunk_idx — deterministic O(1) from seed.
 *   same traversal on encode & decode → no order array, no slot index.
 *
 * Face direction:
 *   hex_ring_enum(N) starts at (N,0) → sector 0 = frustum face 0
 *   6 sectors : 6 frustum faces.  Face pair: s ↔ (s+3)%6.
 *   Hilbert fold per-sector with seed→flip, deterministic both sides.
 *
 * Compile:
 *   gcc -O2 -I. -Inew_diamond_tring -Icore diamond_geo_pipe_v3.c -lm -o diamond_geo_pipe_v3
 *
 * Usage:
 *   ./diamond_geo_pipe_v3 encode    <in.bin>  [out.dat]
 *   ./diamond_geo_pipe_v3 decode    <in.dat>  [out.bin]
 *   ./diamond_geo_pipe_v3 roundtrip <in.bin>
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

#define CHUNK_SZ    64
#define HDR_SZ      64

/* ── xxh64 ─────────────────────────────────────────────────────── */
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

/* ══════════════════════════════════════════════════════════════════
 * SINGLE TRAVERSAL — shared by both encode and decode
 * Iterates (shell,cell) in Hilbert order, yields chunk_idx.
 * No order array — the traversal IS the index.
 * ══════════════════════════════════════════════════════════════════ */
/* Callback: return 0 to continue, non-zero to stop */
typedef int (*traverse_fn)(void *ctx, uint32_t chunk_idx, uint32_t seq_pos);

static int traverse_hilbert(OnionShell *os, int n_chunks,
                            traverse_fn fn, void *ctx){
    uint8_t *written = calloc(n_chunks, 1);
    if (!written) return -1;
    int pos = 0;
    uint8_t sc = os->hdr.fpt.shell_count;

    /* shell 1..shell_count in Hilbert order */
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
    /* remaining chunks not covered by shells */
    for (int i = 0; i < n_chunks && pos < n_chunks; i++) {
        if (!written[i]) {
            if (fn(ctx, (uint32_t)i, (uint32_t)pos)) { free(written); return 1; }
            pos++;
        }
    }
    free(written);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * ENCODE — binary → diamond → Hilbert traversal → flat container
 * ══════════════════════════════════════════════════════════════════ */
typedef struct { FILE *f; uint8_t **enc; uint32_t *esz; } WriteCtx;

static int write_chunk(void *ctx, uint32_t ci, uint32_t seq_pos){
    (void)seq_pos;
    WriteCtx *w = (WriteCtx*)ctx;
    fwrite(&w->esz[ci], 4, 1, w->f);
    fwrite(w->enc[ci], 1, w->esz[ci], w->f);
    return 0;
}

static int do_encode(const uint8_t *data, size_t orig_size,
                     const char *out_path){
    int n_chunks = (int)((orig_size + CHUNK_SZ - 1) / CHUNK_SZ);
    uint64_t digest = xxh64(data, orig_size);

    /* 1. diamond encode all chunks */
    DiamondField df;
    dfield_init(&df, (uint32_t)(n_chunks + 64));

    uint8_t **enc = malloc(n_chunks * sizeof(uint8_t*));
    uint32_t *esz = malloc(n_chunks * sizeof(uint32_t));

    clock_t t0 = clock();

    for (int i = 0; i < n_chunks; i++) {
        uint8_t chunk[CHUNK_SZ] = {0};
        size_t src = (size_t)i * CHUNK_SZ;
        size_t cp = orig_size - src;
        if (cp > CHUNK_SZ) cp = CHUNK_SZ;
        memcpy(chunk, data + src, cp);

        uint32_t tick = dfield_encode_flat(&df, chunk);
        uint32_t sz; const uint8_t *p = tring_read(&df.tring, tick, &sz);
        enc[i] = malloc(sz); memcpy(enc[i], p, sz); esz[i] = sz;
    }

    clock_t t1 = clock();

    /* 2. init OnionShell */
    OnionShell os;
    onion_init(&os, (uint32_t)n_chunks, digest, 4);

    clock_t t2 = clock();

    /* 3. write header + chunks via direct Hilbert traversal */
    FILE *f = fopen(out_path, "wb");
    if (!f) { perror(out_path); return 1; }

    uint8_t hdr[HDR_SZ];
    onion_header_write(&os, hdr);
    memcpy(hdr + 36, &orig_size, 4);
    memcpy(hdr + 40, &digest, 8);
    memcpy(hdr + 48, &n_chunks, 4);
    fwrite(hdr, 1, HDR_SZ, f);
    fwrite(&n_chunks, 4, 1, f);

    uint32_t total_enc = 0;
    WriteCtx wctx = {f, enc, esz};
    traverse_hilbert(&os, n_chunks, write_chunk, &wctx);

    for (int i = 0; i < n_chunks; i++) total_enc += esz[i];
    fclose(f);

    clock_t t3 = clock();

    printf("diamond  orig=%zuB chunks=%d enc=%uB ratio=%.2fx [%.1fms]\n",
           orig_size, n_chunks, total_enc,
           (double)(n_chunks * CHUNK_SZ) / total_enc,
           1000.0*(t1-t0)/CLOCKS_PER_SEC);
    printf("onion    seed=%016llX shells=%d [%.1fms]\n",
           (unsigned long long)digest, 4,
           1000.0*(t2-t1)/CLOCKS_PER_SEC);
    printf("write    %s total=%.1fms\n", out_path,
           1000.0*(t3-t2)/CLOCKS_PER_SEC);

    for (int i = 0; i < n_chunks; i++) free(enc[i]);
    free(enc); free(esz);
    dfield_free(&df); onion_free(&os);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * DECODE — flat container → same Hilbert traversal → diamond → binary
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    FILE *f; DiamondField *df; uint8_t *out;
    int n_chunks; int ok; uint32_t *read_sz;
} ReadCtx;

static int read_chunk(void *ctx, uint32_t ci, uint32_t seq_pos){
    (void)seq_pos;
    ReadCtx *r = (ReadCtx*)ctx;
    if (seq_pos >= (uint32_t)r->n_chunks) return 1;

    uint32_t sz;
    if (fread(&sz, 4, 1, r->f) != 1) return 1;

    uint8_t *buf = malloc(sz);
    if (!buf || fread(buf, 1, sz, r->f) != sz) { free(buf); return 1; }

    uint32_t tick = tring_push(&r->df->tring, buf, sz);
    uint8_t decoded[CHUNK_SZ];
    if (dfield_decode_flat(r->df, tick, decoded) == 0) {
        memcpy(r->out + (size_t)ci * CHUNK_SZ, decoded, CHUNK_SZ);
        r->ok++;
    }
    r->read_sz[seq_pos] = sz;
    free(buf);
    return 0;
}

static int do_decode(const char *in_path, const char *out_path){
    FILE *f = fopen(in_path, "rb");
    if (!f) { perror(in_path); return 1; }

    /* 1. onion shell header */
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

    /* 2. verify n_chunks */
    int file_nc;
    if (fread(&file_nc, 4, 1, f) != 1 || file_nc != n_chunks) {
        fprintf(stderr, "chunk count mismatch\n"); fclose(f); return 1;
    }

    clock_t t0 = clock();

    /* 3. read & decode via same Hilbert traversal */
    DiamondField df;
    dfield_init(&df, (uint32_t)(n_chunks + 64));

    uint8_t *out = calloc(1, (size_t)n_chunks * CHUNK_SZ);
    uint32_t *read_sz = calloc(n_chunks, sizeof(uint32_t));

    ReadCtx rctx = {f, &df, out, n_chunks, 0, read_sz};
    traverse_hilbert(&os, n_chunks, read_chunk, &rctx);

    fclose(f);

    clock_t t1 = clock();

    /* 4. verify */
    uint64_t got = xxh64(out, orig_size);
    if (got != stored_dig) {
        fprintf(stderr, "FAIL xxh64 got=%016llX stored=%016llX\n",
                (unsigned long long)got, (unsigned long long)stored_dig);
        free(out); free(read_sz); dfield_free(&df); onion_free(&os);
        return 2;
    }

    FILE *g = fopen(out_path, "wb");
    if (!g) { perror(out_path); free(out); free(read_sz); dfield_free(&df); onion_free(&os); return 1; }
    fwrite(out, 1, orig_size, g);
    fclose(g);

    printf("decode   ok=%d/%d orig=%zuB xxh64 PASS %016llX [%.1fms]\n",
           rctx.ok, n_chunks, orig_size, (unsigned long long)got,
           1000.0*(t1-t0)/CLOCKS_PER_SEC);

    free(out); free(read_sz); dfield_free(&df); onion_free(&os);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════════════════ */
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
        char def[512]; snprintf(def, sizeof(def), "%s.dat", src);
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
        snprintf(dat, sizeof(dat), "%s.dat", src);
        snprintf(out, sizeof(out), "%s.rt.bin", src);

        printf("=== Roundtrip v3 (direct Hilbert traversal): %s ===\n", src);

        FILE *f = fopen(src, "rb");
        if (!f) { perror(src); return 1; }
        fseek(f, 0, SEEK_END); size_t sz = ftell(f); rewind(f);
        uint8_t *data = malloc(sz);
        if (fread(data, 1, sz, f) != sz) { free(data); fclose(f); return 1; }
        fclose(f);

        printf("[1] binary → diamond + onion → .dat\n");
        if (do_encode(data, sz, dat)) { free(data); return 1; }

        printf("[2] .dat → onion + diamond → binary\n");
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
