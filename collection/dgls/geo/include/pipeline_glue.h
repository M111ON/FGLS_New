/*
 * pipeline_glue.h — Unified Pipeline Driver
 *
 * Chains: seed → chunk → bond → shell → geopixel → hamburger → .gpx5
 *         ↑ high entropy → Jet Bridge → residual_space (timeless)
 *
 * Each stage transforms data into the next stage's input format:
 *
 *   seed (12B) + input data
 *     → [geo_jump / flow_chunker] → chunks (64B each)
 *     → [bond_chain_build_chunk]  → BondChain (PoglsPiece per chunk)
 *     → [shell_container + geo_pixel] → GeoPixel tiles (RGB)
 *     → [hamburger_classify + encode] → Hamburger encoded stream
 *     → [gpx5_container] → .gpx5 file
 *
 *   At tick 11 boundary (Jet Bridge):
 *     → [jet_bridge_hop] → residual_space (bond-only, timeless)
 *
 * Pipeline state machine:
 *   IDLE → CHUNKING → BONDING → SHELLING → PIXELATING → HAMBURGER → GPX5
 *
 * All header-only. Uses static inline for stage functions.
 * Malloc used only for pipeline init / chunk buffer allocation.
 */

#ifndef PIPELINE_GLUE_H
#define PIPELINE_GLUE_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Internal pipeline stages ─────────────────────────── */
#include "geo_jump.h"
#include "geo_pixel.h"
#include "geo_shell.h"
#include "shell_container.h"
#include "shell_hop.h"
#include "skeleton_index.h"

/* ── Bond layer ───────────────────────────────────────── */
#include "pogls_bond.h"
#include "pogls_bond_chain.h"
#include "bond_to_geopixel.h"

/* ── Diamond / Hamburger / GPX ────────────────────────── */
#include "pogls_fold.h"
#include "diamond_shell_v2.h"
#include "diamond_shell_codec.h"
#include "hamburger_classify.h"
#include "hamburger_pipe.h"
#include "hamburger_encode.h"
#include "gpx5_container.h"

/* ── Chord multi-pointer access ───────────────────────── */
#include "geo_chord.h"

/* ── Decode: tile stream random-access ────────────────── */
#include "hb_tile_stream.h"

/* ── Jet Bridge + Residual Space ──────────────────────── */
#include "fibo_spine.h"
#include "residual_space.h"

/* ════════════════════════════════════════════════════════════
   CONSTANTS
   ════════════════════════════════════════════════════════════ */

#define PG_CHUNK_SZ         64u               /* DiamondBlock size   */
#define PG_MAX_CHUNKS       65536u            /* max input chunks     */
#define PG_MAX_PIPELINE_BUF (PG_MAX_CHUNKS * PG_CHUNK_SZ)
#define PG_GPX5_TW          27u               /* default tile cols    */
#define PG_GPX5_TH          1u                /* default tile rows    */
#define PG_SEED_BYTES       12u               /* geo_seed size        */

/* Pipeline stage enum */
#define PG_STAGE_IDLE       0u
#define PG_STAGE_CHUNKING   1u
#define PG_STAGE_BONDING    2u
#define PG_STAGE_SHELLING   3u
#define PG_STAGE_PIXELATING 4u
#define PG_STAGE_HAMBURGER  5u
#define PG_STAGE_GPX5       6u
#define PG_STAGE_DONE       7u
#define PG_STAGE_DECODE     8u

/* Pipeline flags */
#define PG_FLAG_NONE        0x00
#define PG_FLAG_HIGH_ENTROPY  0x01   /* data flagged for residual */
#define PG_FLAG_BRIDGE_ENABLE 0x02   /* enable Jet Bridge         */
#define PG_FLAG_RESIDUAL_ENABLE 0x04 /* enable residual space     */
#define PG_FLAG_VERBOSE     0x08     /* debug output              */

/* ════════════════════════════════════════════════════════════
   PIPELINE CONTEXT
   ════════════════════════════════════════════════════════════ */

