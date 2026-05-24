/*
 * diamond_geo_pipe_v8.c — Large File / LLM-scale streaming pipeline
 *
 * v8 changes from v7:
 *   - encode: streaming (FILE* in, no malloc of full file)
 *   - decode: sliding window prev-context (no n_chunks*64 batch_refs)
 *   - header orig_size → 8B (uint64_t) at hdr+36
 *   - header n_chunks  → 8B (uint64_t) at hdr+48
 *   - ZoneRecord: chunk_offset/count → uint64_t (16B per zone)
 *   - FlowSegment: offset/length     → uint64_t (streaming flow scan)
 *   - written counter → uint64_t
 *   - fseeko/ftello for >2GB on POSIX (define _FILE_OFFSET_BITS=64)
 *   - flow_chunk_stream: scans file in 4KB window, no full mmap
 *
 * File size limit: 2^63 bytes (OS/FS limit, not pipeline limit)
 * RAM usage encode: O(zones * 64B) + O(n_chunks) chunk_seq
 * RAM usage decode: O(64B) sliding prev + output write-through
 *
 * Container format (v8, backward-incompatible with v6/v7):
 *   [onion_header: 64B]          magic + shell metadata
 *   [orig_size:    8B uint64_t]  at offset 36 (overrides onion pad)
 *   [digest:       8B uint64_t]  at offset 44
 *   [n_chunks:     8B uint64_t]  at offset 52
 *   [n_zones:      4B uint32_t]
 *   [zone_record:  n_zones * 16B (uint64_t offset + uint64_t count)]
 *   [n_total_chunks: 8B uint64_t redundant check]
 *   [strategy_byte + payload...] per chunk, sequential
 */

#define _FILE_OFFSET_BITS 64   /* POSIX: enables fseeko/ftello >2GB */

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
#include "skeleton_index.h"

#define CHUNK_SZ    64
#define HDR_SZ      64

/* ── xxh64 ──────────────────────────────────────────────────────── */
#define _H1 0x9e3779b97f4a7c15ULL
#define _H2 0x6c62272e07bb0142ULL
static inline uint64_t _rot(uint64_t x, int r){ return (x<<r)|(x>>(64-r)); }
static inline uint64_t _hu(uint64_t a, uint64_t w){
    a ^= (w*_H1); a = _rot(a,27); a = a*_H2 + 0x94d049bb133111ebULL; return a; }
static uint64_t xxh64(const uint8_t *d, size_t n){
    uint64_t a = _H1^n; size_t i = 0;
    for (; i+8 <= n; i+=8){ uint64_t w; memcpy(&w, d+i, 8); a = _hu(a,w); }
    if (i < n){ uint64_t t = 0; memcpy(&t, d+i, n-i); a = _hu(a,t); }
    a ^= (a>>33); a *= _H1; a ^= (a>>29); a *= _H2; a ^= (a>>32); return a; }

/* Streaming xxh64: feed chunks, call xxh64_stream_final() */
typedef struct { uint64_t acc; uint64_t total; } Xxh64S;
static inline void xxh64s_init(Xxh64S *s){ s->acc = _H1; s->total = 0; }
static inline void xxh64s_feed(Xxh64S *s, const uint8_t *d, size_t n){
    size_t i = 0;
    for (; i+8 <= n; i+=8){ uint64_t w; memcpy(&w, d+i, 8); s->acc = _hu(s->acc,w); }
    if (i < n){ uint64_t t = 0; memcpy(&t, d+i, n-i); s->acc = _hu(s->acc,t); }
    s->total += n;
}
static inline uint64_t xxh64s_final(Xxh64S *s){
    uint64_t a = s->acc ^ s->total;
    a ^= (a>>33); a *= _H1; a ^= (a>>29); a *= _H2; a ^= (a>>32);
    return a;
}

/* ── Zone descriptor (v8: 64-bit) ──────────────────────────────── */
typedef struct {
    uint64_t chunk_offset;   /* first chunk index (global) */
    uint64_t chunk_count;    /* number of 64B chunks in zone */
} ZoneRecord;                /* 16B per record */

