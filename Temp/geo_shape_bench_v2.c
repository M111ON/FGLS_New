/*
 * geo_shape_bench_v2.c — File-as-Shape Placement Benchmark v2
 * ════════════════════════════════════════════════════════════
 * Model: file has geometry derived from metadata.
 *   place(meta)  → geometric slot  (no mutation)
 *   retrieve(shape_query) → file   (no path needed)
 *   delete       = reserved_mask on coset (structural silence)
 *
 * compile: gcc -O2 -o bench_v2 geo_shape_bench_v2.c && ./bench_v2
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ── Sacred constants ── */
#define TRIT_MOD    27u
#define SPOKE_MOD    6u
#define COSET_MOD    9u
#define LETTER_MOD  26u
#define FIBO_MOD   144u
#define SHAPE_SLOTS (TRIT_MOD * SPOKE_MOD * COSET_MOD)  /* 1,458 */
#define CHAIN_MAX   16u   /* v2: 16 (was 8) — deeper ring per slot */

/* ── Bench sizes ── */
#define N_SMALL    10000u
#define N_LARGE   100000u
#define N_STRESS   20000u   /* slot saturation test */

/* ════════════════════════════════════════
   TYPES
   ════════════════════════════════════════ */
typedef struct {
    uint8_t trit, spoke, coset, letter, fibo;
} FileShape;

typedef struct {
    uint64_t size;
    uint8_t  type;
    uint64_t mtime;
    uint8_t  name_first;
    uint64_t content_sig;
} FileMeta;

typedef struct {
    uint64_t content_sig;
    FileMeta meta;
    uint8_t  valid;  /* 0=empty 1=live 3=tombstone */
} ShapeEntry;

typedef struct {
    ShapeEntry chain[CHAIN_MAX];
    uint8_t    count, head, reserved;
} ShapeSlot;

typedef struct {
    ShapeSlot  slots[SHAPE_SLOTS];
    uint16_t   reserved_mask;
    uint32_t   placed, retrieved, hits, misses, deleted, overflow;
} ShapeStore;

typedef struct {
    uint8_t trit, spoke, coset, letter;  /* 0xFF = wildcard */
} ShapeQuery;

/* ── Path baseline ── */
#define PATH_BUCKETS 4096u
typedef struct { uint64_t path_hash, content_sig; uint8_t valid; } PathEntry;
typedef struct { PathEntry e[PATH_BUCKETS]; uint32_t placed, hits, collisions; } PathStore;

/* ════════════════════════════════════════
   SHAPE DERIVATION
   v2: stronger trit mixing — avoids low-trit clustering
   ════════════════════════════════════════ */
static inline FileShape shape_from_meta(const FileMeta *m) {
    /* log2(size) → 0..63, then mix content_sig to spread same-size files */
    uint8_t  sz_bits = 0;
    uint64_t sz = m->size > 0 ? m->size : 1;
    while (sz > 1) { sz >>= 1; sz_bits++; }

    /* v2: Fibonacci-mix on trit seed — avoids modular clustering */
    uint64_t ts = (uint64_t)sz_bits * 1597ULL + (m->content_sig ^ (m->content_sig >> 17));
    ts ^= ts >> 13; ts ^= ts << 7; ts ^= ts >> 17;

    uint8_t spoke = m->type % SPOKE_MOD;
    uint8_t coset = (uint8_t)((m->mtime / 3600) % COSET_MOD);
    uint8_t ltr   = (m->name_first >= 'a' && m->name_first <= 'z')
                    ? (uint8_t)(m->name_first - 'a')
                    : (m->name_first >= 'A' && m->name_first <= 'Z')
                    ? (uint8_t)(m->name_first - 'A')
                    : (uint8_t)(m->name_first % LETTER_MOD);

    FileShape s;
    s.trit   = (uint8_t)(ts % TRIT_MOD);
    s.spoke  = spoke;
    s.coset  = coset;
    s.letter = ltr % LETTER_MOD;
    s.fibo   = (uint8_t)(m->content_sig % FIBO_MOD);
    return s;
}

static inline uint32_t shape_slot(const FileShape *s) {
    return (uint32_t)s->trit  * (SPOKE_MOD * COSET_MOD)
         + (uint32_t)s->spoke * COSET_MOD
         + (uint32_t)s->coset;
}

