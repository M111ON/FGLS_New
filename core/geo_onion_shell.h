/*
 * geo_onion_shell.h — Onion Shell Hex Indexer
 * ═════════════════════════════════════════════
 *
 * Shell geometry:
 *   shell 0  : header only (face_pair_table, metadata) — stored, tiny
 *   shell N  : 6N+1 hex cells, virtual (not stored)
 *              content = chunk sequence in Hilbert order
 *              materialized on decode/access only
 *
 * Hex coordinate system: axial (q, r)
 *   shell N boundary = all cells where max(|q|,|r|,|q+r|) == N
 *   total cells up to shell N = 3N(N+1)+1
 *
 * Frustum face pairing (anti-drift):
 *   6 hex sectors map to 6 frustum faces
 *   sector s pairs with sector (s+3)%6  (opposite face)
 *   pair table stored in shell 0 header
 *   Hilbert respects sector boundary — no cross-sector drift
 *
 * Hilbert on hex: we use axial→cube→linear flattening
 *   cube coords: x=q, z=r, y=-(q+r)
 *   linearize per shell ring using sector-ordered traversal
 *   deterministic: same seed → same order always
 *
 * Scale law: shell N holds chunks at scale 16^N relative to shell 0
 *   shell 0: 1 unit (header)
 *   shell 1: 6×1+1=7 cells, each = 16× shell 0 chunk
 *   shell 2: 6×2+1=13 cells, each = 16× shell 1 = 256× shell 0
 *   (virtual shells do NOT store 16^N entries — they store chunk_idx
 *    pointing into the original flat chunk sequence)
 *
 * Usage:
 *   OnionShell s;
 *   onion_init(&s, n_chunks, hilbert_seed);
 *   uint32_t chunk_idx = onion_chunk_at(&s, shell, cell);
 *   onion_header_write(&s, buf);   // serialize shell 0
 *   onion_header_read(&s, buf);    // deserialize shell 0
 */

#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ── constants ──────────────────────────────────────────────────── */
#define ONION_FACE_COUNT   6
#define ONION_HDR_MAGIC    0x4F4E494F   /* "ONIO" */
#define ONION_HDR_BYTES    64           /* fits one DiamondBlock */
#define ONION_MAX_SHELL    16           /* virtual shells 1..16 */

/* ── face pair table ─────────────────────────────────────────────
 * sector s (0-5) pairs with sector (s+3)%6
 * stored explicitly so decoder needs no convention knowledge
 */
typedef struct {
    uint8_t pair[ONION_FACE_COUNT];   /* pair[s] = paired sector index */
    uint8_t scale_log2;               /* log2(scale per shell) = 4 (×16) */
    uint8_t shell_count;              /* number of virtual shells */
    uint16_t reserved;
} FacePairTable;

/* ── shell 0 header (64 bytes = 1 DiamondBlock) ──────────────────
 * layout:
 *   [0..3]  magic        u32
 *   [4]     shell_count  u8
 *   [5]     scale_log2   u8   (always 4 for ×16)
 *   [6..7]  reserved     u16
 *   [8..13] pair[6]      u8×6
 *   [14..15] reserved    u16
 *   [16..19] n_chunks    u32  (total chunk sequence length)
 *   [20..27] hilbert_seed u64
 *   [28..35] xxh64       u64  (hash of bytes 0..27)
 *   [36..63] zero pad
 */
typedef struct {
    uint32_t      magic;
    FacePairTable fpt;
    uint32_t      n_chunks;
    uint64_t      hilbert_seed;
    uint64_t      checksum;
} OnionHeader;

/* ── OnionShell context ───────────────────────────────────────────*/
typedef struct {
    OnionHeader   hdr;
    uint32_t     *chunk_seq;   /* flat chunk index array [n_chunks] */
    uint32_t      n_chunks;
} OnionShell;

/* ══════════════════════════════════════════════════════════════════
 * HEX MATH
 * ══════════════════════════════════════════════════════════════════
 * axial (q,r) → shell = max(|q|,|r|,|q+r|)
 * cells in shell N ring = 6N  (N>0), 1 for N=0
 * total cells up to shell N = 3N(N+1)+1
 *
 * Sector assignment (which frustum face owns a cell):
 *   The hex ring at shell N has 6 sides of N cells each.
 *   Side 0 (sector 0): top-right    q=N,  r=-N..0
 *   Side 1 (sector 1): right        q=N..0, r=-(q) ... r=N-q
 *   ... (6 sides, clockwise)
 *   We enumerate ring cells clockwise, assign sector = cell_idx/N
 */

static inline int32_t _abs32(int32_t x){ return x<0?-x:x; }
static inline int32_t _max3(int32_t a,int32_t b,int32_t c){
    return a>b?(a>c?a:c):(b>c?b:c);
}

