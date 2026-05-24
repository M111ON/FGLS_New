/*
 * diamond_geo_pipe_v7.c — Skeleton Field Index + flow-zone pipeline
 *
 * Adds skeleton_index.h on top of v6:
 *   - skeleton_lookup(addr) → zone/pair/pole/partner/enc (O(1), 7 ops)
 *   - skel_decide() replaces ts_encode strategy selection
 *     P0 IDENTITY  diff==0           1B
 *     P1 RAW       isect_pop>=16     64B  (residual zone fast-reject)
 *     P2 FLAT      all-zero          1B
 *     P3 DIFF      1..48 bytes diff  10+nB
 *     P4 BREF      byte-reverse      2B
 *     P5 GEOM      isect_pop<16      ~20B
 *   - zone reset tied to skeleton zone boundary (pair change)
 *   - DIFF ceiling raised 32→48 (break-even at 53)
 *
 * Container (unchanged from v6):
 *   [onion_header:64B]
 *   [n_zones:4B]
 *   [zone_record:n_zones * 8B]
 *   [n_total_chunks:4B]
 *   [strategy_byte + payload...] per chunk
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
#include "geo_transform_seq.h"
#include "geo_flow_chunker.h"
#include "skeleton_index.h"

#define CHUNK_SZ    64
#define HDR_SZ      64

/* ── xxh64 ──────────────────────────────────────────────────── */
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

/* ── Zone descriptor ────────────────────────────────────────── */
typedef struct {
    uint32_t chunk_offset;  /* first chunk index (global) */
    uint32_t chunk_count;   /* number of 64B chunks in zone */
} ZoneRecord;

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
 * ENCODE — v7: skeleton-driven decision
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    FILE         *f;
    uint8_t      *data;
    size_t        orig_size;
    int           n_chunks;
    uint32_t      written;
    TsEncCtx     *ts_ctx;
    DiamondField *df;
    int           cur_zone;
    int           n_zones;
    ZoneRecord   *zones;
    uint32_t      zone_first_seq;
    /* v7 additions */
    SkelEncCtx    skel;
    uint8_t       last_pair;
} EncTraverseCtx;

static int enc_traverse(void *ctx, uint32_t ci, uint32_t seq_pos){
    EncTraverseCtx *e = (EncTraverseCtx*)ctx;

    /* Skeleton lookup — O(1), 7 ops */
    uint64_t addr = (uint64_t)ci * CHUNK_SZ;
    SkeletonIdx sk = skeleton_lookup(addr);

    /* Flow zone boundary check */
    for (int z = 0; z < e->n_zones; z++) {
        if (ci >= e->zones[z].chunk_offset &&
            ci < e->zones[z].chunk_offset + e->zones[z].chunk_count &&
            z != e->cur_zone) {
            e->cur_zone = z;
            e->zone_first_seq = seq_pos;
            memset(e->ts_ctx[z].prev_chunk, 0, 64);
            e->ts_ctx[z].has_prev = 0;
            skel_enc_zone_reset(&e->skel);
            break;
        }
    }
    /* Skeleton pair boundary → additional zone reset */
    if (e->skel.has_prev && sk.pair != e->last_pair)
        skel_enc_zone_reset(&e->skel);
    e->last_pair = sk.pair;

    /* Fill chunk */
    uint8_t chunk[CHUNK_SZ] = {0};
    size_t src = (size_t)ci * CHUNK_SZ;
    size_t cp = e->orig_size - src;
    if (cp > CHUNK_SZ) cp = CHUNK_SZ;
    memcpy(chunk, e->data + src, cp);

    /* v7: skeleton decision P0→P5 */
    SkelStrategy ss = skel_decide(chunk, e->skel.prev, e->skel.has_prev);
    uint8_t enc_buf[80]; uint32_t esz = 0;

    switch (ss) {
    case SKEL_S_IDENTITY:
        enc_buf[0] = 5; esz = 1; break;
    case SKEL_S_FLAT:
        enc_buf[0] = 0; esz = 1; break;
    case SKEL_S_BREF:
        enc_buf[0] = 7; esz = 1; break;
    case SKEL_S_DIFF: {
        uint64_t mask = 0; uint8_t dvals[64]; int dc = 0;
        for (int i = 0; i < CHUNK_SZ; i++)
            if (chunk[i] != e->skel.prev[i]) { mask |= (1ULL<<i); dvals[dc++] = chunk[i]; }
        enc_buf[0] = 9; enc_buf[1] = (uint8_t)dc;
        memcpy(enc_buf+2, &mask, 8);
        memcpy(enc_buf+10, dvals, (size_t)dc);
        esz = 10 + (uint32_t)dc;
        break;
    }
    default: {
        /* GEOM / RAW: delegate to ts_encode */
        TsEncCtx *ts = &e->ts_ctx[e->cur_zone >= 0 ? e->cur_zone : 0];
        const uint8_t *tenc = ts_encode(ts, chunk, &esz, seq_pos);
        fwrite(tenc, 1, esz, e->f);
        e->written += esz;
        memcpy(e->skel.prev, chunk, CHUNK_SZ);
        e->skel.has_prev = 1; e->skel.chunk_count++;
        return 0;
    }
    }

    fwrite(enc_buf, 1, esz, e->f);
    e->written += esz;
    memcpy(e->skel.prev, chunk, CHUNK_SZ);
    e->skel.has_prev = 1;
    e->skel.hits[ss]++;
    e->skel.chunk_count++;
    /* Sync ts_ctx prev after skel-handled chunk to avoid stale ref */
    if (e->cur_zone >= 0) {
        memcpy(e->ts_ctx[e->cur_zone].prev_chunk, chunk, CHUNK_SZ);
        e->ts_ctx[e->cur_zone].has_prev = 1;
    }
    return 0;
}