static inline void shape_store_init(ShapeStore *st) { memset(st, 0, sizeof(*st)); }

/* ── place ── */
static inline int shape_place(ShapeStore *st, const FileMeta *m) {
    FileShape s   = shape_from_meta(m);
    uint32_t  idx = shape_slot(&s);
    if (st->reserved_mask & (1u << s.coset)) return -1;
    ShapeSlot *sl = &st->slots[idx];
    if (sl->reserved) return -1;
    for (uint8_t i = 0; i < CHAIN_MAX; i++) {
        uint8_t pos = (sl->head + i) % CHAIN_MAX;
        ShapeEntry *e = &sl->chain[pos];
        if (e->valid == 0 || e->valid == 3) {
            e->content_sig = m->content_sig;
            e->meta        = *m;
            e->valid       = 1;
            sl->count++;
            sl->head = (uint8_t)((pos + 1) % CHAIN_MAX);
            st->placed++;
            return 0;
        }
    }
    st->overflow++;
    return -2;
}

/* ── retrieve (returns first match; wildcard on trit/letter) ── */
static inline int shape_retrieve(ShapeStore *st, const ShapeQuery *q,
                                  uint64_t *sig_out) {
    uint8_t t0 = (q->trit   == 0xFF) ? 0 : q->trit;
    uint8_t t1 = (q->trit   == 0xFF) ? TRIT_MOD : (uint8_t)(q->trit + 1);
    uint8_t la = (q->letter == 0xFF);
    st->retrieved++;
    for (uint8_t t = t0; t < t1; t++) {
        if (st->reserved_mask & (1u << q->coset)) { st->misses++; return 0; }
        uint32_t idx = (uint32_t)t * (SPOKE_MOD * COSET_MOD)
                     + (uint32_t)q->spoke * COSET_MOD
                     + (uint32_t)q->coset;
        ShapeSlot *sl = &st->slots[idx];
        for (uint8_t i = 0; i < CHAIN_MAX; i++) {
            ShapeEntry *e = &sl->chain[i];
            if (e->valid != 1) continue;
            uint8_t el = (e->meta.name_first >= 'a')
                         ? (uint8_t)(e->meta.name_first - 'a')
                         : (uint8_t)(e->meta.name_first - 'A');
            if (!la && (el % LETTER_MOD) != q->letter) continue;
            *sig_out = e->content_sig;
            st->hits++;
            return 1;
        }
    }
    st->misses++;
    return 0;
}

/* ── delete: coset silence ── */
static inline void shape_delete_coset(ShapeStore *st, uint8_t coset) {
    st->reserved_mask |= (uint16_t)(1u << coset);
    for (uint8_t t = 0; t < TRIT_MOD; t++)
        for (uint8_t sp = 0; sp < SPOKE_MOD; sp++) {
            uint32_t  idx = (uint32_t)t * (SPOKE_MOD * COSET_MOD)
                          + (uint32_t)sp * COSET_MOD + coset;
            ShapeSlot *sl = &st->slots[idx];
            sl->reserved = 1;
            for (uint8_t i = 0; i < CHAIN_MAX; i++)
                if (sl->chain[i].valid == 1) sl->chain[i].valid = 3;
            sl->count = 0;
        }
    st->deleted++;
}

/* ── path baseline ── */
static inline void path_init(PathStore *ps) { memset(ps, 0, sizeof(*ps)); }
static inline uint32_t path_hash_fn(uint64_t h) {
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
    return (uint32_t)(h % PATH_BUCKETS);
}
static inline void path_place(PathStore *ps, uint64_t ph, uint64_t sig) {
    uint32_t b = path_hash_fn(ph);
    for (uint32_t i = 0; i < PATH_BUCKETS; i++) {
        uint32_t bi = (b + i) % PATH_BUCKETS;
        if (!ps->e[bi].valid) {
            ps->e[bi] = (PathEntry){ ph, sig, 1 };
            ps->placed++;
            if (i) ps->collisions++;
            return;
        }
    }
}
static inline int path_retrieve(PathStore *ps, uint64_t ph, uint64_t *out) {
    uint32_t b = path_hash_fn(ph);
    for (uint32_t i = 0; i < PATH_BUCKETS; i++) {
        uint32_t bi = (b + i) % PATH_BUCKETS;
        if (!ps->e[bi].valid) return 0;
        if (ps->e[bi].path_hash == ph) { *out = ps->e[bi].content_sig; ps->hits++; return 1; }
    }
    return 0;
}

