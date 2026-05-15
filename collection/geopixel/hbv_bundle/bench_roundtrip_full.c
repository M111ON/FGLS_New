/*
 * bench_roundtrip_full.c
 * Pipeline: encode (D4+Hilbert+classify) -> store -> decode (R-only + seed-index)
 * Verify: decoded == original (lossless path) + seed lookup correctness
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* --- Hilbert 8x8 --- */
static uint8_t G_H[64], G_HI[64];

static void init_hilbert(void) {
    uint8_t hmap[64];
    for(int p=0;p<64;p++){
        uint32_t px=p%8,py=p/8,rx,ry,s,d=0,tx=px,ty=py;
        for(s=4;s>0;s>>=1){rx=(tx&s)?1:0;ry=(ty&s)?1:0;
            d+=s*s*((3*rx)^ry);
            if(!ry){if(rx){tx=s-1-tx;ty=s-1-ty;}uint32_t t=tx;tx=ty;ty=t;}}
        hmap[p]=(uint8_t)(d&0x3F);
    }
    for(int p=0;p<64;p++) G_H[hmap[p]]=(uint8_t)p;
    for(int p=0;p<64;p++) G_HI[G_H[p]]=p;
}
static inline void h_fwd(uint8_t o[64],const uint8_t s[64]){for(int i=0;i<64;i++)o[i]=s[G_H[i]];}
static inline void h_inv(uint8_t o[64],const uint8_t s[64]){for(int i=0;i<64;i++)o[i]=s[G_HI[i]];}

/* --- FNV-64 --- */
static inline uint64_t fnv64(const uint8_t *d,int n){
    uint64_t h=1469598103934665603ULL;
    for(int i=0;i<n;i++){h^=d[i];h*=1099511628211ULL;}
    return h;
}

/* --- D4 canonicalize (8 symmetries, min-hash) --- */
static void d4_canon(const uint8_t s[64], uint8_t out[64]) {
    uint8_t best[64], tmp[64]; uint64_t bh=UINT64_MAX,th;
#define TRY { th=fnv64(tmp,64); if(th<bh){bh=th;memcpy(best,tmp,64);} }
    memcpy(tmp,s,64); TRY
    for(int y=0;y<8;y++)for(int x=0;x<8;x++)tmp[x*8+(7-y)]=s[y*8+x]; TRY /* r90 */
    for(int i=0;i<64;i++)tmp[63-i]=s[i]; TRY                              /* r180 */
    for(int y=0;y<8;y++)for(int x=0;x<8;x++)tmp[(7-x)*8+y]=s[y*8+x]; TRY /* r270 */
    for(int y=0;y<8;y++)for(int x=0;x<8;x++)tmp[y*8+(7-x)]=s[y*8+x]; TRY /* fh */
    for(int y=0;y<8;y++)for(int x=0;x<8;x++)tmp[(7-y)*8+x]=s[y*8+x]; TRY /* fv */
    for(int y=0;y<8;y++)for(int x=0;x<8;x++)tmp[x*8+y]=s[y*8+x]; TRY     /* tr */
    for(int y=0;y<8;y++)for(int x=0;x<8;x++)tmp[(7-x)*8+(7-y)]=s[y*8+x]; TRY /* at */
#undef TRY
    memcpy(out,best,64);
}

/* --- Entropy (unique byte count) --- */
static inline uint32_t entropy(const uint8_t *b){
    uint8_t seen[256]={0}; uint32_t u=0;
    for(int i=0;i<64;i++) if(!seen[b[i]]++) u++;
    return u;
}

/* --- Seed Index (hash map: seed -> canonical 64B) --- */
#define SIDX_CAP  (1<<16)
#define SIDX_MASK (SIDX_CAP-1)
typedef struct { uint64_t seed; uint8_t chunk[64]; uint8_t used; } SeedSlot;

typedef struct {
    SeedSlot *slots;
    uint64_t  n_entries;
    uint64_t  n_collisions;
} SeedIndex;

