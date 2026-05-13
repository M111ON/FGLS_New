/*
 * gpx5_container.h — GeoPixel v5 Container (Hamburger Architecture)
 *
 * Extends GPX4 (gpx4_container_o22.h) — backward-compatible magic/version bump.
 *
 * Core model:
 *   R/G/B are NOT color channels — they are carrier variables.
 *   Header assigns a payload type to each carrier before pipeline starts.
 *   Each carrier selects its own codec. Carriers live in separate Hilbert
 *   address dimensions — they never collide by construction (tring_encode ch=0,1,2).
 *
 *   1 SET = 4 planes stacked:
 *     plane[0] = carrier R  (ch=0, live zone   0..3455)
 *     plane[1] = carrier G  (ch=1, live zone   0..3455)
 *     plane[2] = carrier B  (ch=2, residual zone 3456..6911)
 *     plane[3] = invert Hilbert  ← NOT computed, NOT stored as data
 *                                  it is the negative space of planes 0+1+2
 *                                  signature emerges from geometry alone
 *
 *   plane[3] is the OUTPUT that feeds the next level — and the source of
 *   the LUT pointer table after warm-up (1440 ticks).
 *
 * File layout:
 *
 *   [GPX5_FILE_HDR]     24B
 *   [PIPE_TABLE]        3 entries × 8B = 24B   (R/G/B carrier config)
 *   [SET_TABLE]         n_sets × GPX5_SET_ENTRY_SZ
 *   [LUT_TABLE]         n_tiles × 8B            (built after warm-up, optional)
 *   [LAYER DATA]        raw bytes
 *
 * Tick model:
 *   System runs 1440 ticks (= 144 × 10, sacred period × scale factor).
 *   After full run: LUT + builtin pointer table is frozen.
 *   Random access = O(1) via LUT, no linear traversal needed after that.
 *
 * Block/fence (17³ geometry):
 *   block = 4896 bytes  (= 17 × 2 × 144, actual storage)
 *   fence = 17  bytes   (drain gap — switch gate, NOT stored)
 *   total = 4913 = 17³  (geometric boundary)
 *
 * Sacred numbers: 144, 1440, 4896, 4913, 17, 54, 12, 27, 3456, 6912
 * No float. No malloc inside hot path.
 *
 * Backward compat:
 *   GPX4 files: magic='GPX4', version<0x05 → use gpx4_container_o22.h reader
 *   GPX5 files: magic='GPX5', version=0x05
 */

#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── magic & version ─────────────────────────────────── */
#define GPX5_MAGIC          0x47505835u   /* 'GPX5' */
#define GPX5_VERSION        0x05

/* ── sacred geometry ─────────────────────────────────── */
#define GPX5_TICK_PERIOD    1440u         /* full warm-up cycle          */
#define GPX5_BLOCK_SZ       4896u         /* = 17 × 2 × 144              */
#define GPX5_FENCE_SZ       17u           /* drain gap (not stored)       */
#define GPX5_CUBE_SZ        4913u         /* = 17³ = block + fence        */
#define GPX5_COMPOUNDS      144u          /* fibo clock period            */
#define GPX5_LANES          54u           /* Rubik sticker count          */
#define GPX5_DRAIN_COUNT    12u           /* pentagon drain               */

/* ── planes per set ──────────────────────────────────── */
#define GPX5_PLANES_PER_SET 4u
#define GPX5_PLANE_R        0u            /* carrier R, ch=0, live zone   */
#define GPX5_PLANE_G        1u            /* carrier G, ch=1, live zone   */
#define GPX5_PLANE_B        2u            /* carrier B, ch=2, residual    */
#define GPX5_PLANE_INV      3u            /* invert Hilbert — signature   */

/* ── carrier payload types (what header assigns to R/G/B) ─ */
#define GPX5_CTYPE_FLAT     0x00  /* uniform / bg — seed only, delta≈0  */
#define GPX5_CTYPE_GRAD     0x01  /* gradient field                      */
#define GPX5_CTYPE_EDGE     0x02  /* edge / boundary data                */
#define GPX5_CTYPE_NOISE    0x03  /* bounded noise — stored as params    */
#define GPX5_CTYPE_VECTOR   0x04  /* motion vector / animation param     */
#define GPX5_CTYPE_FREQ     0x05  /* frequency separation output (FFT δ) */
#define GPX5_CTYPE_SIG      0x06  /* signature passthrough (invert feed) */
#define GPX5_CTYPE_RAW      0xFF  /* fallback: raw bytes                 */