/* ════════════════════════════════════════
   UTILITIES
   ════════════════════════════════════════ */
static uint64_t lcg(uint64_t *s) {
    *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
    return *s;
}
static const uint64_t SIZES[]  = { 512,1024,4096,8192,65536,131072,1048576,4194304 };
static const char     NAMES[]  = "abcdefghijklmnopqrstuvwxyz";

static inline FileMeta gen_meta(uint64_t *seed) {
    return (FileMeta){
        .size        = SIZES[lcg(seed) % 8],
        .type        = (uint8_t)(lcg(seed) % SPOKE_MOD),
        .mtime       = lcg(seed) % (24 * 3600),
        .name_first  = NAMES[lcg(seed) % 26],
        .content_sig = lcg(seed)
    };
}

static inline uint64_t ns_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

/* slot occupancy stats */
static void slot_stats(const ShapeStore *st,
                        uint32_t *used_out, uint32_t *live_out,
                        uint32_t *max_chain_out) {
    uint32_t used = 0, live = 0, mx = 0;
    for (uint32_t i = 0; i < SHAPE_SLOTS; i++) {
        const ShapeSlot *sl = &st->slots[i];
        uint32_t c = 0;
        for (uint8_t j = 0; j < CHAIN_MAX; j++)
            if (sl->chain[j].valid == 1) { live++; c++; }
        if (c > 0) used++;
        if (c > mx) mx = c;
    }
    *used_out = used; *live_out = live; *max_chain_out = mx;
}

/* ════════════════════════════════════════
   CORRECTNESS
   ════════════════════════════════════════ */
static int run_correctness(void) {
    static ShapeStore st;
    shape_store_init(&st);
    int ok = 1;

#define ASSERT(cond, msg) do { if(!(cond)){ printf("[FAIL] %s\n", msg); ok=0; } } while(0)

    /* 1. place → retrieve exact */
    FileMeta m = {4096,1,3600,'r',0xDEADBEEF};
    shape_place(&st, &m);
    FileShape s = shape_from_meta(&m);
    ShapeQuery q = { s.trit, s.spoke, s.coset, s.letter };
    uint64_t out = 0;
    ASSERT(shape_retrieve(&st, &q, &out) && out == 0xDEADBEEF, "place/retrieve");

    /* 2. shape determinism */
    FileShape s2 = shape_from_meta(&m);
    ASSERT(s.trit==s2.trit && s.spoke==s2.spoke && s.coset==s2.coset, "determinism");

    /* 3. delete coset → silence */
    shape_delete_coset(&st, s.coset);
    ASSERT(!shape_retrieve(&st, &q, &out), "coset silence");

    /* 4. wildcard trit+letter */
    shape_store_init(&st);
    FileMeta m2 = {65536,2,7200,'k',0xCAFEBABE};
    shape_place(&st, &m2);
    FileShape s3 = shape_from_meta(&m2);
    ShapeQuery qw = { 0xFF, s3.spoke, s3.coset, 0xFF };
    ASSERT(shape_retrieve(&st, &qw, &out), "wildcard");

    /* 5. two same-shape files → both in chain
       NOTE: content_sig feeds trit mixing in v2, so same shape requires
       identical meta. Use same sig — they share a slot, chain grows.       */
    shape_store_init(&st);
    FileMeta ma={4096,1,3600,'a',0xAAAA}, mb={4096,1,3600,'a',0xAAAA};
    mb.content_sig = ma.content_sig;  /* force identical trit → same slot */
    shape_place(&st,&ma); shape_place(&st,&mb);
    FileShape sa = shape_from_meta(&ma);
    ASSERT(st.slots[shape_slot(&sa)].count >= 2, "chain count");

    /* 6. chain overflow — fill same slot beyond CHAIN_MAX */
    shape_store_init(&st);
    FileMeta mf = {4096,1,3600,'z',0xDEAD};   /* fixed sig → fixed trit */
    int overflow_hit = 0;
    for (int i = 0; i < (int)(CHAIN_MAX + 5); i++) {
        /* vary only mtime within same coset-hour to stay in same slot */
        mf.mtime = (uint64_t)(i % 3600);      /* stays in coset 0 */
        if (shape_place(&st, &mf) == -2) overflow_hit = 1;
    }
    ASSERT(overflow_hit, "chain overflow -2");

    if (ok) printf("[PASS] all 6 correctness checks\n");
    return ok;
#undef ASSERT
}