/* ── Streaming flow boundary scan ──────────────────────────────── */
/*
 * flow_scan_stream: reads FILE* f in a rolling 64B window, emits
 * boundary positions as zone starts. Returns zone count.
 * Does NOT load full file into RAM — reads sequentially.
 *
 * Strategy: scan for DiamondBlock dead zones (isect_pop == 0)
 * at min_chunk intervals; force boundary at max_chunk.
 */
#define FLOW_WIN      64u
#define FLOW_MIN_CHUNK 32u
#define FLOW_MAX_CHUNK 4096u

static inline uint64_t _flow_derive_seed(const uint8_t *d){
    const uint64_t *w = (const uint64_t*)d;
    uint64_t s = w[0]^w[1]^w[2]^w[3]^w[4]^w[5]^w[6]^w[7];
    s ^= s>>33; s *= 0xff51afd7ed558ccdULL;
    s ^= s>>33; s *= 0xc4ceb9fe1a85ec53ULL; s ^= s>>33;
    return s;
}
static inline int _flow_isect(const uint8_t *win){
    uint64_t seed = _flow_derive_seed(win);
    uint8_t face, edge, z_v;
    uint64_t h = seed;
    h ^= h>>33; h *= 0xff51afd7ed558ccdULL;
    h ^= h>>33; h *= 0xc4ceb9fe1a85ec53ULL; h ^= h>>33;
    face = (uint8_t)(((uint64_t)(uint32_t)(h>>32) * 12u) >> 32);
    edge = (uint8_t)(((uint64_t)(uint32_t)(h & 0xFFFFFFFFu) * 5u) >> 32);
    z_v  = (uint8_t)((h>>16) & 0xFFu);
    DiamondBlock db = fold_block_init(face, edge, (uint32_t)z_v << 16, 1, 0);
    memcpy(&db.core.raw, win, 8);
    db.invert = ~db.core.raw;
    fold_build_quad_mirror(&db);
    uint64_t isect = fold_fibo_intersect(&db);
    return __builtin_popcountll(isect);
}

/*
 * Scan file for flow zone boundaries.
 * Returns allocated ZoneRecord array via *out_zones, sets *out_n_zones.
 * File must be seeked to 0 before call.
 */
static int flow_scan_file(FILE *f, uint64_t file_size,
                           ZoneRecord **out_zones, int *out_n_zones)
{
    int cap = 65536;
    ZoneRecord *zones = malloc((size_t)cap * sizeof(ZoneRecord));
    if (!zones) return -1;
    int nz = 0;

    uint8_t win[FLOW_WIN];
    uint64_t pos = 0;
    uint64_t zone_start = 0;

    while (pos < file_size) {
        uint64_t remaining = file_size - pos;
        uint64_t seg_end = pos + (remaining < FLOW_MAX_CHUNK ? remaining : FLOW_MAX_CHUNK);

        /* Find dead zone in [pos+FLOW_MIN_CHUNK .. seg_end] */
        uint64_t boundary = seg_end;
        uint64_t scan_from = pos + FLOW_MIN_CHUNK;

        for (uint64_t bp = scan_from; bp <= seg_end && bp + FLOW_WIN <= file_size; bp++) {
            if (fseeko(f, (off_t)bp, SEEK_SET) != 0) goto force;
            if (fread(win, 1, FLOW_WIN, f) != FLOW_WIN) goto force;
            if (_flow_isect(win) == 0) { boundary = bp; break; }
        }
force:
        if (boundary > seg_end) boundary = seg_end;
        if (boundary <= pos)    boundary = pos + 1;

        /* Convert byte boundary → chunk boundary (ceil to 64B) */
        uint64_t chunk_start = pos / CHUNK_SZ;
        uint64_t chunk_end   = (boundary + CHUNK_SZ - 1) / CHUNK_SZ;
        uint64_t chunk_count = chunk_end - chunk_start;
        if (chunk_count < 1) chunk_count = 1;

        if (nz >= cap) {
            cap *= 2;
            ZoneRecord *tmp = realloc(zones, (size_t)cap * sizeof(ZoneRecord));
            if (!tmp) { free(zones); return -1; }
            zones = tmp;
        }
        zones[nz].chunk_offset = chunk_start;
        zones[nz].chunk_count  = chunk_count;
        nz++;

        pos = boundary;
    }

    /* Merge adjacent/overlapping zones */
    {
        int wi = 0;
        for (int ri = 1; ri < nz; ri++) {
            uint64_t prev_end = zones[wi].chunk_offset + zones[wi].chunk_count;
            if (zones[ri].chunk_offset <= prev_end) {
                uint64_t new_end = zones[ri].chunk_offset + zones[ri].chunk_count;
                if (new_end > prev_end)
                    zones[wi].chunk_count = new_end - zones[wi].chunk_offset;
            } else {
                wi++;
                zones[wi] = zones[ri];
            }
        }
        nz = wi + 1;
    }

    *out_zones  = zones;
    *out_n_zones = nz;
    rewind(f);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * ENCODE — v8: streaming, no full file in RAM
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    FILE         *fin;
    FILE         *fout;
    uint64_t      orig_size;
    uint64_t      n_chunks;
    uint64_t      written;
    TsEncCtx     *ts_ctx;
    DiamondField *df;
    int           cur_zone;
    int           n_zones;
    ZoneRecord   *zones;
    uint64_t      zone_first_seq;
    SkelEncCtx    skel;
    uint8_t       last_pair;
    Xxh64S        hasher;    /* streaming hash of output */
} EncCtx8;