/* Per-chunk metadata produced by chunking, consumed by later stages */
typedef struct {
    uint32_t    chunk_idx;              /* 0..n_chunks-1             */
    uint8_t     data[PG_CHUNK_SZ];      /* 64B chunk data            */
    uint8_t     shell_flag;             /* SHELL_FLAG_* from v2     */
    uint8_t     is_high_entropy;        /* flagged for residual     */
    uint8_t     ttype;                  /* GPX5_TTYPE_*             */
    uint8_t     _pad;
    uint32_t    tile_id;                /* tile id for shell/gpx    */
    uint64_t    seed;                   /* derived chunk seed       */
    PoglsPiece  piece;                  /* bond piece               */
    GeoPixel    pixel;                  /* encoded RGB pixel        */
} PGChunk;

/*
 * PGContext — full pipeline state
 *
 * Lifetime: caller allocates, pg_init(), pg_run(), pg_free()
 */
typedef struct {
    /* Input */
    const uint8_t  *input_data;         /* raw input bytes          */
    uint32_t        input_size;         /* input byte count         */

    /* Pipeline state */
    uint8_t         stage;              /* PG_STAGE_*               */
    uint8_t         flags;              /* PG_FLAG_*                */
    uint8_t         error;              /* != 0 on error            */

    /* Chunking */
    PGChunk        *chunks;             /* array [n_chunks]         */
    uint32_t        n_chunks;
    uint32_t        chunk_cursor;       /* current chunk index      */

    /* Bond chain */
    BondChain       bond_chain;

    /* Shell */
    ShellContainer  shell;

    /* Geo field state (for geo_field_encode path) */
    uint32_t        global_seed;
    uint32_t        face_max;           /* faces per level          */

    /* Fibo Spine + Jet Bridge */
    FiboSpine       spine;
    P5HRibcage      ribcage;

    /* Residual space */
    ResidualSpace   residual;

    /* Hamburger encode context */
    HbEncodeCtx    *hb_ctx;
    HbTileIn       *hb_tiles;

    /* Decode output */
    uint8_t       **decoded_tiles;  /* [n_chunks][HB_TILE_SZ_MAX] */

    /* Output */
    char            output_path[256];   /* .gpx5 output path        */
    uint32_t        output_size;        /* bytes written            */

    /* Stats */
    uint32_t        n_bonded;
    uint32_t        n_shelled;
    uint32_t        n_pixelated;
    uint32_t        n_bridged;
    uint32_t        n_hamburger;
} PGContext;

/* ════════════════════════════════════════════════════════════
   INIT / FREE
   ════════════════════════════════════════════════════════════ */

/*
 * pg_init() — initialise pipeline context
 *
 * input_data: raw bytes to process (caller owns, must outlive pipeline)
 * input_size: byte count
 * seed:       geo seed for deterministic addressing
 * flags:      PG_FLAG_* bitmask
 *
 * Returns 0 on success, -1 on allocation failure.
 */
static inline int pg_init(PGContext *pg,
                           const void *input_data,
                           uint32_t input_size,
                           uint32_t seed,
                           uint8_t flags)
{
    if (!pg || !input_data || input_size == 0) return -1;

    memset(pg, 0, sizeof(*pg));

    pg->input_data  = (const uint8_t *)input_data;
    pg->input_size  = input_size;
    pg->global_seed = seed;
    pg->flags       = flags;
    pg->stage       = PG_STAGE_IDLE;

    /* Compute chunk count */
    pg->n_chunks = (input_size + PG_CHUNK_SZ - 1) / PG_CHUNK_SZ;
    if (pg->n_chunks > PG_MAX_CHUNKS) {
        pg->error = 1;
        return -1;
    }

    /* Allocate chunk array */
    pg->chunks = (PGChunk *)calloc(pg->n_chunks, sizeof(PGChunk));
    if (!pg->chunks) { pg->error = 1; return -1; }

    /* Init bond chain */
    if (bond_chain_init(&pg->bond_chain, pg->n_chunks) != 0) {
        free(pg->chunks);
        pg->chunks = NULL;
        pg->error = 1;
        return -1;
    }

    /* Init shell */
    shell_init(&pg->shell, (uint8_t)(seed & 0xFu), (uint8_t)((seed >> 4) & 0xFu), seed);

    /* Init spine + ribcage */
    fibo_spine_init(&pg->spine);
    p5h_ribcage_init(&pg->ribcage, &pg->spine);

    /* Init residual space (if enabled) */
    if (flags & PG_FLAG_RESIDUAL_ENABLE) {
        if (rs_init(&pg->residual, 1024) != 0) {
            bond_chain_free(&pg->bond_chain);
            free(pg->chunks);
            pg->chunks = NULL;
            pg->error = 1;
            return -1;
        }
    }

    /* Init hamburger context (lazily in stage 5) */
    pg->hb_ctx   = NULL;
    pg->hb_tiles = NULL;

    pg->face_max = 12;  /* default dodeca faces */

    return 0;
}