/* ════════════════════════════════════════
   BENCHMARK A — SCALE (10K / 100K)
   ════════════════════════════════════════ */
static void bench_scale(uint32_t n) {
    static ShapeStore st;
    shape_store_init(&st);

    FileMeta *metas = malloc(n * sizeof(FileMeta));
    uint64_t seed = 0xBEEFCAFEDEAD0000ULL;
    for (uint32_t i = 0; i < n; i++) metas[i] = gen_meta(&seed);

    /* PLACE */
    uint64_t t0 = ns_now();
    for (uint32_t i = 0; i < n; i++) shape_place(&st, &metas[i]);
    uint64_t t1 = ns_now();

    /* DELETE 2 cosets */
    shape_delete_coset(&st, 2);
    shape_delete_coset(&st, 6);

    /* RETRIEVE exact shape */
    uint32_t hits = 0;
    uint64_t t2 = ns_now();
    for (uint32_t i = 0; i < n; i++) {
        FileShape s = shape_from_meta(&metas[i]);
        if (st.reserved_mask & (1u << s.coset)) continue;
        ShapeQuery q = { s.trit, s.spoke, s.coset, s.letter };
        uint64_t out;
        if (shape_retrieve(&st, &q, &out)) hits++;
    }
    uint64_t t3 = ns_now();

    /* WILDCARD sweep (spoke=1, coset=3, any trit/letter) */
    uint32_t wc_hits = 0;
    uint64_t t4 = ns_now();
    for (uint8_t sp = 0; sp < SPOKE_MOD; sp++) {
        for (uint8_t co = 0; co < COSET_MOD; co++) {
            ShapeQuery qw = { 0xFF, sp, co, 0xFF };
            uint64_t out;
            if (shape_retrieve(&st, &qw, &out)) wc_hits++;
        }
    }
    uint64_t t5 = ns_now();

    uint32_t used, live, mx;
    slot_stats(&st, &used, &live, &mx);

    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║  SHAPE STORE  n=%-6u                              ║\n", n);
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  PLACE     %9.0f ops/s   %6.1f ns/op           ║\n",
           (double)n / ((t1-t0)/1e9), (double)(t1-t0)/n);
    printf("║  RETRIEVE  %9.0f ops/s   %6.1f ns/op           ║\n",
           (double)n / ((t3-t2)/1e9), (double)(t3-t2)/n);
    printf("║  WILDCARD  %9.0f ops/s   %6.1f ns/op (all slots)║\n",
           (double)(SPOKE_MOD*COSET_MOD) / ((t5-t4)/1e9),
           (double)(t5-t4)/(SPOKE_MOD*COSET_MOD));
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  slots used %4u / %4u  (%.1f%%)                    ║\n",
           used, SHAPE_SLOTS, 100.0*used/SHAPE_SLOTS);
    printf("║  live files %5u  max_chain %2u  overflow %u          ║\n",
           live, mx, st.overflow);
    printf("║  hit rate   %5.1f%%  (after 2 coset deletes)         ║\n",
           100.0*hits/n);
    printf("║  wildcard   %3u / %2u slot-axes found               ║\n",
           wc_hits, SPOKE_MOD * COSET_MOD);
    printf("╚══════════════════════════════════════════════════════╝\n");

    free(metas);
}

/* ════════════════════════════════════════
   BENCHMARK B — STRESS (slot saturation)
   Fill until overflow, measure saturation point
   ════════════════════════════════════════ */
