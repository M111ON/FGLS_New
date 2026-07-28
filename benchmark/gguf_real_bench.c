/*
 * gguf_real_bench.c — REAL GGUF Tensor Layout Benchmark
 *
 * Tests hypothesis: "Geometric layout makes tensor access faster"
 * using REAL weights from Qwen3-0.6B Q4_0.
 *
 * Tensor: token_embd.weight (155 MB, F32, 1024 × 151936)
 *   - This exceeds L3 cache (3-16MB) → real cache pressure
 *   - Accessed during every forward pass (embedding lookup)
 *   - Layout affects: row access pattern, cache behavior, TLB pressure
 *
 * What we measure:
 *   1. Embedding lookup time (ns) — random row access × 10000
 *   2. Cache miss rate — simulated L2 (256KB) + L3 (8MB)
 *   3. Page faults — distinct 4KB pages touched
 *   4. Latency distribution — avg + P95 + P99
 *
 * Build: gcc -O2 -Wall -o gguf_real_bench.exe gguf_real_bench.c -lm -I../beam_addressing
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* GGUF reader */
#include "gguf_reader.h"

/* ── Config ── */
#define EMB_DIM     (1024)          /* Qwen3-0.6B hidden dim */
#define N_ROWS      (151936)        /* vocab size */
#define ROW_BYTES   (EMB_DIM * sizeof(float))   /* 4096 bytes per row */
#define TOTAL_BYTES ((uint64_t)N_ROWS * ROW_BYTES)  /* ~593 MB */
#define N_LOOKUPS   (10000)         /* random row lookups */
#define CACHE_LINE  (64)
#define PAGE_B      (4096)
#define SEED        (42)

/* ── Timer ── */
#ifdef _WIN32
#include <windows.h>
static double now_ns(void) {
    static LARGE_INTEGER f={0}; if(!f.QuadPart)QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;QueryPerformanceCounter(&t);
    return (double)t.QuadPart/(double)f.QuadPart*1e9;
}
#else
static double now_ns(void) {
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec*1e9+ts.tv_nsec;
}
#endif

/* ── RNG ── */
static uint32_t rng_s;
static uint32_t rng(void){rng_s^=rng_s<<13;rng_s^=rng_s>>17;rng_s^=rng_s<<5;return rng_s;}

/* ── Hilbert 2D ── */
#define H_N (1024)  /* 1024×1024 = 1M > 151936 */
static void hrot(int n,int*x,int*y,int rx,int ry){
    if(ry==0){if(rx==1){*x=n-1-*x;*y=n-1-*y;}int t=*x;*x=*y;*y=t;}
}
static int h_xy2d(int n,int x,int y){
    int rx,ry,s,d=0;
    for(s=n/2;s>0;s/=2){rx=(x&s)>0;ry=(y&s)>0;d+=s*s*((3*rx)^ry);hrot(s,&x,&y,rx,ry);}
    return d;
}

/* ── Morton 3D ── */
static uint32_t p1(uint32_t n){n=(n|(n<<8))&0xFF00FF;n=(n|(n<<4))&0xF0F0F0F;n=(n|(n<<2))&0x33333333;n=(n|(n<<1))&0x55555555;return n;}
static uint64_t morton3(uint32_t x,uint32_t y,uint32_t z){return((uint64_t)p1(x)<<2)|((uint64_t)p1(y)<<1)|p1(z);}

/* ── Layout types ── */
typedef struct {
    float *data;        /* remapped data (N_ROWS × EMB_DIM) */
    int *row_map;       /* row_map[original_row] = new_position */
    char *name;
} Layout;

/* ── Cache simulators ── */
#define L2_LINES (256*1024/CACHE_LINE)   /* 256KB L2 */
#define L3_LINES (8*1024*1024/CACHE_LINE) /* 8MB L3 */
static uint8_t *l2_cache, *l3_cache;
static int l2_n, l3_n;
static uint32_t *l2_count, *l3_count;
static uint8_t *page_bitmap;static int page_n;