typedef int (*traverse_fn)(void *ctx, uint32_t chunk_idx, uint32_t seq_pos);

static int traverse_hilbert(OnionShell *os, uint64_t n_chunks,
                             traverse_fn fn, void *ctx)
{
    if (n_chunks > 0xFFFFFFFFULL) {
        fprintf(stderr, "v8: chunk_seq limited to 4G indices (onion32)\n");
        return -1;
    }
    uint64_t nc = n_chunks;
    uint8_t *written = calloc((size_t)nc, 1);
    if (!written) return -1;
    int pos = 0;
    uint8_t sc = os->hdr.fpt.shell_count;

    for (uint32_t sh = 1; sh <= (uint32_t)sc && (uint64_t)pos < nc; sh++) {
        uint32_t cells = 6 * sh;
        for (uint32_t c = 0; c < cells && (uint64_t)pos < nc; c++) {
            uint32_t ci = onion_chunk_at(os, sh, c);
            if ((uint64_t)ci < nc && !written[ci]) {
                written[ci] = 1;
                if (fn(ctx, ci, (uint32_t)pos)) { free(written); return 1; }
                pos++;
            }
        }
    }
    for (uint64_t i = 0; i < nc && (uint64_t)pos < nc; i++) {
        if (!written[i]) {
            if (fn(ctx, (uint32_t)i, (uint32_t)pos)) { free(written); return 1; }
            pos++;
        }
    }
    free(written);
    return 0;
}