/*
 * pg_free() — free pipeline resources
 */
static inline void pg_free(PGContext *pg) {
    if (!pg) return;

    bond_chain_free(&pg->bond_chain);
    p5h_ribcage_free(&pg->ribcage);
    free(pg->chunks);

    if (pg->flags & PG_FLAG_RESIDUAL_ENABLE) {
        rs_free(&pg->residual);
    }

    if (pg->hb_ctx) {
        hb_encode_free(pg->hb_ctx);
        free(pg->hb_ctx);
    }
    /* Free decoded tiles */
    if (pg->decoded_tiles) {
        for (uint32_t i = 0; i < pg->n_chunks; i++)
            free(pg->decoded_tiles[i]);
        free(pg->decoded_tiles);
        pg->decoded_tiles = NULL;
    }

    free(pg->hb_tiles);

    pg->chunks    = NULL;
    pg->hb_ctx    = NULL;
    pg->hb_tiles  = NULL;
}

/* ════════════════════════════════════════════════════════════
   STAGE 1: CHUNKING — split input into 64B blocks
   ════════════════════════════════════════════════════════════ */

/*
 * pg_stage_chunking() — copy input data into 64B chunks
 * Returns n_chunks, or 0 on error.
 */
static inline uint32_t pg_stage_chunking(PGContext *pg) {
    if (!pg || !pg->chunks) return 0;

    pg->stage = PG_STAGE_CHUNKING;

    uint32_t offset = 0;
    for (uint32_t i = 0; i < pg->n_chunks; i++) {
        PGChunk *ch = &pg->chunks[i];
        ch->chunk_idx = i;
        uint32_t sz = (offset + PG_CHUNK_SZ <= pg->input_size)
                      ? PG_CHUNK_SZ
                      : pg->input_size - offset;
        memcpy(ch->data, pg->input_data + offset, sz);
        if (sz < PG_CHUNK_SZ)
            memset(ch->data + sz, 0, PG_CHUNK_SZ - sz);
        ch->seed = pogls_fibo_addr((uint64_t)pg->global_seed ^ (uint64_t)i);
        ch->tile_id = i % pg->face_max;
        offset += PG_CHUNK_SZ;
    }

    return pg->n_chunks;
}

/* ════════════════════════════════════════════════════════════
   STAGE 2: BONDING — assign bond identity to each chunk
   ════════════════════════════════════════════════════════════ */

/*
 * pg_stage_bonding() — create bond pieces for all chunks
 * Bonds are deterministic from chunk index + seed.
 * Each chunk gets a unique PoglsPiece with intrinsic bond_key.
 *
 * Returns count of bonded chunks.
 *
 * High-entropy detection:
 *   Uses fold_fibo_intersect popcount as entropy proxy.
 *   Low popcount (< 4) = low structure = high entropy candidate.
 */