static void bench_stress(void) {
    static ShapeStore st;
    shape_store_init(&st);
    uint64_t seed = 0xABCDEF1234567890ULL;
    uint32_t placed = 0, overflow = 0;

    /* fill until 10% overflow */
    uint32_t n = N_STRESS;
    for (uint32_t i = 0; i < n; i++) {
        FileMeta m = gen_meta(&seed);
        int r = shape_place(&st, &m);
        if (r == 0) placed++;
        else if (r == -2) overflow++;
    }

    uint32_t used, live, mx;
    slot_stats(&st, &used, &live, &mx);

    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║  STRESS — slot saturation  n=%-6u                ║\n", n);
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  placed  %6u  overflow  %6u (%.1f%%)            ║\n",
           placed, overflow, 100.0*overflow/n);
    printf("║  slots   %4u / %4u used  max_chain=%2u             ║\n",
           used, SHAPE_SLOTS, mx);
    printf("║  capacity = slots × chain = %u × %u = %u files   ║\n",
           SHAPE_SLOTS, CHAIN_MAX, SHAPE_SLOTS * CHAIN_MAX);
    printf("║  utilization %.1f%% of theoretical max               ║\n",
           100.0 * live / (SHAPE_SLOTS * CHAIN_MAX));
    printf("╚══════════════════════════════════════════════════════╝\n");
}

/* ════════════════════════════════════════
   BENCHMARK C — PATH BASELINE
   ════════════════════════════════════════ */
static void bench_path(uint32_t n) {
    static PathStore ps;
    path_init(&ps);
    uint64_t seed = 0xBEEFCAFEDEAD0000ULL;
    uint64_t *paths = malloc(n * sizeof(uint64_t));
    uint64_t *sigs  = malloc(n * sizeof(uint64_t));
    for (uint32_t i = 0; i < n; i++) { paths[i]=lcg(&seed); sigs[i]=lcg(&seed); }

    uint64_t t0 = ns_now();
    for (uint32_t i = 0; i < n; i++) path_place(&ps, paths[i], sigs[i]);
    uint64_t t1 = ns_now();

    uint32_t hits = 0;
    uint64_t t2 = ns_now();
    for (uint32_t i = 0; i < n; i++) {
        uint64_t out; if (path_retrieve(&ps, paths[i], &out)) hits++;
    }
    uint64_t t3 = ns_now();

    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║  PATH STORE (baseline hash-table)  n=%-6u        ║\n", n);
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  PLACE     %9.0f ops/s   %6.1f ns/op           ║\n",
           (double)n / ((t1-t0)/1e9), (double)(t1-t0)/n);
    printf("║  RETRIEVE  %9.0f ops/s   %6.1f ns/op           ║\n",
           (double)n / ((t3-t2)/1e9), (double)(t3-t2)/n);
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  hit rate  %5.1f%%  collision %5.1f%%                ║\n",
           100.0*hits/n,
           100.0*ps.collisions/(ps.placed > 0 ? ps.placed : 1));
    printf("║  no partial/wildcard query possible                  ║\n");
    printf("║  delete = full scan required                         ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");

    free(paths); free(sigs);
}

/* ════════════════════════════════════════
   MAIN
   ════════════════════════════════════════ */
int main(void) {
    printf("geo_shape_bench v2\n");
    printf("SHAPE_SLOTS=%u  CHAIN=%u  capacity=%u\n\n",
           SHAPE_SLOTS, CHAIN_MAX, SHAPE_SLOTS * CHAIN_MAX);

    printf("=== CORRECTNESS ===\n");
    if (!run_correctness()) return 1;

    printf("\n=== BENCH A: SCALE ===\n");
    bench_scale(N_SMALL);
    bench_scale(N_LARGE);

    printf("\n=== BENCH B: STRESS (saturation) ===\n");
    bench_stress();

    printf("\n=== BENCH C: PATH BASELINE ===\n");
    bench_path(N_SMALL);

    printf("\n=== SUMMARY ===\n");
    printf("Shape: place-by-geometry  — shape IS the address\n");
    printf("       retrieve by shape  — no filename/path needed\n");
    printf("       wildcard query     — 'any size, type=img, morning'\n");
    printf("       delete = coset silence — structural, not per-entry\n");
    printf("Path:  exact key required — must know full path\n");
    printf("       no partial query   — all-or-nothing\n");
    printf("       delete = scan+mark\n\n");
    printf("Memory: ShapeStore=%.1fKB  PathStore=%.1fKB\n",
           sizeof(ShapeStore)/1024.0, sizeof(PathStore)/1024.0);
    return 0;
}