static int do_encode(const uint8_t *data, size_t orig_size,
                     const char *out_path){
    int n_chunks = (int)((orig_size + CHUNK_SZ - 1) / CHUNK_SZ);
    uint64_t digest = xxh64(data, orig_size);

    /* Flow chunk the data → find zones */
    FlowSegment *segs = NULL;
    int n_segs = flow_chunk(data, orig_size, &segs, 0, 0, 4096);
    if (n_segs <= 0) { fprintf(stderr, "flow_chunk failed\n"); return 1; }

    /* Build zones: merge consecutive flow segments into zones of 64B-aligned chunks */
    ZoneRecord *zones = malloc((size_t)n_segs * sizeof(ZoneRecord));
    int n_zones = 0;
    int prev_chunk_end = 0;
    int zone_start_chunk = 0;

    for (int i = 0; i < n_segs; i++) {
        uint32_t seg_end_chunk = (segs[i].offset + segs[i].length + CHUNK_SZ - 1) / CHUNK_SZ;
        int seg_chunk_count = (int)(seg_end_chunk) - (int)(segs[i].offset / CHUNK_SZ);
        if (seg_chunk_count < 1) seg_chunk_count = 1;

        if (i == 0) {
            zone_start_chunk = (int)(segs[i].offset / CHUNK_SZ);
        }

        /* New zone at boundary */
        zones[n_zones].chunk_offset = (uint32_t)(segs[i].offset / CHUNK_SZ);
        zones[n_zones].chunk_count = (uint32_t)seg_chunk_count;
        n_zones++;
    }

    /* Deduplicate overlapping zone starts (merge adjacent) */
    {
        int wi = 0;
        for (int ri = 1; ri < n_zones; ri++) {
            uint32_t prev_end = zones[wi].chunk_offset + zones[wi].chunk_count;
            if (zones[ri].chunk_offset <= prev_end) {
                /* Merge: extend previous zone */
                uint32_t new_end = zones[ri].chunk_offset + zones[ri].chunk_count;
                if (new_end > prev_end)
                    zones[wi].chunk_count = new_end - zones[wi].chunk_offset;
            } else {
                wi++;
                zones[wi] = zones[ri];
            }
        }
        n_zones = wi + 1;
    }

    /* Allocate per-zone TsEncCtx (share same DiamondField) */
    DiamondField df;
    dfield_init(&df, (uint32_t)(n_chunks + 64));

    TsEncCtx *ts_ctx = calloc((size_t)n_zones, sizeof(TsEncCtx));
    for (int z = 0; z < n_zones; z++) {
        ts_enc_init(&ts_ctx[z], &df, (uint32_t)n_chunks);
        ts_ctx[z].has_prev = 0;
    }

    OnionShell os;
    onion_init(&os, (uint32_t)n_chunks, digest, 4);

    clock_t t0 = clock();

    FILE *f = fopen(out_path, "wb");
    if (!f) { perror(out_path); goto cleanup; }

    uint8_t hdr[HDR_SZ];
    onion_header_write(&os, hdr);
    memcpy(hdr + 36, &orig_size, 4);
    memcpy(hdr + 40, &digest, 8);
    memcpy(hdr + 48, &n_chunks, 4);
    fwrite(hdr, 1, HDR_SZ, f);

    /* Write zone index */
    fwrite(&n_zones, 4, 1, f);
    fwrite(zones, sizeof(ZoneRecord), (size_t)n_zones, f);

    /* Write total chunks */
    fwrite(&n_chunks, 4, 1, f);

    /* Encode */
    EncTraverseCtx ectx = {f, (uint8_t*)data, orig_size, n_chunks, 0,
                           ts_ctx, &df, 0, n_zones, zones, 0};
    skel_enc_init(&ectx.skel);
    ectx.last_pair = 0xFF;  /* force first-chunk zone init */
    traverse_hilbert(&os, n_chunks, enc_traverse, &ectx);

    fclose(f);
    clock_t t1 = clock();

    /* Strategy breakdown — guard NULL (skeleton chunks bypass ts_ctx) */
    uint32_t cnt[10] = {0};
    size_t   bytes[10] = {0};
    for (int i = 0; i < n_chunks; i++) {
        for (int z = 0; z < n_zones; z++) {
            if (ts_ctx[z].base.stored_enc && ts_ctx[z].base.stored_enc[i]) {
                uint8_t s = ts_ctx[z].base.stored_enc[i][0];
                if (s < 10) cnt[s]++;
                break;
            }
        }
    }
    /* Re-count bytes from all zones */
    memset(bytes, 0, sizeof(bytes));
    for (int z = 0; z < n_zones; z++) {
        for (uint32_t i = 0; i < (uint32_t)n_chunks; i++) {
            uint8_t *enc = ts_ctx[z].base.stored_enc[i];
            if (enc) {
                uint8_t s = enc[0];
                if (s < 10) bytes[s] += ts_ctx[z].base.stored_sz[i];
            }
        }
    }

    double ratio = (double)(n_chunks * CHUNK_SZ) / ectx.written;
    const char *names[10] = {"FLAT","RAW","DIAMOND","BATCH","GEOM",
                             "IDENTITY","BROT","BREF","D4","DIFF"};
    /* v7 skeleton hit summary */
    const char *skelnames[6] = {"SK_ID","SK_FLAT","SK_DIFF","SK_BREF","SK_GEOM","SK_RAW"};
    printf("  [skel] ");
    for (int i=0;i<6;i++) if(ectx.skel.hits[i]) printf("%s=%u ",skelnames[i],ectx.skel.hits[i]);
    printf("\n");
    printf("v7_skel  orig=%zuB chunks=%d enc=%uB ratio=%.2fx [%.1fms] zones=%d\n",
           orig_size, n_chunks, ectx.written, ratio,
           1000.0*(t1-t0)/CLOCKS_PER_SEC, n_zones);
    printf("  ");
    for (int i = 0; i < 10; i++) {
        if (cnt[i])
            printf("%s=%u(%zuB) ", names[i], cnt[i], bytes[i]);
    }
    printf("\n");

    flow_segments_free(segs);
    free(zones);
    for (int z = 0; z < n_zones; z++) ts_enc_free(&ts_ctx[z]);
    free(ts_ctx);
    dfield_free(&df);
    onion_free(&os);
    return 0;

cleanup:
    flow_segments_free(segs);
    free(zones);
    for (int z = 0; z < n_zones; z++) ts_enc_free(&ts_ctx[z]);
    free(ts_ctx);
    dfield_free(&df);
    onion_free(&os);
    return 1;
}