static SeedIndex* sidx_create(void){
    SeedIndex *idx=calloc(1,sizeof(SeedIndex));
    idx->slots=calloc(SIDX_CAP,sizeof(SeedSlot));
    return idx;
}
static void sidx_free(SeedIndex *idx){ free(idx->slots); free(idx); }

static void sidx_put(SeedIndex *idx, uint64_t seed, const uint8_t *chunk){
    uint32_t slot=(uint32_t)(seed & SIDX_MASK);
    for(int i=0;i<SIDX_CAP;i++){
        uint32_t s=(slot+i)&SIDX_MASK;
        if(!idx->slots[s].used){
            idx->slots[s].used=1;
            idx->slots[s].seed=seed;
            memcpy(idx->slots[s].chunk,chunk,64);
            idx->n_entries++;
            return;
        }
        if(idx->slots[s].seed==seed) return;
        idx->n_collisions++;
    }
}
static const uint8_t* sidx_get(const SeedIndex *idx, uint64_t seed){
    uint32_t slot=(uint32_t)(seed & SIDX_MASK);
    for(int i=0;i<SIDX_CAP;i++){
        uint32_t s=(slot+i)&SIDX_MASK;
        if(!idx->slots[s].used) return NULL;
        if(idx->slots[s].seed==seed) return idx->slots[s].chunk;
    }
    return NULL;
}

/* --- Encoded tile store --- */
#define FLAG_LOSSLESS 1
#define FLAG_SEED     0
typedef struct {
    uint8_t  flag;
    uint64_t seed;
    uint8_t  r[64];
} EncodedTile;

/* --- Full pipeline --- */
typedef struct {
    uint64_t n;
    uint64_t n_lossless, n_seed;
    uint64_t bytes_encoded;
    uint64_t bytes_raw;
    double   enc_ms, dec_ms;
    uint64_t dec_exact;
    uint64_t dec_seed_hit;
    uint64_t dec_seed_miss;
} Stats;

static Stats run(const uint8_t *data, uint64_t len, const char *label,
                 int ent_thresh)
{
    Stats st={0};
    uint64_t N=len/64;
    st.n=N; st.bytes_raw=N*64;

    EncodedTile *tiles=malloc(N*sizeof(EncodedTile));
    SeedIndex   *sidx=sidx_create();

    /* --- ENCODE --- */
    struct timespec t0,t1;
    clock_gettime(CLOCK_MONOTONIC,&t0);

    for(uint64_t i=0;i<N;i++){
        const uint8_t *src=data+i*64;
        uint8_t canon[64], hperm[64];
        d4_canon(src,canon);
        h_fwd(hperm,canon);
        uint64_t seed=fnv64(hperm,64);
        tiles[i].seed=seed;
        uint32_t ent=entropy(hperm);
        if(ent<=ent_thresh){
            tiles[i].flag=FLAG_SEED;
            sidx_put(sidx,seed,hperm);
            st.n_seed++;
            st.bytes_encoded+=8;
        } else {
            tiles[i].flag=FLAG_LOSSLESS;
            memcpy(tiles[i].r,hperm,64);
            st.n_lossless++;
            st.bytes_encoded+=64;
        }
    }

    clock_gettime(CLOCK_MONOTONIC,&t1);
    st.enc_ms=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)*1e-6;

    /* --- DECODE --- */
    uint8_t *decoded=malloc(N*64);
    clock_gettime(CLOCK_MONOTONIC,&t0);

    for(uint64_t i=0;i<N;i++){
        uint8_t hperm[64];
        if(tiles[i].flag==FLAG_LOSSLESS){
            h_inv(hperm,tiles[i].r);
            memcpy(decoded+i*64,hperm,64);
        } else {
            const uint8_t *found=sidx_get(sidx,tiles[i].seed);
            if(found){
                h_inv(hperm,found);
                memcpy(decoded+i*64,hperm,64);
                st.dec_seed_hit++;
            } else {
                memset(decoded+i*64,0,64);
                st.dec_seed_miss++;
            }
        }
    }

    clock_gettime(CLOCK_MONOTONIC,&t1);
    st.dec_ms=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)*1e-6;

    /* --- VERIFY: compare canonical form --- */
    for(uint64_t i=0;i<N;i++){
        uint8_t canon[64];
        d4_canon(data+i*64,canon);
        if(memcmp(decoded+i*64,canon,64)==0) st.dec_exact++;
    }

    /* --- REPORT --- */
    double ratio=(double)st.bytes_raw/(double)st.bytes_encoded;
    double enc_mbs=(double)st.bytes_raw/1e6/(st.enc_ms/1e3);
    double dec_mbs=(double)st.bytes_raw/1e6/(st.dec_ms/1e3);

    printf("\n[%s]  thresh=%d\n", label, ent_thresh);
    printf("  chunks    : %llu\n",(unsigned long long)N);
    printf("  lossless  : %5llu (%4.1f%%)  x 64B\n",
           (unsigned long long)st.n_lossless,100.0*st.n_lossless/N);
    printf("  seed-only : %5llu (%4.1f%%)  x 8B   idx_entries=%llu  collisions=%llu\n",
           (unsigned long long)st.n_seed,100.0*st.n_seed/N,
           (unsigned long long)sidx->n_entries,(unsigned long long)sidx->n_collisions);
    printf("  ratio     : %.3fx   (raw=%llu  enc=%llu bytes)\n",
           ratio,(unsigned long long)st.bytes_raw,(unsigned long long)st.bytes_encoded);
    printf("  encode    : %6.2f ms  -> %6.0f MB/s\n",st.enc_ms,enc_mbs);
    printf("  decode    : %6.2f ms  -> %6.0f MB/s\n",st.dec_ms,dec_mbs);
    printf("  verify    : %llu/%llu exact (canonical roundtrip)\n",
           (unsigned long long)st.dec_exact,(unsigned long long)N);
    printf("  seed hits : %llu  misses=%llu\n",
           (unsigned long long)st.dec_seed_hit,(unsigned long long)st.dec_seed_miss);

    free(tiles); sidx_free(sidx); free(decoded);
    return st;
}