static inline uint32_t pg_stage_bonding(PGContext *pg) {
    if (!pg || !pg->chunks || pg->n_chunks == 0) return 0;

    pg->stage = PG_STAGE_BONDING;

    for (uint32_t i = 0; i < pg->n_chunks; i++) {
        PGChunk *ch = &pg->chunks[i];

        /* Create bond piece from chunk seed */
        uint8_t axis = (uint8_t)((ch->tile_id & 0x6) + 1);  /* 1..7 */
        ch->piece = pogls_make_piece(ch->seed, axis);

        /* Register in bond chain */
        bond_chain_build_chunk(&pg->bond_chain, i, ch->tile_id,
                               (uint8_t)(ch->tile_id & 0x7Fu));

        /* High-entropy detection via diamond shell classify */
        DiamondBlock db = fold_block_init(
            (uint8_t)(i & 0x7u),                       /* face_id     */
            (uint8_t)((i >> 3) & 0x3u),                /* engine_id   */
            (uint32_t)(ch->seed & 0xFFFFFFFFu),        /* vector_pos  */
            (uint8_t)((i >> 5) & 0x3u),                /* fibo_gear   */
            (uint8_t)((i >> 7) & 0x3u)                 /* quad_flags  */
        );
        uint64_t isect = fold_fibo_intersect(&db);
        uint8_t  pc    = (uint8_t)__builtin_popcountll(isect);
        ch->is_high_entropy = (pc < 4) ? 1 : 0;

        /* Classify via diamond_shell_v2 */
        ShellChunkResult sr;
        sr.isect_pc = pc;
        sr.flag     = (pc == 0) ? SHELL_FLAG_FLAT
                     : (pc <= SHELL_SPARSE_THRESH) ? SHELL_FLAG_SPARSE
                     : SHELL_FLAG_DENSE;
        ch->shell_flag = sr.flag;
        ch->ttype      = (pc == 0) ? GPX5_TTYPE_NOISE
                        : (pc <= 4) ? GPX5_TTYPE_FLAT
                        : (pc <= 30) ? GPX5_TTYPE_GRADIENT
                        : GPX5_TTYPE_EDGE;

        pg->n_bonded++;
    }

    /* Build hash table overlay for O(1) find */
    bond_chain_build_ht(&pg->bond_chain);

    return pg->n_bonded;
}

/* ════════════════════════════════════════════════════════════
   STAGE 3: SHELLING — map bond pieces to shell containers
   ════════════════════════════════════════════════════════════ */

/*
 * pg_stage_shelling() — assign each chunk to a shell container
 *
 * Uses the bond geo_key as a shell address:
 *   shell_id = geo_key % SHELL_N_ANCHORS (= 12)
 *   shell_addr is computed via shell_addr(bond.geo_key)
 *
 * Also records ribcage entries for Jet Bridge tracking.
 */
static inline uint32_t pg_stage_shelling(PGContext *pg) {
    if (!pg || !pg->chunks || pg->n_chunks == 0) return 0;

    pg->stage = PG_STAGE_SHELLING;

    for (uint32_t i = 0; i < pg->n_chunks; i++) {
        PGChunk *ch = &pg->chunks[i];

        /* Compute shell address via Chord */
        Chord chord;
        chord.seed       = (uint32_t)(ch->piece.geo_key & 0xFFFFFFFFu);
        chord.chord_id   = (uint8_t)(i % 4);
        chord.key_offset = (uint32_t)(ch->piece.geo_key >> 20);
        /* Map to shell layout — shell_addr computes deterministic address */
        shell_addr(&pg->shell, &chord);

        /* Record in ribcage for Jet Bridge tracking */
        p5h_ribcage_step(&pg->ribcage,
                          (uint16_t)(i % FS_PIPES),
                          pg->spine.global_tick,
                          pogls_bond_key(&ch->piece));

        pg->n_shelled++;
    }

    return pg->n_shelled;
}

/* ════════════════════════════════════════════════════════════
   STAGE 4: PIXELATING — encode bond pieces into GeoPixel RGB
   ════════════════════════════════════════════════════════════ */

/*
 * pg_stage_pixelating() — encode each chunk as a GeoPixel
 *
 * Uses bond_piece_fingerprint() for single-pixel visual identity.
 * Each chunk's bond piece maps to a unique RGB triple.
 */