/* ── codec ids (carrier self-selects) ───────────────────── */
#define GPX5_CODEC_NONE     0x00  /* no-op — pass through                */
#define GPX5_CODEC_SEED     0x01  /* single seed, reconstruct on decode  */
#define GPX5_CODEC_DELTA    0x02  /* delta chain vs previous set         */
#define GPX5_CODEC_RICE3    0x03  /* Rice(k=3) for count stream          */
#define GPX5_CODEC_ZSTD19   0x04  /* zstd level 19                       */
#define GPX5_CODEC_FREQ     0x05  /* frequency params (range+freq_seed)  */
#define GPX5_CODEC_HILBERT  0x06  /* hilbert bit stream (walk/no-walk)   */
#define GPX5_CODEC_RAW      0xFF  /* explicit raw passthrough fallback   */

/* ── tile classify (same as v18 classify output) ─────────── */
#define GPX5_TTYPE_FLAT     0x00
#define GPX5_TTYPE_GRADIENT 0x01
#define GPX5_TTYPE_EDGE     0x02
#define GPX5_TTYPE_NOISE    0x03

/* ── LUT flags ───────────────────────────────────────────── */
#define GPX5_LUT_WARM       0x01  /* LUT is valid (warm-up complete)     */
#define GPX5_LUT_FROZEN     0x02  /* LUT frozen — O(1) random access     */

/* ── sizes ───────────────────────────────────────────────── */
#define GPX5_FILE_HDR_SZ    24u
#define GPX5_PIPE_ENTRY_SZ   8u   /* per carrier: 3 entries total        */
#define GPX5_SET_ENTRY_SZ   16u   /* per set                             */
#define GPX5_LUT_ENTRY_SZ    8u   /* per tile: frozen pointer            */
#define GPX5_TILE_META_SZ    8u   /* per tile in set: type+codec+offset  */

/* ── byte I/O ────────────────────────────────────────────── */
static inline void g5w2(uint8_t *b, uint16_t v)
    { b[0]=(uint8_t)(v>>8); b[1]=(uint8_t)v; }
static inline void g5w4(uint8_t *b, uint32_t v)
    { b[0]=(uint8_t)(v>>24); b[1]=(uint8_t)(v>>16);
      b[2]=(uint8_t)(v>>8);  b[3]=(uint8_t)v; }
static inline uint16_t g5r2(const uint8_t *b)
    { return (uint16_t)((b[0]<<8)|b[1]); }
static inline uint32_t g5r4(const uint8_t *b)
    { return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|
             ((uint32_t)b[2]<<8)|(uint32_t)b[3]; }

/* ══════════════════════════════════════════════════════════
 * STRUCTS
 * ══════════════════════════════════════════════════════════ */

/*
 * Gpx5FileHdr — 24B
 *   magic    4B  'GPX5'
 *   version  1B  0x05
 *   flags    1B  GPX5_LUT_* bits
 *   n_sets   2B  number of sets in file
 *   tw       2B  tile columns
 *   th       2B  tile rows
 *   n_tiles  4B  tw × th
 *   global_seed 4B  root seed (a=2,b=3 origin)
 *   tick_max 2B  tick budget (default 1440)
 *   reserved 2B
 */
typedef struct {
    uint32_t magic;        /* GPX5_MAGIC                              */
    uint8_t  version;      /* GPX5_VERSION                            */
    uint8_t  flags;        /* GPX5_LUT_*                              */
    uint16_t n_sets;
    uint16_t tw;
    uint16_t th;
    uint32_t n_tiles;
    uint32_t global_seed;
    uint16_t tick_max;     /* default 1440                            */
    uint16_t reserved;
} Gpx5FileHdr;             /* 24B */

/*
 * Gpx5PipeEntry — 8B per carrier (3 carriers: R/G/B)
 *   carrier_id  1B   0=R 1=G 2=B
 *   ctype       1B   GPX5_CTYPE_* (what this carrier carries)
 *   codec       1B   GPX5_CODEC_* (how it encodes)
 *   load_pct    1B   0-100, current load share (for dynamic balancing)
 *   hilbert_mod 1B   modulus for pipe assignment (default 3)
 *   hilbert_rem 1B   remainder: tile_hilbert_pos % hilbert_mod == hilbert_rem
 *   lflags      2B  GPX5_PIPE_FLAG_*
 *
 *   Default assignment:
 *     R: hilbert_pos % 3 == 0
 *     G: hilbert_pos % 3 == 1
 *     B: hilbert_pos % 3 == 2
 *
 *   Dynamic balancing (R heavy example):
 *     R: hilbert_pos % 6 == 0
 *     G: hilbert_pos % 6 == 1 OR 3  (G helps R via extra remainder)
 *     B: hilbert_pos % 6 == 2
 */