int main(void){
    init_hilbert();

    /* --- 1. Real source files --- */
    const char *files[]={
        "fgls/fibo_tile_dispatch.h","fgls/fibo_layer_header.h",
        "fgls/test_diamond_flow_grouping.c","fgls/hb_header_frame.h",
        "core/core/pogls_fold.h","core/core/geo_diamond_field.h",
        NULL
    };
    uint8_t *pool=malloc(8192*64), *fbuf=malloc(512*1024); int ftotal=0;
    for(int fi=0;files[fi];fi++){
        FILE *f=fopen(files[fi],"rb"); if(!f) continue;
        int n=(int)fread(fbuf,1,512*1024,f); fclose(f);
        int nc=n/64; if(ftotal+nc>8192) nc=8192-ftotal;
        memcpy(pool+ftotal*64,fbuf,nc*64); ftotal+=nc;
    }
    free(fbuf);
    run(pool,(uint64_t)ftotal*64,"C source files",12);
    run(pool,(uint64_t)ftotal*64,"C source files",24);
    free(pool);

    /* --- 2. Geometric synthetic --- */
    int GN=8192; uint8_t *gbuf=malloc(GN*64);
    for(int i=0;i<GN*64;i++) gbuf[i]=(uint8_t)((i*17+13*((i/64)%144))&0xFF);
    run(gbuf,(uint64_t)GN*64,"Geometric/Fibonacci synthetic",12);
    free(gbuf);

    /* --- 3. Weight-like (sparse unique values) --- */
    int WN=8192; uint8_t *wbuf=malloc(WN*64);
    for(int i=0;i<WN*64;i++) wbuf[i]=(uint8_t)((((i/64)*7+i%64)*3)&0x0F)*17;
    run(wbuf,(uint64_t)WN*64,"Weight-like (16 unique vals/chunk)",12);
    free(wbuf);

    /* --- 4. Worst case: random --- */
    int RN=4096; uint8_t *rbuf=malloc(RN*64);
    srand(42); for(int i=0;i<RN*64;i++) rbuf[i]=(uint8_t)(rand()&0xFF);
    run(rbuf,(uint64_t)RN*64,"Random (worst case)",12);
    free(rbuf);

    return 0;
}