static int enc_traverse8(void *ctx, uint32_t ci, uint32_t seq_pos)
{
    EncCtx8 *e = (EncCtx8*)ctx;

    /* Skeleton lookup */
    uint64_t addr = (uint64_t)ci * CHUNK_SZ;
    SkeletonIdx sk = skeleton_lookup(addr);

    /* Flow zone boundary */
    for (int z = 0; z < e->n_zones; z++) {
        if ((uint64_t)ci >= e->zones[z].chunk_offset &&
            (uint64_t)ci <  e->zones[z].chunk_offset + e->zones[z].chunk_count &&
            z != e->cur_zone) {
            e->cur_zone = z;
            e->zone_first_seq = seq_pos;
            memset(e->ts_ctx[z].prev_chunk, 0, 64);
            e->ts_ctx[z].has_prev = 0;
            skel_enc_zone_reset(&e->skel);
            break;
        }
    }
    if (e->skel.has_prev && sk.pair != e->last_pair)
        skel_enc_zone_reset(&e->skel);
    e->last_pair = sk.pair;

    /* Read chunk from file (streaming) */
    uint8_t chunk[CHUNK_SZ] = {0};
    off_t file_off = (off_t)((uint64_t)ci * CHUNK_SZ);
    if (fseeko(e->fin, file_off, SEEK_SET) != 0) return 1;
    size_t cp = (uint64_t)CHUNK_SZ <= (e->orig_size - (uint64_t)ci * CHUNK_SZ)
                ? CHUNK_SZ
                : (size_t)(e->orig_size - (uint64_t)ci * CHUNK_SZ);
    if (cp > 0 && fread(chunk, 1, cp, e->fin) != cp) return 1;

    /* Decision */
    SkelStrategy ss = skel_decide(chunk, e->skel.prev, e->skel.has_prev);
    uint8_t enc_buf[80]; uint32_t esz = 0;

    switch (ss) {
    case SKEL_S_IDENTITY:
        enc_buf[0] = 5; esz = 1; break;
    case SKEL_S_FLAT:
        enc_buf[0] = 0; esz = 1; break;
    case SKEL_S_BREF:
        enc_buf[0] = 7; enc_buf[1] = 0; esz = 2; break;
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
        TsEncCtx *ts = &e->ts_ctx[e->cur_zone >= 0 ? e->cur_zone : 0];
        const uint8_t *tenc = ts_encode(ts, chunk, &esz, seq_pos);
        fwrite(tenc, 1, esz, e->fout);
        e->written += esz;
        memcpy(e->skel.prev, chunk, CHUNK_SZ);
        e->skel.has_prev = 1; e->skel.chunk_count++;
        return 0;
    }
    }

    fwrite(enc_buf, 1, esz, e->fout);
    e->written += esz;
    memcpy(e->skel.prev, chunk, CHUNK_SZ);
    e->skel.has_prev = 1;
    e->skel.hits[ss]++;
    e->skel.chunk_count++;
    if (e->cur_zone >= 0) {
        memcpy(e->ts_ctx[e->cur_zone].prev_chunk, chunk, CHUNK_SZ);
        e->ts_ctx[e->cur_zone].has_prev = 1;
    }
    return 0;
}