static inline uint32_t pg_stage_pixelating(PGContext *pg) {
    if (!pg || !pg->chunks || pg->n_chunks == 0) return 0;

    pg->stage = PG_STAGE_PIXELATING;

    for (uint32_t i = 0; i < pg->n_chunks; i++) {
        PGChunk *ch = &pg->chunks[i];

        /* Encode bond piece as GeoPixel fingerprint */
        ch->pixel = bond_piece_fingerprint(&ch->piece);

        pg->n_pixelated++;
    }

    return pg->n_pixelated;
}

/* ════════════════════════════════════════════════════════════
   STAGE 5: HAMBURGER — run Hamburger codec on all tiles
   ════════════════════════════════════════════════════════════ */

/*
 * pg_stage_hamburger() — encode all chunks via Hamburger
 *
 * Sets up HbTileIn array, runs hb_encode_init + hb_encode_run.
 * The encoded stream will be written to .gpx5 in stage 6.
 *
 * Returns number of tiles encoded, or 0 on error.
 */
static inline uint32_t pg_stage_hamburger(PGContext *pg) {
    if (!pg || !pg->chunks || pg->n_chunks == 0) return 0;

    pg->stage = PG_STAGE_HAMBURGER;

    /* Allocate tile input array */
    pg->hb_tiles = (HbTileIn *)calloc(pg->n_chunks, sizeof(HbTileIn));
    if (!pg->hb_tiles) { pg->error = 1; return 0; }

    /* Fill tile inputs from chunks */
    for (uint32_t i = 0; i < pg->n_chunks; i++) {
        PGChunk *ch = &pg->chunks[i];
        pg->hb_tiles[i].data    = ch->data;
        pg->hb_tiles[i].sz      = PG_CHUNK_SZ;
        pg->hb_tiles[i].tile_id = (uint16_t)ch->tile_id;
        pg->hb_tiles[i].ttype   = ch->ttype;
    }

    /* Allocate and init encode context */
    pg->hb_ctx = (HbEncodeCtx *)calloc(1, sizeof(HbEncodeCtx));
    if (!pg->hb_ctx) { pg->error = 1; return 0; }

    /* Auto-configure pipes from tile types */
    {
        uint8_t *ttypes = (uint8_t *)calloc(pg->n_chunks, 1);
        if (ttypes) {
            for (uint32_t j = 0; j < pg->n_chunks; j++)
                ttypes[j] = pg->hb_tiles[j].ttype;
            hb_auto_pipes(ttypes, pg->n_chunks, pg->hb_ctx->pipes);
            free(ttypes);
        }
    }

    /* Save pipes before init (hb_encode_init memsets entire ctx first) */
    Gpx5PipeEntry saved_pipes[3];
    memcpy(saved_pipes, pg->hb_ctx->pipes, sizeof(saved_pipes));
    hb_encode_init(pg->hb_ctx, pg->global_seed, PG_GPX5_TW, PG_GPX5_TH,
                   saved_pipes, 0);
    pg->hb_ctx->n_tiles = pg->n_chunks;  /* override for dynamic count */

    /* Run encode */
    int result = hb_encode_run(pg->hb_ctx, pg->hb_tiles, pg->n_chunks);
    if (result != HB_OK) {
        pg->error = 1;
        return 0;
    }

    pg->n_hamburger = pg->hb_ctx->n_active;

    return pg->n_hamburger;
}

/* ════════════════════════════════════════════════════════════
   STAGE 6: GPX5 — write Hamburger output to .gpx5 file
   ════════════════════════════════════════════════════════════ */

/*
 * pg_stage_gpx5() — write encoded data to GPX5 container
 *
 * Uses hamburger_encode() which internally calls gpx5_write().
 * Output path constructed from seed: "pipeline_<seed>.gpx5"
 *
 * Returns file size, or 0 on error.
 */