typedef struct {
    uint8_t  carrier_id;
    uint8_t  ctype;
    uint8_t  codec;
    uint8_t  load_pct;
    uint8_t  hilbert_mod;
    uint8_t  hilbert_rem;
    uint16_t lflags;        /* GPX5_PIPE_FLAG_* bits */
} Gpx5PipeEntry;           /* 8B */

/*
 * Gpx5SetEntry — 16B per set
 *   set_id      2B
 *   level       2B   which processing level (0..N, flexible)
 *   tick_start  2B   first tick of this set
 *   tick_end    2B   last tick of this set (tick_end - tick_start = budget)
 *   data_offset 4B   byte offset of set data from file start
 *   data_size   4B   byte size of set data
 *
 *   plane[3] (invert) data is NOT stored here.
 *   It is derived: negative space of planes 0+1+2 at this set's tick range.
 *   Its resulting pointer is in the LUT table after warm-up.
 */
typedef struct {
    uint16_t set_id;
    uint16_t level;
    uint16_t tick_start;
    uint16_t tick_end;
    uint32_t data_offset;
    uint32_t data_size;
} Gpx5SetEntry;            /* 16B */

/*
 * Gpx5TileMeta — 8B per tile per set (inside set data)
 *   tile_id     2B
 *   ttype       1B   GPX5_TTYPE_* (from classify, same as v18)
 *   codec       1B   GPX5_CODEC_* (carrier selected)
 *   carrier_id  1B   which carrier owns this tile
 *   plane       1B   which plane (0=R,1=G,2=B,3=INV)
 *   blob_sz     2B   bytes of encoded data for this tile
 */
typedef struct {
    uint16_t tile_id;
    uint8_t  ttype;
    uint8_t  codec;
    uint8_t  carrier_id;
    uint8_t  plane;
    uint16_t blob_sz;
} Gpx5TileMeta;            /* 8B */

/*
 * Gpx5LutEntry — 8B per tile (frozen after warm-up)
 *   tile_id        2B
 *   set_id         2B   which set owns this tile's final state
 *   invert_offset  4B   byte offset to invert-plane signature for this tile
 *                       → direct jump, no traversal needed
 *
 *   After GPX5_LUT_FROZEN is set in file flags:
 *   reader can jump directly to invert_offset without touching any set data.
 *   This is the O(1) random access guarantee.
 */
typedef struct {
    uint16_t tile_id;
    uint16_t set_id;
    uint32_t invert_offset;
} Gpx5LutEntry;            /* 8B */

/* ══════════════════════════════════════════════════════════
 * SEED DERIVATION (integer only, no float, no malloc)
 * ══════════════════════════════════════════════════════════ */

/*
 * gpx5_seed_local — derive per-tile seed from global_seed + tile_id
 *   Uses xorshift mix — deterministic, reversible, no external state.
 *   Same result on encode and decode: no need to store seed_local.
 */
static inline uint32_t gpx5_seed_local(uint32_t global_seed, uint32_t tile_id) {
    uint32_t s = global_seed ^ (tile_id * 0x9e3779b9u);
    s ^= s >> 16; s *= 0x85ebca6bu;
    s ^= s >> 13; s *= 0xc2b2ae35u;
    s ^= s >> 16;
    return s;
}

/*
 * gpx5_hilbert_entry — derive hilbert start position for tile
 *   from seed_local + tile geometry position.
 *   Output is position within [0, GPX5_COMPOUNDS) — fibo clock domain.
 */
static inline uint16_t gpx5_hilbert_entry(uint32_t seed_local, uint16_t tile_id) {
    return (uint16_t)((seed_local ^ ((uint32_t)tile_id * 7u)) % GPX5_COMPOUNDS);
}

/*
 * gpx5_pipe_for_tile — which carrier owns this tile
 *   Default: hilbert_pos % 3
 *   With dynamic balancing: use pipe table entries
 */
static inline uint8_t gpx5_pipe_for_tile(
        uint16_t hilbert_pos,
        const Gpx5PipeEntry pipes[3])
{
    for (int i = 0; i < 3; i++) {
        if ((hilbert_pos % pipes[i].hilbert_mod) == pipes[i].hilbert_rem)
            return pipes[i].carrier_id;
    }
    return (uint8_t)(hilbert_pos % 3u);   /* fallback default */
}

/* ══════════════════════════════════════════════════════════
 * BLOCK / FENCE — 17³ geometry
 * ══════════════════════════════════════════════════════════ */

/*
 * gpx5_block_for_tick — which block index does tick N fall in?
 *   ticks per block = GPX5_COMPOUNDS = 144 (one fibo period per block)
 *   drain tick (fence) = tick where (tick % 144) == 0 → not stored
 */