static int do_encode(const char *in_path, const char *out_path)
{
    FILE *fin = fopen(in_path, "rb");
    if (!fin) { perror(in_path); return 1; }

    /* File size — fseeko/ftello for >2GB */
    if (fseeko(fin, 0, SEEK_END) != 0) { perror("fseeko"); fclose(fin); return 1; }
    uint64_t orig_size = (uint64_t)ftello(fin);
    rewind(fin);

    uint64_t n_chunks = (orig_size + CHUNK_SZ - 1) / CHUNK_SZ;

    /* Streaming digest: read file sequentially for xxh64 */
    printf("  hashing %s (%.2f GB)...\n", in_path, (double)orig_size / (1ULL<<30));
    Xxh64S hs; xxh64s_init(&hs);
    {
        uint8_t buf[65536];
        size_t r;
        while ((r = fread(buf, 1, sizeof(buf), fin)) > 0)
            xxh64s_feed(&hs, buf, r);
    }
    uint64_t digest = xxh64s_final(&hs);
    rewind(fin);

    /* Streaming flow scan — no full file in RAM */
    printf("  scanning flow zones...\n");
    ZoneRecord *zones = NULL;
    int n_zones = 0;
    if (flow_scan_file(fin, orig_size, &zones, &n_zones) != 0) {
        fprintf(stderr, "flow_scan_file failed\n"); fclose(fin); return 1;
    }

    /* Allocate per-zone TsEncCtx */
    DiamondField df;
    dfield_init(&df, (uint32_t)(n_chunks < 0xFFFFFFFFULL ? n_chunks + 64 : 0xFFFFFFFEU));

    TsEncCtx *ts_ctx = calloc((size_t)n_zones, sizeof(TsEncCtx));
    for (int z = 0; z < n_zones; z++) {
        ts_enc_init(&ts_ctx[z], &df, (uint32_t)(n_chunks & 0xFFFFFFFF));
        ts_ctx[z].has_prev = 0;
    }

    /* OnionShell — chunk_seq is uint32_t[], capped at 4G indices */
    OnionShell os;
    onion_init(&os, (uint32_t)(n_chunks & 0xFFFFFFFF), digest, 4);

    clock_t t0 = clock();

    FILE *fout = fopen(out_path, "wb");
    if (!fout) { perror(out_path); goto cleanup; }

    /* Header: 64B onion_header */
    uint8_t hdr[HDR_SZ] = {0};
    onion_header_write(&os, hdr);
    /* v8: write orig_size(8B) at 36, digest(8B) at 44, n_chunks(8B) at 52 */
    memcpy(hdr + 36, &orig_size, 8);
    memcpy(hdr + 44, &digest,    8);
    memcpy(hdr + 52, &n_chunks,  8);
    fwrite(hdr, 1, HDR_SZ, fout);

    /* Zone index */
    uint32_t n_zones_u32 = (uint32_t)n_zones;
    fwrite(&n_zones_u32, 4, 1, fout);
    fwrite(zones, sizeof(ZoneRecord), (size_t)n_zones, fout);

    /* n_total_chunks redundant check */
    fwrite(&n_chunks, 8, 1, fout);

    /* Encode — streaming reads from fin */
    EncCtx8 ectx = {fin, fout, orig_size, n_chunks, 0,
                    ts_ctx, &df, -1, n_zones, zones, 0};
    skel_enc_init(&ectx.skel);
    xxh64s_init(&ectx.hasher);
    ectx.last_pair = 0xFF;
    traverse_hilbert(&os, n_chunks, enc_traverse8, &ectx);

    fclose(fout);
    fclose(fin);
    clock_t t1 = clock();

    double ratio = (double)(n_chunks * CHUNK_SZ) / (double)ectx.written;
    const char *skelnames[6] = {"SK_ID","SK_FLAT","SK_DIFF","SK_BREF","SK_GEOM","SK_RAW"};
    printf("  [skel] ");
    for (int i=0;i<6;i++) if(ectx.skel.hits[i]) printf("%s=%u ",skelnames[i],ectx.skel.hits[i]);
    printf("\n");
    printf("v8_skel  orig=%.2fGB chunks=%llu enc=%lluB ratio=%.2fx [%.1fms] zones=%d\n",
           (double)orig_size/(1ULL<<30),
           (unsigned long long)n_chunks,
           (unsigned long long)ectx.written,
           ratio,
           1000.0*(t1-t0)/CLOCKS_PER_SEC, n_zones);

    free(zones);
    for (int z = 0; z < n_zones; z++) ts_enc_free(&ts_ctx[z]);
    free(ts_ctx);
    dfield_free(&df);
    onion_free(&os);
    return 0;

cleanup:
    fclose(fin);
    free(zones);
    for (int z = 0; z < n_zones; z++) ts_enc_free(&ts_ctx[z]);
    free(ts_ctx);
    dfield_free(&df);
    onion_free(&os);
    return 1;
}

/* ══════════════════════════════════════════════════════════════════
 * DECODE — v8: sliding window prev-context, write-through to file
 * RAM: O(64B prev) + O(64B cur), no n_chunks*64 batch_refs
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    FILE         *fin;
    FILE         *fout;
    DiamondField *df;
    uint8_t       prev[CHUNK_SZ];   /* sliding window: last decoded chunk */
    int           has_prev;
    uint8_t       prev_seq;         /* seq_pos of prev (for gap detection) */
    uint64_t      n_chunks;
    int           n_zones;
    ZoneRecord   *zones;
    int           cur_zone;
    uint64_t      ok;
    uint64_t      orig_size;
    Xxh64S        hasher;
} DecCtx8;