static inline uint32_t pg_stage_gpx5(PGContext *pg) {
    if (!pg || !pg->hb_ctx) return 0;

    pg->stage = PG_STAGE_GPX5;

    /* Build output path */
    snprintf(pg->output_path, sizeof(pg->output_path),
             "pipeline_%08x.gpx5", pg->global_seed);

    /* Call hamburger_encode to write the .gpx5 file */
    int result = hamburger_encode(pg->output_path,
                                   pg->hb_ctx,
                                   pg->hb_tiles,
                                   pg->n_chunks);

    if (result != 0) {
        pg->error = 1;
        return 0;
    }

    /* Get file size */
    FILE *fp = fopen(pg->output_path, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        pg->output_size = (uint32_t)ftell(fp);
        fclose(fp);
    }

    pg->stage = PG_STAGE_DONE;
    return pg->output_size;
}

/* ════════════════════════════════════════════════════════════
   DECODE PATH
   ════════════════════════════════════════════════════════════ */

/*
 * pg_stage_decode() — decode .gpx5 file back to chunk array
 *
 * Reads .gpx5 at output_path → reconstructs tiles via hamburger_decode
 * → rebuilds internal chunk array with decoded data
 *
 * Returns number of tiles decoded, or 0 on error.
 */
static inline uint32_t pg_stage_decode(PGContext *pg) {
    if (!pg) return 0;

    pg->stage = PG_STAGE_DECODE;

    /* Open file to get tile count */
    Gpx5File gf;
    if (gpx5_open(pg->output_path, &gf) != 0) { pg->error = 1; return 0; }
    uint32_t n_tiles = gf.hdr.n_tiles;
    gpx5_close(&gf);

    /* Allocate chunk array matching the file */
    if (pg->chunks) { free(pg->chunks); pg->chunks = NULL; }
    pg->n_chunks = n_tiles;
    pg->chunks   = (PGChunk *)calloc(n_tiles, sizeof(PGChunk));
    if (!pg->chunks) { pg->error = 1; return 0; }

    /* Allocate tile pointers for hamburger_decode */
    uint8_t **tiles = (uint8_t **)calloc(n_tiles, sizeof(uint8_t *));
    if (!tiles) { free(pg->chunks); pg->chunks = NULL; pg->error = 1; return 0; }

    for (uint32_t i = 0; i < n_tiles; i++) {
        tiles[i] = (uint8_t *)calloc(1, HB_TILE_SZ_MAX);
        if (!tiles[i]) {
            for (uint32_t j = 0; j < i; j++) free(tiles[j]);
            free(tiles); free(pg->chunks); pg->chunks = NULL; pg->error = 1; return 0;
        }
    }

    /* Decode */
    if (pg->decoded_tiles) {
        for (uint32_t i = 0; i < pg->n_chunks; i++) free(pg->decoded_tiles[i]);
        free(pg->decoded_tiles);
    }
    pg->decoded_tiles = tiles;

    uint32_t out_n = 0, out_sz = 0;
    int r = hamburger_decode(pg->output_path, tiles, &out_n, &out_sz);
    if (r != HB_OK || out_n == 0) {
        pg->error = 1;
        return 0;
    }

    /* Copy decoded data into chunk array */
    for (uint32_t i = 0; i < out_n && i < pg->n_chunks; i++) {
        PGChunk *ch = &pg->chunks[i];
        ch->chunk_idx = i;
        uint32_t cp = out_sz < PG_CHUNK_SZ ? out_sz : PG_CHUNK_SZ;
        memcpy(ch->data, tiles[i], cp);
        ch->ttype = GPX5_TTYPE_FLAT;
        ch->tile_id = (uint16_t)i;
    }

    pg->n_chunks = out_n;
    return out_n;
}

/*
 * pg_read_tile() — random-access decode a single tile
 *
 * Uses hb_tile_stream() for O(1) seek + decode.
 * No full file decode — reads only the invert entry for tile_id.
 *
 * Returns number of bytes decoded, or 0 on error.
 */