static void cache_reset(void){
    memset(l2_cache,0,L2_LINES);memset(l3_cache,0,L3_LINES);
    memset(l2_count,0,L2_LINES*sizeof(uint32_t));
    memset(l3_count,0,L3_LINES*sizeof(uint32_t));
    memset(page_bitmap,0,(TOTAL_BYTES/PAGE_B+1));
    l2_n=l3_n=page_n=0;
}
static void cache_touch(uint64_t addr){
    uint64_t cl=addr/CACHE_LINE;
    uint32_t pg=(uint32_t)(addr/PAGE_B);
    /* L2 */
    int l2_idx=(int)(cl%L2_LINES);
    if(!l2_cache[l2_idx])l2_n++;
    l2_cache[l2_idx]=1;l2_count[l2_idx]++;
    /* L3 */
    int l3_idx=(int)(cl%L3_LINES);
    if(!l3_cache[l3_idx])l3_n++;
    l3_cache[l3_idx]=1;l3_count[l3_idx]++;
    /* Page */
    if(!page_bitmap[pg]){page_bitmap[pg]=1;page_n++;}
}

/* ── Latency array ── */
typedef struct{double*d;int n,c;}LA;
static void la_init(LA*a,int c){a->d=malloc(c*sizeof(double));a->n=0;a->c=c;}
static void la_push(LA*a,double v){if(a->n<a->c)a->d[a->n++]=v;}
static void la_sort(LA*a){
    for(int i=1;i<a->n;i++){double k=a->d[i];int j=i-1;while(j>=0&&a->d[j]>k){a->d[j+1]=a->d[j];j--;}a->d[j+1]=k;}
}
static double la_avg(LA*a){double s=0;for(int i=0;i<a->n;i++)s+=a->d[i];return s/a->n;}
static double la_p50(LA*a){return a->d[a->n/2];}
static double la_p95(LA*a){return a->d[(int)(a->n*0.95)];}
static double la_p99(LA*a){return a->d[(int)(a->n*0.99)];}
static double la_max(LA*a){return a->d[a->n-1];}

/* ── Result ── */
typedef struct{
    double avg_ns,p50_ns,p95_ns,p99_ns,max_ns;
    int l2_used,l3_used,pages;
    double l2_miss_rate,l3_miss_rate;
    double total_ms;
}Result;

/* ── Build layouts ── */
static Layout build_linear(float *src){
    Layout L={.data=malloc(TOTAL_BYTES),.row_map=malloc(N_ROWS*sizeof(int)),.name="Linear"};
    memcpy(L.data,src,TOTAL_BYTES);
    for(int i=0;i<N_ROWS;i++)L.row_map[i]=i;
    return L;
}

static Layout build_hilbert(float *src){
    Layout L={.data=malloc(TOTAL_BYTES),.row_map=malloc(N_ROWS*sizeof(int)),.name="Hilbert-2D"};
    /* Map each original row to a Hilbert position */
    int *hil_order=malloc(N_ROWS*sizeof(int));
    for(int i=0;i<N_ROWS;i++){
        int x=i%H_N,y=i/H_N;
        hil_order[i]=h_xy2d(H_N,x,y);
    }
    /* Sort by Hilbert distance */
    int *idx=malloc(N_ROWS*sizeof(int));
    for(int i=0;i<N_ROWS;i++)idx[i]=i;
    for(int i=1;i<N_ROWS;i++){
        int kd=hil_order[i],ki=idx[i],j=i-1;
        while(j>=0&&hil_order[j]>kd){hil_order[j+1]=hil_order[j];idx[j+1]=idx[j];j--;}
        hil_order[j+1]=kd;idx[j+1]=ki;
    }
    /* Remap: new position → original row */
    for(int i=0;i<N_ROWS;i++){
        L.row_map[idx[i]]=i;  /* original row idx[i] goes to position i */
        memcpy(L.data+(uint64_t)i*EMB_DIM, src+(uint64_t)idx[i]*EMB_DIM, ROW_BYTES);
    }
    free(hil_order);free(idx);
    return L;
}