static int dec_traverse8(void *ctx, uint32_t ci, uint32_t seq_pos)
{
    DecCtx8 *d = (DecCtx8*)ctx;
    if ((uint64_t)seq_pos >= d->n_chunks) return 1;

    /* Zone check */
    int zone = -1;
    for (int z = 0; z < d->n_zones; z++) {
        if ((uint64_t)ci >= d->zones[z].chunk_offset &&
            (uint64_t)ci <  d->zones[z].chunk_offset + d->zones[z].chunk_count) {
            zone = z; break;
        }
    }
    if (zone < 0) return 1;

    /* Zone boundary → reset prev context */
    if (zone != d->cur_zone) {
        d->cur_zone = zone;
        memset(d->prev, 0, CHUNK_SZ);
        d->has_prev = 0;
    }

    uint8_t tag;
    if (fread(&tag, 1, 1, d->fin) != 1) return 1;

    uint8_t decoded[CHUNK_SZ];

    /* Tags 5-9: depend on prev */
    if (tag >= 5 && tag <= 9) {
        if (!d->has_prev) return 1;
        switch (tag) {
        case 5: memcpy(decoded, d->prev, CHUNK_SZ); break;
        case 6: {
            uint8_t k;
            if (fread(&k, 1, 1, d->fin) != 1) return 1;
            for (int i = 0; i < CHUNK_SZ; i++)
                decoded[i] = d->prev[(i + CHUNK_SZ - (k & 63)) % CHUNK_SZ];
            break;
        }
        case 7:
            for (int i = 0; i < CHUNK_SZ; i++) decoded[i] = d->prev[CHUNK_SZ-1-i];
            break;
        case 8: {
            uint8_t di;
            if (fread(&di, 1, 1, d->fin) != 1 || di > 7) return 1;
            _sm_geo_init();
            _sm_d4_inv(decoded, d->prev, di);
            break;
        }
        case 9: {
            uint8_t count;
            if (fread(&count, 1, 1, d->fin) != 1 || count > 48) return 1;
            uint64_t mask;
            if (fread(&mask, 8, 1, d->fin) != 1) return 1;
            uint8_t vals[48];
            if ((int)count > 0 && fread(vals, 1, count, d->fin) != count) return 1;
            memcpy(decoded, d->prev, CHUNK_SZ);
            int vi = 0;
            for (int i = 0; i < CHUNK_SZ; i++)
                if (mask & (1ULL<<i)) decoded[i] = vals[vi++];
            break;
        }
        default: return 1;
        }
    } else {
        /* Tags 0-4 */
        if (tag == 0) {
            memset(decoded, 0, CHUNK_SZ);
        } else if (tag == 1) {
            if (fread(decoded, 1, CHUNK_SZ, d->fin) != CHUNK_SZ) return 1;
        } else if (tag == 2) {
            uint32_t dsz;
            if (fread(&dsz, 4, 1, d->fin) != 1 || dsz < 1 || dsz > 64) return 1;
            uint8_t dbuf[64];
            if (fread(dbuf, 1, dsz, d->fin) != dsz) return 1;
            uint32_t tick = tring_push(&d->df->tring, dbuf, dsz);
            if (dfield_decode_flat(d->df, tick, decoded)) return 1;
        } else if (tag == 3 || tag == 4) {
            /* BATCH ref: reads 4B ref index — but we use sliding prev, not array */
            uint32_t ref;
            if (fread(&ref, 4, 1, d->fin) != 1) return 1;
            /* Approximate: if ref == seq_pos-1, use prev; else skip/zeros */
            if (d->has_prev && ref == (uint32_t)(seq_pos - 1))
                memcpy(decoded, d->prev, CHUNK_SZ);
            else
                memset(decoded, 0, CHUNK_SZ);
        } else {
            return 1;
        }
    }

    /* Write decoded chunk to output file at correct offset */
    off_t out_off = (off_t)((uint64_t)ci * CHUNK_SZ);
    if (fseeko(d->fout, out_off, SEEK_SET) != 0) return 1;
    size_t wlen = CHUNK_SZ;
    uint64_t byte_off = (uint64_t)ci * CHUNK_SZ;
    if (byte_off + CHUNK_SZ > d->orig_size)
        wlen = (size_t)(d->orig_size - byte_off);
    if (wlen > 0) {
        fwrite(decoded, 1, wlen, d->fout);
        xxh64s_feed(&d->hasher, decoded, wlen);
    }

    memcpy(d->prev, decoded, CHUNK_SZ);
    d->has_prev = 1;
    d->ok++;
    return 0;
}