/* shell number of axial cell (q,r) */
static inline uint32_t hex_shell(int32_t q, int32_t r){
    return (uint32_t)_max3(_abs32(q),_abs32(r),_abs32(q+r));
}

/* total cells from shell 0 through shell N (inclusive) */
static inline uint32_t hex_total_cells(uint32_t N){
    return 3*N*(N+1)+1;
}

/* enumerate ring cells of shell N in clockwise order
 * returns count=6N (N>0), 1 (N=0)
 * out_q, out_r: caller provides arrays of size 6N */
static inline uint32_t hex_ring_enum(uint32_t N,
                                      int32_t *out_q, int32_t *out_r){
    if(N==0){ out_q[0]=0; out_r[0]=0; return 1; }
    /* start at (N, 0), walk 6 axial directions (standard hex ring) */
    static const int32_t dir_q[6]={-1,-1, 0, 1, 1, 0};
    static const int32_t dir_r[6]={ 1, 0,-1,-1, 0, 1};
    int32_t q=(int32_t)N, r=0;
    uint32_t idx=0;
    for(int side=0;side<6;side++){
        for(uint32_t step=0;step<N;step++){
            out_q[idx]=q; out_r[idx]=r; idx++;
            q+=dir_q[side]; r+=dir_r[side];
        }
    }
    return idx;  /* = 6N */
}

/* sector of a ring cell (0-5) given its position in the ring enumeration */
static inline uint32_t hex_cell_sector(uint32_t ring_pos, uint32_t N){
    if(N==0) return 0;
    return ring_pos / N;   /* 6N cells / 6 sectors = N cells per sector */
}

/* ══════════════════════════════════════════════════════════════════
 * HILBERT-LIKE TRAVERSAL (deterministic, sector-respecting)
 * ══════════════════════════════════════════════════════════════════
 * We use a simple sector-interleaved Hilbert approximation:
 *   For shell N, cells are ordered by sector first (anti-drift),
 *   within sector by a 1D Hilbert-folded index (alternating direction
 *   per shell level to maintain locality).
 *
 * "Hilbert" here = zigzag within sector, flip direction every shell.
 * This is the minimal deterministic neighbor-preserving order
 * that respects face pair boundaries and fits in ~20 lines of C.
 *
 * seed XORs into the starting direction per shell — same seed
 * always produces same order (deterministic reconstruct).
 */
static inline uint32_t _hilbert_cell_order(uint32_t shell,
                                            uint32_t sector,
                                            uint32_t pos_in_sector,
                                            uint32_t N,
                                            uint64_t seed){
    uint32_t base = sector * N;
    /* mix seed with shell+sector to avoid seed=0 producing same as any other */
    uint64_t mixed = (seed ^ (uint64_t)(shell * 2654435761ULL)
                            ^ (uint64_t)(sector * 1234567891ULL));
    uint32_t flip = (uint32_t)((mixed >> sector) ^ (mixed >> (sector+32))) & 1;
    uint32_t local = flip ? (N - 1 - pos_in_sector) : pos_in_sector;
    return base + local;
}

/* ══════════════════════════════════════════════════════════════════
 * CHUNK MAPPING
 * chunk_idx for (shell, cell_in_ring) =
 *   hilbert_order_within_shell → maps to chunk_seq index
 *   wraps modulo n_chunks (virtual shells reuse sequence cyclically)
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t onion_chunk_at(const OnionShell *s,
                                       uint32_t shell, uint32_t cell_in_ring){
    if(s->n_chunks == 0) return 0;
    uint32_t N = shell;
    uint32_t ring_cells = (N==0)?1:6*N;
    if(cell_in_ring >= ring_cells) cell_in_ring = ring_cells-1;

    uint32_t sector   = hex_cell_sector(cell_in_ring, N==0?1:N);
    uint32_t pos_in_s = (N==0)?0:(cell_in_ring % N);

    uint32_t h_order  = (N==0)?0:_hilbert_cell_order(
                            shell, sector, pos_in_s, N,
                            s->hdr.hilbert_seed);

    /* global offset = total cells before this shell + h_order */
    uint32_t global_off = (N==0)?0:(hex_total_cells(N-1) + h_order);

    /* map into chunk sequence (virtual shells wrap) */
    return s->chunk_seq[global_off % s->n_chunks];
}

/* ══════════════════════════════════════════════════════════════════
 * HASH (same xxh64 family used throughout pipeline)
 * ══════════════════════════════════════════════════════════════════ */
#define _OH1 0x9e3779b97f4a7c15ULL
#define _OH2 0x6c62272e07bb0142ULL
static inline uint64_t _orot(uint64_t x,int r){return(x<<r)|(x>>(64-r));}
static inline uint64_t _ohu(uint64_t a,uint64_t w){
    a^=(w*_OH1);a=_orot(a,27);a=a*_OH2+0x94d049bb133111ebULL;return a;}