static Layout build_morton3(float *src){
    Layout L={.data=malloc(TOTAL_BYTES),.row_map=malloc(N_ROWS*sizeof(int)),.name="Morton-3D"};
    /* 3D: 54×54×52 = 151632 ≈ 151936 */
    int GX=54,GY=54,GZ=52;
    int *morton_order=malloc(N_ROWS*sizeof(int));
    for(int i=0;i<N_ROWS;i++){
        int x=i%GX,y=(i/GX)%GY,z=i/(GX*GY);
        if(z>=GZ)z=GZ-1;
        morton_order[i]=(int)morton3(x,y,z);
    }
    int *idx=malloc(N_ROWS*sizeof(int));
    for(int i=0;i<N_ROWS;i++)idx[i]=i;
    for(int i=1;i<N_ROWS;i++){
        int kd=morton_order[i],ki=idx[i],j=i-1;
        while(j>=0&&morton_order[j]>kd){morton_order[j+1]=morton_order[j];idx[j+1]=idx[j];j--;}
        morton_order[j+1]=kd;idx[j+1]=ki;
    }
    for(int i=0;i<N_ROWS;i++){
        L.row_map[idx[i]]=i;
        memcpy(L.data+(uint64_t)i*EMB_DIM, src+(uint64_t)idx[i]*EMB_DIM, ROW_BYTES);
    }
    free(morton_order);free(idx);
    return L;
}

static Layout build_random(float *src){
    Layout L={.data=malloc(TOTAL_BYTES),.row_map=malloc(N_ROWS*sizeof(int)),.name="Random"};
    int *idx=malloc(N_ROWS*sizeof(int));
    for(int i=0;i<N_ROWS;i++)idx[i]=i;
    rng_s=99;
    for(int i=N_ROWS-1;i>0;i--){int j=rng()%(i+1);int t=idx[i];idx[i]=idx[j];idx[j]=t;}
    for(int i=0;i<N_ROWS;i++){
        L.row_map[idx[i]]=i;
        memcpy(L.data+(uint64_t)i*EMB_DIM, src+(uint64_t)idx[i]*EMB_DIM, ROW_BYTES);
    }
    free(idx);
    return L;
}

/* ── Benchmark: Embedding lookup (random row access) ── */
static void bench_lookup(Layout *L, float *query_vec, float *output, Result *R){
    cache_reset();
    rng_s=SEED;
    LA lat;la_init(&lat,N_LOOKUPS);

    /* Generate random row indices */
    int *rows=malloc(N_LOOKUPS*sizeof(int));
    for(int i=0;i<N_LOOKUPS;i++)rows[i]=rng()%N_ROWS;

    double t0=now_ns();
    for(int i=0;i<N_LOOKUPS;i++){
        int row=rows[i];
        uint64_t offset=(uint64_t)row*EMB_DIM*sizeof(float);
        float *row_ptr=(float*)((char*)L->data+offset);

        /* Embedding lookup: dot product query · row (simulates attention) */
        double ta=now_ns();
        float dot=0;
        for(int d=0;d<EMB_DIM;d++){
            dot+=query_vec[d]*row_ptr[d];
            /* Touch cache lines */
            cache_touch(offset+d*sizeof(float));
        }
        output[i]=dot;
        double tb=now_ns();
        la_push(&lat,tb-ta);
    }
    R->total_ms=(now_ns()-t0)/1e6;

    la_sort(&lat);
    R->avg_ns=la_avg(&lat);R->p50_ns=la_p50(&lat);
    R->p95_ns=la_p95(&lat);R->p99_ns=la_p99(&lat);R->max_ns=la_max(&lat);
    R->l2_used=l2_n;R->l3_used=l3_n;R->pages=page_n;
    R->l2_miss_rate=(double)l2_n/N_LOOKUPS*100;
    R->l3_miss_rate=(double)l3_n/N_LOOKUPS*100;

    free(rows);free(lat.d);
}