static inline uint32_t pg_read_tile(PGContext *pg,
                                     uint32_t tile_id,
                                     uint8_t *out,
                                     uint32_t out_cap)
{
    if (!pg || !out || out_cap < HB_TILE_SZ_MAX) return 0;

    /* Build LUT entry for this tile */
    HbFiboLutEntry entry;
    entry.tile_id    = (uint16_t)tile_id;
    entry.invert_off = 0xFFFFFFFFu;  /* will be read from file */
    (void)entry;

    /* Use tile stream decoder */
    uint32_t bytes = 0;
    int r = hb_tile_stream(pg->output_path, &entry, out, out_cap, &bytes);

    if (r == HBT_ERR_ENTRY) {
        /* Clear tile — reconstruct from prediction */
        r = hb_tile_stream_clear(pg->output_path, &entry, out, out_cap, &bytes);
    }

    return (r == HBT_OK || r == HBT_ERR_ENTRY) ? bytes : 0;
}

/*
 * pg_read_tile_by_chord() — decode multiple tiles via chord resolution
 *
 * chord    : chord descriptor (root_addr, quality, flags, transpose)
 * shell_off: shell-level key_offset (0 if none)
 * tiles_out: caller-alloc [CHORD_MAX_NOTES][HB_TILE_SZ_MAX]
 *
 * Returns number of tiles successfully decoded.
 */
static inline uint32_t pg_read_tile_by_chord(PGContext *pg,
                                              const GeoChord *chord,
                                              uint32_t shell_off,
                                              uint8_t tiles_out[][HB_TILE_SZ_MAX])
{
    if (!pg || !chord || !tiles_out) return 0;

    GeoChordResolved r = geo_chord_press(chord, shell_off);
    uint32_t ok = 0;

    for (uint8_t i = 0; i < r.count; i++) {
        /* Map address → tile_id by modding into available tiles */
        uint32_t tile_id = r.addrs[i] % pg->n_chunks;

        uint32_t bytes = pg_read_tile(pg, tile_id, tiles_out[i], HB_TILE_SZ_MAX);
        if (bytes > 0) ok++;
    }

    return ok;
}

/* ════════════════════════════════════════════════════════════
   JET BRIDGE INTEGRATION
   ════════════════════════════════════════════════════════════ */

/*
 * pg_jet_bridge() — execute Jet Bridge for high-entropy chunks
 *
 * For each chunk marked is_high_entropy:
 *   1. Detect tick 11 boundary via spine
 *   2. Bridge data into residual_space via bond_key
 *   3. Freeze in ribcage (P5H barrier)
 *   4. Mark chunk as bridged
 *
 * Returns count of bridged chunks.
 */
static inline uint32_t pg_jet_bridge(PGContext *pg) {
    if (!pg || !pg->chunks) return 0;
    if (!(pg->flags & PG_FLAG_BRIDGE_ENABLE)) return 0;

    uint32_t bridged = 0;

    for (uint32_t i = 0; i < pg->n_chunks; i++) {
        PGChunk *ch = &pg->chunks[i];
        if (!ch->is_high_entropy) continue;

        /* Advance spine tick */
        fibo_spine_tick(&pg->spine);

        /* Check if Jet Bridge should fire (tick 11) */
        if (pg->spine.global_tick == FS_JET_BRIDGE_TICK) {
            if (pg->flags & PG_FLAG_RESIDUAL_ENABLE) {
                rs_freeze(&pg->residual, &ch->piece,
                           ch->data, PG_CHUNK_SZ, 1);
            }

            /* Execute the hop (data already stored via rs_freeze above) */
            jet_bridge_hop(&pg->spine,
                           (uint16_t)(i % FS_PIPES),
                           ch->data, PG_CHUNK_SZ,
                           NULL);

            /* Freeze in ribcage (P5H barrier) */
            p5h_freeze_at_tick12(&pg->ribcage);

            bridged++;
            pg->n_bridged++;
        }
    }

    return bridged;
}

/* ════════════════════════════════════════════════════════════
   FULL PIPELINE RUN
   ════════════════════════════════════════════════════════════ */

/*
 * pg_run_full() — execute all 6 pipeline stages + Jet Bridge
 *
 * Stages:
 *   1. CHUNKING   — split input into 64B blocks
 *   2. BONDING    — assign bond identity + detect entropy
 *   3. SHELLING   — map to shell containers
 *   4. PIXELATING — encode as GeoPixel RGB
 *   5. HAMBURGER  — run Hamburger codec
 *   6. GPX5       — write output file
 *   + Jet Bridge  — bridge high-entropy chunks at tick 11
 *
 * Returns 0 on success, -1 on any stage failure.
 */