static uint64_t _oxxh(const uint8_t*d,size_t n){
    uint64_t a=_OH1^n; size_t i=0;
    for(;i+8<=n;i+=8){uint64_t w;memcpy(&w,d+i,8);a=_ohu(a,w);}
    if(i<n){uint64_t t=0;memcpy(&t,d+i,n-i);a=_ohu(a,t);}
    a^=(a>>33);a*=_OH1;a^=(a>>29);a*=_OH2;a^=(a>>32);return a;}

/* ══════════════════════════════════════════════════════════════════
 * HEADER SERIALIZE / DESERIALIZE (shell 0, 64 bytes)
 * ══════════════════════════════════════════════════════════════════ */
static inline void onion_header_write(const OnionShell *s, uint8_t buf[64]){
    memset(buf,0,64);
    uint32_t magic = ONION_HDR_MAGIC;
    memcpy(buf+0,  &magic,                    4);
    buf[4] = s->hdr.fpt.shell_count;
    buf[5] = s->hdr.fpt.scale_log2;
    memcpy(buf+8,  s->hdr.fpt.pair,           6);
    memcpy(buf+16, &s->hdr.n_chunks,          4);
    memcpy(buf+20, &s->hdr.hilbert_seed,      8);
    /* checksum covers bytes 0..27 */
    uint64_t ck = _oxxh(buf, 28);
    memcpy(buf+28, &ck, 8);
}

static inline int onion_header_read(OnionShell *s, const uint8_t buf[64]){
    uint32_t magic; memcpy(&magic, buf+0, 4);
    if(magic != ONION_HDR_MAGIC) return -1;
    /* verify checksum */
    uint64_t stored; memcpy(&stored, buf+28, 8);
    uint64_t computed = _oxxh(buf, 28);
    if(stored != computed) return -2;

    s->hdr.fpt.shell_count = buf[4];
    s->hdr.fpt.scale_log2  = buf[5];
    memcpy(s->hdr.fpt.pair, buf+8, 6);
    memcpy(&s->hdr.n_chunks,      buf+16, 4);
    memcpy(&s->hdr.hilbert_seed,  buf+20, 8);
    s->hdr.checksum = stored;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * INIT
 * ══════════════════════════════════════════════════════════════════ */

/* default face pair table: sector s ↔ sector (s+3)%6 */
static inline void _default_face_pairs(FacePairTable *fpt,
                                        uint8_t shell_count){
    for(int i=0;i<6;i++) fpt->pair[i]=(uint8_t)((i+3)%6);
    fpt->scale_log2  = 4;   /* ×16 per shell */
    fpt->shell_count = shell_count;
    fpt->reserved    = 0;
}

static inline int onion_init(OnionShell *s,
                              uint32_t n_chunks,
                              uint64_t hilbert_seed,
                              uint8_t  shell_count){
    memset(s, 0, sizeof(*s));
    s->n_chunks = n_chunks;
    s->hdr.magic        = ONION_HDR_MAGIC;
    s->hdr.n_chunks     = n_chunks;
    s->hdr.hilbert_seed = hilbert_seed;
    _default_face_pairs(&s->hdr.fpt, shell_count);

    /* build chunk_seq: identity mapping (chunk i → index i)
     * caller can reorder for custom spatial layout */
    s->chunk_seq = (uint32_t*)malloc(n_chunks * sizeof(uint32_t));
    if(!s->chunk_seq) return -1;
    for(uint32_t i=0;i<n_chunks;i++) s->chunk_seq[i]=i;
    return 0;
}

static inline void onion_free(OnionShell *s){
    free(s->chunk_seq);
    s->chunk_seq = NULL;
    s->n_chunks  = 0;
}

/* ══════════════════════════════════════════════════════════════════
 * QUERY HELPERS
 * ══════════════════════════════════════════════════════════════════ */

/* given chunk_idx, find which (shell, cell) it lives in
 * returns first match (shell 1 priority — shell 0 is header) */
static inline int onion_locate(const OnionShell *s, uint32_t chunk_idx,
                                uint32_t *out_shell, uint32_t *out_cell){
    uint8_t max_shell = s->hdr.fpt.shell_count;
    for(uint32_t sh=1; sh<=(uint32_t)max_shell; sh++){
        uint32_t cells = 6*sh;
        for(uint32_t c=0; c<cells; c++){
            if(onion_chunk_at(s, sh, c) == chunk_idx){
                *out_shell=sh; *out_cell=c; return 0;
            }
        }
    }
    return -1;
}

/* paired sector for sector s */
static inline uint32_t onion_paired_sector(const OnionShell *s, uint32_t sector){
    return s->hdr.fpt.pair[sector % ONION_FACE_COUNT];
}

/* scale factor for shell N = 16^N (as uint64, careful for N>15) */
static inline uint64_t onion_scale(uint32_t shell){
    uint64_t r=1;
    for(uint32_t i=0;i<shell;i++) r<<=4;  /* ×16 = <<4 */
    return r;
}