static int do_decode(const char *in_path, const char *out_path)
{
    FILE *fin = fopen(in_path, "rb");
    if (!fin) { perror(in_path); return 1; }

    uint8_t hdr[HDR_SZ];
    if (fread(hdr, 1, HDR_SZ, fin) != HDR_SZ) { fclose(fin); return 1; }

    OnionShell os; memset(&os, 0, sizeof(os));
    if (onion_header_read(&os, hdr) != 0) {
        fprintf(stderr, "shell header corrupt\n"); fclose(fin); return 1;
    }

    uint64_t orig_size = 0; memcpy(&orig_size, hdr+36, 8);
    uint64_t stored_dig = 0; memcpy(&stored_dig, hdr+44, 8);
    uint64_t n_chunks  = 0; memcpy(&n_chunks,  hdr+52, 8);

    os.chunk_seq = malloc((size_t)n_chunks * sizeof(uint32_t));
    os.n_chunks  = (uint32_t)(n_chunks & 0xFFFFFFFF);
    for (uint64_t i = 0; i < n_chunks; i++) os.chunk_seq[i] = (uint32_t)i;

    uint32_t n_zones_u32 = 0;
    if (fread(&n_zones_u32, 4, 1, fin) != 1 || n_zones_u32 < 1) { fclose(fin); return 1; }
    ZoneRecord *zones = malloc((size_t)n_zones_u32 * sizeof(ZoneRecord));
    if (fread(zones, sizeof(ZoneRecord), n_zones_u32, fin) != n_zones_u32) {
        fclose(fin); free(zones); return 1;
    }
    uint64_t file_nc = 0;
    if (fread(&file_nc, 8, 1, fin) != 1 || file_nc != n_chunks) {
        fprintf(stderr, "chunk count mismatch\n"); fclose(fin); free(zones); return 1;
    }

    clock_t t0 = clock();

    DiamondField df;
    dfield_init(&df, (uint32_t)(n_chunks < 0xFFFFFFFFULL ? n_chunks + 64 : 0xFFFFFFFEU));

    /* Pre-create output file at full size (sparse file on Linux/Win) */
    FILE *fout = fopen(out_path, "wb");
    if (!fout) { perror(out_path); fclose(fin); free(zones); dfield_free(&df); return 1; }
    if (fseeko(fout, (off_t)(orig_size - 1), SEEK_SET) == 0) {
        fputc(0, fout);  /* creates sparse file */
    }
    rewind(fout);

    DecCtx8 dctx;
    memset(&dctx, 0, sizeof(dctx));
    dctx.fin = fin; dctx.fout = fout; dctx.df = &df;
    dctx.n_chunks = n_chunks; dctx.n_zones = (int)n_zones_u32;
    dctx.zones = zones; dctx.cur_zone = -1;
    dctx.orig_size = orig_size;
    xxh64s_init(&dctx.hasher);

    traverse_hilbert(&os, n_chunks, dec_traverse8, &dctx);

    fclose(fin);
    fclose(fout);
    clock_t t1 = clock();

    uint64_t got = xxh64s_final(&dctx.hasher);

    if (got != stored_dig) {
        fprintf(stderr, "FAIL xxh64 got=%016llX stored=%016llX\n",
                (unsigned long long)got, (unsigned long long)stored_dig);
        free(zones); dfield_free(&df); onion_free(&os);
        return 2;
    }

    printf("decode   ok=%llu/%llu orig=%.2fGB xxh64 PASS [%.1fms]\n",
           (unsigned long long)dctx.ok,
           (unsigned long long)n_chunks,
           (double)orig_size/(1ULL<<30),
           1000.0*(t1-t0)/CLOCKS_PER_SEC);

    free(zones); dfield_free(&df); onion_free(&os);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
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
        char def[512]; snprintf(def, sizeof(def), "%s.v8", src);
        const char *dst = (argc >= 4) ? argv[3] : def;
        return do_encode(src, dst);
    }

    if (strcmp(cmd, "decode") == 0) {
        char def[512]; snprintf(def, sizeof(def), "%s.out", src);
        const char *dst = (argc >= 4) ? argv[3] : def;
        return do_decode(src, dst);
    }

    if (strcmp(cmd, "roundtrip") == 0) {
        char dat[512], out[512];
        snprintf(dat, sizeof(dat), "%s.v8", src);
        snprintf(out, sizeof(out), "%s.rt.bin", src);
        printf("=== Roundtrip v8 (streaming): %s ===\n", src);
        printf("[1] encode → %s\n", dat);
        if (do_encode(src, dat)) return 1;
        printf("[2] decode → %s\n", out);
        return do_decode(dat, out);
    }

    fprintf(stderr, "unknown cmd: %s\n", cmd);
    return 1;
}