static inline uint32_t gpx5_block_for_tick(uint32_t tick) {
    return tick / GPX5_COMPOUNDS;
}

static inline int gpx5_is_drain_tick(uint32_t tick) {
    return (tick % GPX5_COMPOUNDS) == 0;
}

/* ══════════════════════════════════════════════════════════
 * WRITE HELPERS
 * ══════════════════════════════════════════════════════════ */

static inline void gpx5_hdr_write(uint8_t *b, const Gpx5FileHdr *h) {
    g5w4(b+ 0, h->magic);
    b[4] = h->version; b[5] = h->flags;
    g5w2(b+ 6, h->n_sets);
    g5w2(b+ 8, h->tw);
    g5w2(b+10, h->th);
    g5w4(b+12, h->n_tiles);
    g5w4(b+16, h->global_seed);
    g5w2(b+20, h->tick_max);
    g5w2(b+22, h->reserved);
}

static inline void gpx5_hdr_read(const uint8_t *b, Gpx5FileHdr *h) {
    h->magic       = g5r4(b+ 0);
    h->version     = b[4]; h->flags = b[5];
    h->n_sets      = g5r2(b+ 6);
    h->tw          = g5r2(b+ 8);
    h->th          = g5r2(b+10);
    h->n_tiles     = g5r4(b+12);
    h->global_seed = g5r4(b+16);
    h->tick_max    = g5r2(b+20);
    h->reserved    = g5r2(b+22);
}

static inline void gpx5_pipe_write(uint8_t *b, const Gpx5PipeEntry *p) {
    b[0]=p->carrier_id; b[1]=p->ctype;   b[2]=p->codec;
    b[3]=p->load_pct;   b[4]=p->hilbert_mod; b[5]=p->hilbert_rem;
    b[6]=(uint8_t)(p->lflags>>8); b[7]=(uint8_t)(p->lflags);
}

static inline void gpx5_pipe_read(const uint8_t *b, Gpx5PipeEntry *p) {
    p->carrier_id  = b[0]; p->ctype       = b[1]; p->codec       = b[2];
    p->load_pct    = b[3]; p->hilbert_mod = b[4]; p->hilbert_rem = b[5];
    p->lflags      = (uint16_t)((b[6]<<8)|b[7]);
}

static inline void gpx5_set_write(uint8_t *b, const Gpx5SetEntry *s) {
    g5w2(b+ 0, s->set_id);    g5w2(b+ 2, s->level);
    g5w2(b+ 4, s->tick_start); g5w2(b+ 6, s->tick_end);
    g5w4(b+ 8, s->data_offset); g5w4(b+12, s->data_size);
}

static inline void gpx5_set_read(const uint8_t *b, Gpx5SetEntry *s) {
    s->set_id      = g5r2(b+ 0); s->level      = g5r2(b+ 2);
    s->tick_start  = g5r2(b+ 4); s->tick_end   = g5r2(b+ 6);
    s->data_offset = g5r4(b+ 8); s->data_size  = g5r4(b+12);
}

static inline void gpx5_lut_write(uint8_t *b, const Gpx5LutEntry *e) {
    g5w2(b+0, e->tile_id); g5w2(b+2, e->set_id);
    g5w4(b+4, e->invert_offset);
}

static inline void gpx5_lut_read(const uint8_t *b, Gpx5LutEntry *e) {
    e->tile_id       = g5r2(b+0);
    e->set_id        = g5r2(b+2);
    e->invert_offset = g5r4(b+4);
}

/* ══════════════════════════════════════════════════════════
 * DEFAULT PIPE TABLE — 3 carriers, balanced, no dynamic
 * ══════════════════════════════════════════════════════════ */

/*
 * gpx5_default_pipes — fill pipe table with default 3-way split.
 *   ctype / codec must be set by caller based on input data type.
 */
static inline void gpx5_default_pipes(Gpx5PipeEntry pipes[3]) {
    for (int i = 0; i < 3; i++) {
        pipes[i].carrier_id  = (uint8_t)i;
        pipes[i].ctype       = GPX5_CTYPE_RAW;
        pipes[i].codec       = GPX5_CODEC_DELTA;
        pipes[i].load_pct    = 33u;
        pipes[i].hilbert_mod = 3u;
        pipes[i].hilbert_rem = (uint8_t)i;
        pipes[i].lflags      = 0;
    }
}

/* ══════════════════════════════════════════════════════════
 * OPEN / CLOSE — read-side context
 * ══════════════════════════════════════════════════════════ */