static inline int pg_run_full(PGContext *pg) {
    if (!pg) return -1;

    /* Stage 1: Chunking */
    if (pg_stage_chunking(pg) == 0) { pg->error = 1; return -1; }

    /* Stage 2: Bonding */
    if (pg_stage_bonding(pg) == 0) { pg->error = 1; return -1; }

    /* Stage 3: Shelling */
    if (pg_stage_shelling(pg) == 0) { pg->error = 1; return -1; }

    /* Stage 4: Pixelating */
    if (pg_stage_pixelating(pg) == 0) { pg->error = 1; return -1; }

    /* Jet Bridge: bridge high-entropy chunks before hamburger */
    if (pg->flags & PG_FLAG_BRIDGE_ENABLE) {
        pg_jet_bridge(pg);
    }

    /* Stage 5: Hamburger */
    if (pg_stage_hamburger(pg) == 0) { pg->error = 1; return -1; }

    /* Stage 6: GPX5 */
    if (pg_stage_gpx5(pg) == 0) { pg->error = 1; return -1; }

    return 0;
}

/*
 * pg_run_decode() — decode .gpx5 back into chunk array
 *
 * Runs pg_stage_decode after pg_run_full, returning number of
 * decoded tiles. Caller must have called pg_run_full first.
 *
 * Returns number of tiles decoded, or 0 on error.
 */
static inline uint32_t pg_run_decode(PGContext *pg) {
    if (!pg || pg->stage != PG_STAGE_DONE) return 0;
    return pg_stage_decode(pg);
}

/* ════════════════════════════════════════════════════════════
   STAGE NAMES
   ════════════════════════════════════════════════════════════ */

static inline const char *pg_stage_name(uint8_t stage) {
    switch (stage) {
        case PG_STAGE_IDLE:       return "IDLE";
        case PG_STAGE_CHUNKING:   return "CHUNKING";
        case PG_STAGE_BONDING:    return "BONDING";
        case PG_STAGE_SHELLING:   return "SHELLING";
        case PG_STAGE_PIXELATING: return "PIXELATING";
        case PG_STAGE_HAMBURGER:  return "HAMBURGER";
        case PG_STAGE_GPX5:       return "GPX5";
        case PG_STAGE_DONE:       return "DONE";
        case PG_STAGE_DECODE:     return "DECODE";
        default:                  return "UNKNOWN";
    }
}

/* ════════════════════════════════════════════════════════════
   STATS
   ════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t   input_size;
    uint32_t   n_chunks;
    uint32_t   n_bonded;
    uint32_t   n_shelled;
    uint32_t   n_pixelated;
    uint32_t   n_bridged;
    uint32_t   n_hamburger;
    uint32_t   n_decoded;
    uint32_t   output_size;
    uint8_t    stage;
    uint8_t    error;
    const char *stage_name;
    double     compression_ratio;
} PGStats;

static inline PGStats pg_stats(const PGContext *pg) {
    PGStats s;
    memset(&s, 0, sizeof(s));
    if (!pg) return s;

    s.input_size     = pg->input_size;
    s.n_chunks       = pg->n_chunks;
    s.n_bonded       = pg->n_bonded;
    s.n_shelled      = pg->n_shelled;
    s.n_pixelated    = pg->n_pixelated;
    s.n_bridged      = pg->n_bridged;
    s.n_hamburger    = pg->n_hamburger;
    s.n_decoded      = pg->n_chunks;  /* after decode, n_chunks = decoded tiles */
    s.output_size    = pg->output_size;
    s.stage          = pg->stage;
    s.error          = pg->error;
    s.stage_name     = pg_stage_name(pg->stage);
    s.compression_ratio = pg->output_size > 0
                          ? (double)pg->input_size / (double)pg->output_size
                          : 0.0;

    return s;
}

#endif /* PIPELINE_GLUE_H */