/* ══════════════════════════════════════════════════════════════
 * DECODE
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    FILE         *f;
    DiamondField *df;
    uint8_t      *out;
    uint8_t      *batch_refs;
    int           n_chunks;
    int           n_zones;
    ZoneRecord   *zones;
    int           cur_zone;
    int           ok;
} DecTraverseCtx;

static int dec_traverse(void *ctx, uint32_t ci, uint32_t seq_pos){
    DecTraverseCtx *d = (DecTraverseCtx*)ctx;
    if (seq_pos >= (uint32_t)d->n_chunks) return 1;

    /* Determine zone for this chunk */
    int zone = -1;
    for (int z = 0; z < d->n_zones; z++) {
        if (ci >= d->zones[z].chunk_offset &&
            ci < d->zones[z].chunk_offset + d->zones[z].chunk_count) {
            zone = z; break;
        }
    }
    if (zone < 0) return 1;

    /* Reset transform state at zone boundary */
    if (zone != d->cur_zone) {
        d->cur_zone = zone;
        memset(d->batch_refs + (size_t)seq_pos * 64, 0, 64); /* prev = zeros */
    }

    uint8_t tag;
    if (fread(&tag, 1, 1, d->f) != 1) return 1;
    uint8_t decoded[64];
    uint8_t *prev = (seq_pos > 0) ? d->batch_refs + (size_t)(seq_pos - 1) * 64 : NULL;

    /* Transform tags 5-9 */
    if (tag >= 5 && tag <= 9) {
        if (!prev) return 1;

        switch (tag) {
        case 5: memcpy(decoded, prev, 64); break;
        case 6: {
            uint8_t k;
            if (fread(&k, 1, 1, d->f) != 1) return 1;
            for (int i = 0; i < 64; i++) decoded[i] = prev[(i + 64 - (k & 63)) % 64];
            break;
        }
        case 7:
            for (int i = 0; i < 64; i++) decoded[i] = prev[63 - i];
            break;
        case 8: {
            uint8_t di;
            if (fread(&di, 1, 1, d->f) != 1 || di > 7) return 1;
            _sm_geo_init();
            _sm_d4_inv(decoded, prev, di);
            break;
        }
        case 9: {
            uint8_t count;
            if (fread(&count, 1, 1, d->f) != 1 || count > 48) return 1;
            uint64_t mask;
            if (fread(&mask, 8, 1, d->f) != 1) return 1;
            uint8_t vals[48];
            if (fread(vals, 1, count, d->f) != count) return 1;
            memcpy(decoded, prev, 64);
            int vi = 0;
            for (int i = 0; i < 64; i++)
                if (mask & (1ULL << i)) decoded[i] = vals[vi++];
            break;
        }
        default: return 1;
        }

        memcpy(d->out + (size_t)ci * 64, decoded, 64);
        memcpy(d->batch_refs + (size_t)seq_pos * 64, decoded, 64);
        d->ok++;
        return 0;
    }

    /* Tags 0-4 */
    if (tag == 0) {
        memset(decoded, 0, 64);
    } else if (tag == 1) {
        if (fread(decoded, 1, 64, d->f) != 64) return 1;
    } else if (tag == 2) {
        uint32_t dsz;
        if (fread(&dsz, 4, 1, d->f) != 1 || dsz < 1 || dsz > 64) return 1;
        uint8_t dbuf[64];
        if (fread(dbuf, 1, dsz, d->f) != dsz) return 1;
        uint32_t tick = tring_push(&d->df->tring, dbuf, dsz);
        if (dfield_decode_flat(d->df, tick, decoded)) return 1;
    } else if (tag == 3 || tag == 4) {
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

    /* Read zone index */
    int n_zones = 0;
    if (fread(&n_zones, 4, 1, f) != 1 || n_zones < 1) { fclose(f); return 1; }
    ZoneRecord *zones = malloc((size_t)n_zones * sizeof(ZoneRecord));
    if (fread(zones, sizeof(ZoneRecord), (size_t)n_zones, f) != (size_t)n_zones) { fclose(f); free(zones); return 1; }

    int file_nc;
    if (fread(&file_nc, 4, 1, f) != 1 || file_nc != n_chunks) {
        fprintf(stderr, "chunk count mismatch\n"); fclose(f); free(zones); return 1;
    }

    clock_t t0 = clock();

    DiamondField df;
    dfield_init(&df, (uint32_t)(n_chunks + 64));

    uint8_t *out = calloc((size_t)n_chunks * 64, 1);
    uint8_t *batch_refs = calloc((size_t)n_chunks * 64, 1);

    DecTraverseCtx dctx = {f, &df, out, batch_refs, n_chunks, n_zones, zones, -1, 0};
    traverse_hilbert(&os, n_chunks, dec_traverse, &dctx);

    fclose(f);
    clock_t t1 = clock();

    uint64_t got = xxh64(out, orig_size);
    FILE *g = fopen(out_path, "wb");
    if (!g) { perror(out_path); free(out); free(batch_refs); free(zones); dfield_free(&df); onion_free(&os); return 1; }
    fwrite(out, 1, orig_size, g);
    fclose(g);

    if (got != stored_dig) {
        fprintf(stderr, "FAIL xxh64 got=%016llX stored=%016llX\n",
                (unsigned long long)got, (unsigned long long)stored_dig);
        free(out); free(batch_refs); free(zones);
        dfield_free(&df); onion_free(&os);
        return 2;
    }

    printf("decode   ok=%d/%d orig=%zuB xxh64 PASS [%.1fms]\n",
           dctx.ok, n_chunks, orig_size,
           1000.0*(t1-t0)/CLOCKS_PER_SEC);

    free(out); free(batch_refs); free(zones);
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
        char def[512]; snprintf(def, sizeof(def), "%s.v6", src);
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
        snprintf(dat, sizeof(dat), "%s.v6", src);
        snprintf(out, sizeof(out), "%s.rt.bin", src);

        printf("=== Roundtrip v6 (flow zones): %s ===\n", src);

        FILE *f = fopen(src, "rb");
        if (!f) { perror(src); return 1; }
        fseek(f, 0, SEEK_END); size_t sz = ftell(f); rewind(f);
        uint8_t *data = malloc(sz);
        if (fread(data, 1, sz, f) != sz) { free(data); fclose(f); return 1; }
        fclose(f);

        printf("[1] binary → flow_chunk + ts_encode → .v6\n");
        if (do_encode(data, sz, dat)) { free(data); return 1; }

        printf("[2] .v6 → decode → binary\n");
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