typedef struct {
    Gpx5FileHdr   hdr;
    Gpx5PipeEntry pipes[3];
    Gpx5SetEntry  *sets;       /* hdr.n_sets entries */
    Gpx5LutEntry  *lut;        /* hdr.n_tiles entries, NULL if not warm */
    uint8_t       *raw;
    uint32_t       raw_sz;
    /* derived offsets (filled on open) */
    uint32_t       off_pipes;
    uint32_t       off_sets;
    uint32_t       off_lut;
    uint32_t       off_data;
} Gpx5File;

static inline int gpx5_open(const char *path, Gpx5File *gf) {
    memset(gf, 0, sizeof(*gf));
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long fsz = ftell(f); rewind(f);
    gf->raw    = (uint8_t *)malloc((size_t)fsz);
    gf->raw_sz = (uint32_t)fsz;
    if ((long)fread(gf->raw, 1, (size_t)fsz, f) != fsz) {
        fclose(f); free(gf->raw); return -1;
    }
    fclose(f);

    const uint8_t *p = gf->raw;
    if (p[0]!='G'||p[1]!='P'||p[2]!='X'||p[3]!='5') {
        free(gf->raw); return -1;
    }
    gpx5_hdr_read(p, &gf->hdr);

    /* pipe table: immediately after file header */
    gf->off_pipes = GPX5_FILE_HDR_SZ;
    for (int i = 0; i < 3; i++)
        gpx5_pipe_read(p + gf->off_pipes + (uint32_t)i * GPX5_PIPE_ENTRY_SZ,
                       &gf->pipes[i]);

    /* set table: after pipe table */
    gf->off_sets = gf->off_pipes + 3u * GPX5_PIPE_ENTRY_SZ;
    int ns = (int)gf->hdr.n_sets;
    gf->sets = (Gpx5SetEntry *)calloc((size_t)ns, sizeof(Gpx5SetEntry));
    for (int i = 0; i < ns; i++)
        gpx5_set_read(p + gf->off_sets + (uint32_t)i * GPX5_SET_ENTRY_SZ,
                      &gf->sets[i]);

    /* LUT table: after set table (present if GPX5_LUT_WARM set) */
    gf->off_lut  = gf->off_sets + (uint32_t)ns * GPX5_SET_ENTRY_SZ;
    gf->off_data = gf->off_lut;
    if (gf->hdr.flags & GPX5_LUT_WARM) {
        uint32_t nt = gf->hdr.n_tiles;
        gf->lut = (Gpx5LutEntry *)calloc(nt, sizeof(Gpx5LutEntry));
        for (uint32_t i = 0; i < nt; i++)
            gpx5_lut_read(p + gf->off_lut + i * GPX5_LUT_ENTRY_SZ,
                          &gf->lut[i]);
        gf->off_data = gf->off_lut + nt * GPX5_LUT_ENTRY_SZ;
    }
    return 0;
}

static inline void gpx5_close(Gpx5File *gf) {
    free(gf->sets);
    free(gf->lut);
    free(gf->raw);
    memset(gf, 0, sizeof(*gf));
}

/* ══════════════════════════════════════════════════════════
 * RANDOM ACCESS via LUT (O(1), requires GPX5_LUT_FROZEN)
 * ══════════════════════════════════════════════════════════ */

/*
 * gpx5_tile_invert_ptr — return pointer to invert-plane signature for tile_id.
 *   Returns NULL if LUT not warm or tile_id out of range.
 *   This is the O(1) access path — one array lookup, one pointer offset.
 */
static inline const uint8_t *gpx5_tile_invert_ptr(
        const Gpx5File *gf, uint16_t tile_id)
{
    if (!gf->lut || tile_id >= (uint16_t)gf->hdr.n_tiles) return NULL;
    uint32_t off = gf->lut[tile_id].invert_offset;
    if (off == 0xFFFFFFFFu || off >= gf->raw_sz) return NULL;
    return gf->raw + off;
}

/*
 * gpx5_tile_set_data — return pointer to set data for a tile (via LUT).
 *   Returns NULL if LUT not warm or tile not in a valid set.
 */
static inline const uint8_t *gpx5_tile_set_data(
        const Gpx5File *gf, uint16_t tile_id, uint32_t *out_sz)
{
    if (!gf->lut || tile_id >= (uint16_t)gf->hdr.n_tiles) return NULL;
    uint16_t sid = gf->lut[tile_id].set_id;
    if (sid >= gf->hdr.n_sets) return NULL;
    const Gpx5SetEntry *s = &gf->sets[sid];
    if (out_sz) *out_sz = s->data_size;
    return gf->raw + s->data_offset;
}