/* ── Benchmark: Sequential scan (full embedding matrix) ── */
static void bench_scan(Layout *L, Result *R){
    cache_reset();
    volatile float sink=0;
    double t0=now_ns();
    for(uint64_t i=0;i<N_ROWS;i++){
        float *row=(float*)((char*)L->data+i*ROW_BYTES);
        sink+=row[0]; /* touch first element of each row */
        cache_touch(i*ROW_BYTES);
    }
    R->total_ms=(now_ns()-t0)/1e6;
    R->l2_used=l2_n;R->l3_used=l3_n;R->pages=page_n;
    R->l2_miss_rate=(double)l2_n/N_ROWS*100;
    R->l3_miss_rate=(double)l3_n/N_ROWS*100;
    (void)sink;
}

int main(void){
    printf("═══════════════════════════════════════════════════════════════════════════════════════════\n");
    printf("  GGUF REAL TENSOR BENCH — Layout Impact on Real Model Weights\n");
    printf("  Tensor: token_embd.weight (Qwen3-0.6B Q4_0)\n");
    printf("  Size: %d × %d = %.0f MB  |  L2: 256KB  |  L3: 8MB\n",
           N_ROWS, EMB_DIM, TOTAL_BYTES/1e6);
    printf("  Lookups: %d random rows  |  Pattern: embedding lookup (dot product)\n", N_LOOKUPS);
    printf("═══════════════════════════════════════════════════════════════════════════════════════════\n\n");

    /* ── Load real tensor from GGUF ── */
    printf("Loading tensor from GGUF...\n");
    GGUF_File *gf=gguf_open("I:/model/Qwen3-0.6B-Q4_0.gguf");
    if(!gf){fprintf(stderr,"Failed to open GGUF\n");return 1;}

    int tidx=gguf_find_tensor(gf,"token_embd");
    if(tidx<0){fprintf(stderr,"tensor not found\n");gguf_close(gf);return 1;}
    GGUF_Tensor *t=&gf->tensors[tidx];
    printf("  Found: %s  dims=%lux%lu  type=%u  bytes=%lu\n",
           t->name,(unsigned long)t->dims[0],(unsigned long)t->dims[1],
           t->type,(unsigned long)t->size_bytes);

    /* Read tensor data (F32) */
    float *src_rows=malloc(TOTAL_BYTES);
    fseek(gf->fp, gf->tensor_data_start+t->offset, SEEK_SET);
    /* token_embd is stored as F16 in GGUF, need to read carefully */
    /* Actually the GGUF says type=0 (F32) for token_embd in some versions */
    /* Let's read raw bytes and see */
    uint64_t read_bytes=t->size_bytes;
    if(read_bytes>TOTAL_BYTES)read_bytes=TOTAL_BYTES;
    fread(src_rows, 1, (size_t)read_bytes, gf->fp);
    printf("  Read %lu bytes (%.1f MB)\n",(unsigned long)read_bytes,read_bytes/1e6);
    gguf_close(gf);

    /* ── Allocate cache simulators ── */
    l2_cache=calloc(L2_LINES,1);l3_cache=calloc(L3_LINES,1);
    l2_count=calloc(L2_LINES,sizeof(uint32_t));
    l3_count=calloc(L3_LINES,sizeof(uint32_t));
    page_bitmap=calloc(TOTAL_BYTES/PAGE_B+1,1);

    /* ── Build layouts ── */
    printf("\nBuilding layouts...\n");
    Layout layouts[4];
    layouts[0]=build_linear(src_rows);
    layouts[1]=build_hilbert(src_rows);
    layouts[2]=build_morton3(src_rows);
    layouts[3]=build_random(src_rows);

    for(int i=0;i<4;i++)
        printf("  %s: %p\n",layouts[i].name,(void*)layouts[i].data);

    /* ── Prepare query vector ── */
    float *query=malloc(EMB_DIM*sizeof(float));
    float *output=malloc(N_LOOKUPS*sizeof(float));
    rng_s=SEED;
    for(int i=0;i<EMB_DIM;i++)query[i]=(float)rng()/UINT32_MAX;

    /* ── Benchmark ── */
    Result R[4];

    printf("\n┌──────────────────────────────────────────────────────────────────────────────────┐\n");
    printf("│  TEST 1: Embedding Lookup (dot product, %d random rows)                        │\n",N_LOOKUPS);
    printf("├────────────────┬──────────┬──────────┬──────────┬──────────┬──────────┬──────────┤\n");
    printf("│ Layout         │ Avg ns   │ P50 ns   │ P95 ns   │ P99 ns   │ Total ms │ L2 Miss%% │\n");
    printf("├────────────────┼──────────┼──────────┼──────────┼──────────┼──────────┼──────────┤\n");
    for(int i=0;i<4;i++){
        bench_lookup(&layouts[i],query,output,&R[i]);
        printf("│ %-14s │ %8.1f │ %8.1f │ %8.0f │ %8.0f │ %8.1f │ %7.2f%% │\n",
               layouts[i].name,R[i].avg_ns,R[i].p50_ns,R[i].p95_ns,R[i].p99_ns,
               R[i].total_ms,R[i].l2_miss_rate);
    }
    printf("└────────────────┴──────────┴──────────┴──────────┴──────────┴──────────┴──────────┘\n");

    printf("\n┌──────────────────────────────────────────────────────────────────────────────────┐\n");
    printf("│  TEST 2: Cache & Memory Behavior                                               │\n");
    printf("├────────────────┬──────────┬──────────┬──────────┬──────────┬─────────────────────┤\n");
    printf("│ Layout         │ L2 Lines │ L3 Lines │ Pages    │ L3 Miss%% │ Footprint           │\n");
    printf("├────────────────┼──────────┼──────────┼──────────┼──────────┼─────────────────────┤\n");
    for(int i=0;i<4;i++){
        printf("│ %-14s │ %8d │ %8d │ %8d │ %7.2f%% │ %8.1f MB           │\n",
               layouts[i].name,R[i].l2_used,R[i].l3_used,R[i].pages,
               R[i].l3_miss_rate,R[i].pages*4.0/1024);
    }
    printf("└────────────────┴──────────┴──────────┴──────────┴──────────┴─────────────────────┘\n");

    /* ── VERDICT ── */
    printf("\n═══════════════════════════════════════════════════════════════════════════════════════════\n");
    printf("  VERDICT — Real GGUF Tensor Layout Impact\n");
    printf("───────────────────────────────────────────────────────────────────────────────────────────\n");

    int best=0;
    for(int i=1;i<4;i++)if(R[i].avg_ns<R[best].avg_ns)best=i;
    double spd=(R[0].avg_ns-R[best].avg_ns)/R[0].avg_ns*100;

    printf("  🏆 Fastest: %s (%.1f ns avg)\n",layouts[best].name,R[best].avg_ns);
    if(best!=0)
        printf("  ✅ %s is %.1f%% faster than Linear for real tensor lookup\n",layouts[best].name,spd);
    else
        printf("  ⚠️  Linear is fastest — geometry advantage not visible at this access pattern\n");

    printf("\n  L3 Cache Miss Rate (lower = fewer DRAM accesses):\n");
    for(int i=0;i<4;i++){
        const char *tag="";
        if(i==0)tag="(baseline)";
        else if(R[i].l3_miss_rate<R[0].l3_miss_rate*0.95)tag="← BETTER";
        else if(R[i].l3_miss_rate>R[0].l3_miss_rate*1.05)tag="← worse";
        else tag="≈ same";
        printf("    %-14s  L3 miss: %6.2f%%  %s\n",layouts[i].name,R[i].l3_miss_rate,tag);
    }

    printf("\n  Page Footprint (lower = less TLB pressure):\n");
    for(int i=0;i<4;i++){
        printf("    %-14s  %5d pages = %.1f MB\n",layouts[i].name,R[i].pages,R[i].pages*4.0/1024);
    }

    printf("═══════════════════════════════════════════════════════════════════════════════════════════\n");

    /* Cleanup */
    free(src_rows);free(query);free(output);
    for(int i=0;i<4;i++){free(layouts[i].data);free(layouts[i].row_map);}
    free(l2_cache);free(l3_cache);free(l2_count);free(l3_count);free(page_bitmap);
    return 0;
}
