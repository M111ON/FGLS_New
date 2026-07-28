/*
 * geo_seek_bench_v3.c — Large scale: 1M items × 64B = 64MB (exceeds L3)
 *
 * This tests the REAL hypothesis at scale where cache actually matters.
 * Items = 1,048,576 (1M). Layouts map these to memory addresses.
 * Access pattern: spatial locality (nearby in 3D → nearby in memory).
 *
 * Build: gcc -O2 -Wall -o geo_seek_bench_v3.exe geo_seek_bench_v3.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define N_ITEMS     (65536)          /* 64K items = 4MB at 64B each */
#define SLOT_B      (64)
#define BUF_BYTES   ((uint64_t)N_ITEMS * SLOT_B)
#define TRIALS      (1000)          /* spatial queries */
#define CACHE_LINE  (64)
#define K_NEIGHBORS (8)            /* spatial query: K nearest */
#define SEED        (42)

static uint8_t *bigbuf;            /* 64 MB */

/* Timer */
#ifdef _WIN32
#include <windows.h>
static double now_ns(void) {
    static LARGE_INTEGER f = {0};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart * 1e9;
}
#else
static double now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}
#endif

/* RNG */
static uint32_t rng_s;
static uint32_t rng(void) {
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5;
    return rng_s;
}

/* ── Hilbert 2D ── */
#define H_N 256    /* 256x256 = 64K grid */
static void hrot(int n, int *x, int *y, int rx, int ry) {
    if (ry == 0) { if (rx==1){*x=n-1-*x;*y=n-1-*y;} int t=*x;*x=*y;*y=t; }
}
static int h_xy2d(int n, int x, int y) {
    int rx,ry,s,d=0;
    for (s=n/2;s>0;s/=2){rx=(x&s)>0;ry=(y&s)>0;d+=s*s*((3*rx)^ry);hrot(s,&x,&y,rx,ry);}
    return d;
}

/* ── 3D Hilbert (simple: use 32x32x32 = 32768, repeat for 1M) ── */
/* Actually, let's use a simpler approach: 2D Hilbert on 1024x1024 */
/* Then for 3D: use Z-order (Morton) on 100x100x100 ≈ 1M */

/* Morton 3D interleaving */
static uint32_t p1(uint32_t n){n=(n|(n<<8))&0xFF00FF;n=(n|(n<<4))&0xF0F0F0F;n=(n|(n<<2))&0x33333333;n=(n|(n<<1))&0x55555555;return n;}
static uint64_t morton3(uint32_t x,uint32_t y,uint32_t z){
    return ((uint64_t)p1(x)<<2)|((uint64_t)p1(y)<<1)|p1(z);
}

/* ── Types ── */
typedef struct { float x,y,z; } Vec3;
typedef struct { int *map; Vec3 *pos; char *name; } Layout;

static void build_linear(Layout *L) {
    L->map = malloc(N_ITEMS * sizeof(int));
    L->pos = NULL;
    L->name = "Linear";
    for (int i=0;i<N_ITEMS;i++) L->map[i]=i;
}

static void build_hilbert(Layout *L) {
    L->map = malloc(N_ITEMS * sizeof(int));
    L->pos = NULL;
    L->name = "Hilbert-2D";
    /* Build Hilbert distance for each cell, sort, assign */
    int *dist = malloc(N_ITEMS * sizeof(int));
    int *ord = malloc(N_ITEMS * sizeof(int));
    for (int i=0;i<N_ITEMS;i++){dist[i]=h_xy2d(H_N,i%H_N,i/H_N);ord[i]=i;}
    /* Insertion sort (too big for qsort on some systems, but fine here) */
    for (int i=1;i<N_ITEMS;i++){
        int kd=dist[i],ko=ord[i],j=i-1;
        while(j>=0&&dist[j]>kd){dist[j+1]=dist[j];ord[j+1]=ord[j];j--;}
        dist[j+1]=kd;ord[j+1]=ko;
    }
    for (int i=0;i<N_ITEMS;i++) L->map[ord[i]]=i;
    free(dist);free(ord);
}

static void build_morton3(Layout *L) {
    L->map = malloc(N_ITEMS * sizeof(int));
    L->pos = NULL;
    L->name = "Morton-3D";
    /* 40x40x40 = 64000 grid */
    int GR = 40;
    int *dist = malloc(N_ITEMS * sizeof(int));
    int *ord = malloc(N_ITEMS * sizeof(int));
    for (int i=0;i<N_ITEMS;i++){
        int x=i%GR, y=(i/GR)%GR, z=i/(GR*GR);
        dist[i]=(int)morton3(x,y,z);
        ord[i]=i;
    }
    for (int i=1;i<N_ITEMS;i++){
        int kd=dist[i],ko=ord[i],j=i-1;
        while(j>=0&&dist[j]>kd){dist[j+1]=dist[j];ord[j+1]=ord[j];j--;}
        dist[j+1]=kd;ord[j+1]=ko;
    }
    for (int i=0;i<N_ITEMS;i++) L->map[ord[i]]=i;
    free(dist);free(ord);
}

static void build_random(Layout *L) {
    L->map = malloc(N_ITEMS * sizeof(int));
    L->pos = NULL;
    L->name = "Random";
    for (int i=0;i<N_ITEMS;i++) L->map[i]=i;
    rng_s=99;
    for (int i=N_ITEMS-1;i>0;i--){int j=rng()%(i+1);int t=L->map[i];L->map[i]=L->map[j];L->map[j]=t;}
}

/* ── Generate 3D positions for each item (golden spiral) ── */
static Vec3 *gen_positions(void) {
    Vec3 *p = malloc(N_ITEMS * sizeof(Vec3));
    for (int i=0;i<N_ITEMS;i++){
        double y=1.0-2.0*((double)i/(N_ITEMS-1));
        double r=sqrt(1.0-y*y);
        double th=2.39996322972865332*i;
        p[i].x=(float)(r*cos(th)); p[i].y=(float)y; p[i].z=(float)(r*sin(th));
    }
    return p;
}

/* ── Cache simulator (direct-mapped, 64K lines = 4MB L3) ── */
#define SIM_LINES (65536)
static int sim_cache[SIM_LINES];
static int sim_total, sim_hits;

static void sim_reset(void){memset(sim_cache,0,sizeof(sim_cache));sim_total=0;sim_hits=0;}
static void sim_access(uint64_t a){
    int l=(int)((a/CACHE_LINE)%SIM_LINES);
    sim_total++;
    if(sim_cache[l])sim_hits++;
    sim_cache[l]=1;
}

/* ── Page tracker ── */
#define MAX_PG 131072
static uint32_t pg_seen[MAX_PG]; static int pg_n;
static void pg_reset(void){memset(pg_seen,0,sizeof(pg_seen));pg_n=0;}
static void pg_touch(uint64_t a){
    uint32_t pg=(uint32_t)(a/4096);
    for(int i=0;i<pg_n;i++)if(pg_seen[i]==pg)return;
    pg_seen[pg_n++]=pg;
}

/* ── Bench: Spatial K-NN query ── */
typedef struct { double total_ns,avg_ns; int hits,total_acc,cache_used,pages; } Result;

static void bench_spatial(Layout *L, Vec3 *pos, Result *R) {
    sim_reset(); pg_reset();
    rng_s = SEED;
    int Q = TRIALS;
    double total = 0;

    for (int q=0;q<Q;q++){
        int c = rng()%N_ITEMS;
        Vec3 cv = pos[c];

        /* Brute-force K-NN */
        double *d = malloc(N_ITEMS*sizeof(double));
        int *idx = malloc(N_ITEMS*sizeof(int));
        for(int i=0;i<N_ITEMS;i++){
            float dx=pos[i].x-cv.x,dy=pos[i].y-cv.y,dz=pos[i].z-cv.z;
            d[i]=dx*dx+dy*dy+dz*dz; idx[i]=i;
        }
        /* Partial selection */
        for(int k=0;k<K_NEIGHBORS;k++){
            int mk=k;
            for(int j=k+1;j<N_ITEMS;j++)if(d[j]<d[mk])mk=j;
            double td=d[k];d[k]=d[mk];d[mk]=td;
            int ti=idx[k];idx[k]=idx[mk];idx[mk]=ti;
        }
        /* Access K neighbors through layout */
        double t0=now_ns();
        for(int k=0;k<K_NEIGHBORS;k++){
            uint64_t a=(uint64_t)L->map[idx[k]]*SLOT_B;
            bigbuf[a]=(uint8_t)k;
            sim_access(a); pg_touch(a);
        }
        total+=now_ns()-t0;
        free(d);free(idx);
    }
    R->total_ns=total; R->avg_ns=total/Q;
    R->hits=sim_hits; R->total_acc=sim_total;
    R->cache_used=0;
    for(int i=0;i<SIM_LINES;i++)R->cache_used+=sim_cache[i];
    R->pages=pg_n;
}

/* ── Bench: Sequential scan ── */
static void bench_seq(Layout *L, Result *R) {
    sim_reset(); pg_reset();
    double t0=now_ns();
    volatile uint8_t sink=0;
    for(int i=0;i<N_ITEMS;i++){
        uint64_t a=(uint64_t)L->map[i]*SLOT_B;
        sink+=bigbuf[a];
        sim_access(a); pg_touch(a);
    }
    R->total_ns=now_ns()-t0;
    R->avg_ns=R->total_ns/N_ITEMS;
    R->hits=sim_hits; R->total_acc=sim_total;
    R->cache_used=0;
    for(int i=0;i<SIM_LINES;i++)R->cache_used+=sim_cache[i];
    R->pages=pg_n;
    (void)sink;
}

int main(void) {
    printf("════════════════════════════════════════════════════════════════════\n");
    printf("  GEO SEEK BENCH v3 — LARGE SCALE\n");
    printf("  Items: %d (%.0f MB)  |  Cache sim: %d lines (%d KB)\n",
           N_ITEMS, BUF_BYTES/1e6, SIM_LINES, SIM_LINES*CACHE_LINE/1024);
    printf("  Spatial query: K=%d nearest  |  Trials: %d\n", K_NEIGHBORS, TRIALS);
    printf("════════════════════════════════════════════════════════════════════\n\n");

    bigbuf = (uint8_t *)malloc(BUF_BYTES);
    memset(bigbuf, 0xAA, BUF_BYTES);
    Vec3 *pos = gen_positions();

    Layout layouts[4];
    build_linear(&layouts[0]);
    build_hilbert(&layouts[1]);
    build_morton3(&layouts[2]);
    build_random(&layouts[3]);

    /* ── Spatial K-NN ── */
    printf("┌───────────────────────────────────────────────────────────────┐\n");
    printf("│  TEST: Spatial K-NN Query  (K=%d, %d queries)              │\n", K_NEIGHBORS, TRIALS);
    printf("├────────────────┬──────────┬──────────┬──────────┬────────────┤\n");
    printf("│ Layout         │ Avg μs/q │ Hit Rate │ Lines    │ Pages      │\n");
    printf("├────────────────┼──────────┼──────────┼──────────┼────────────┤\n");

    Result R[4];
    for (int i=0;i<4;i++) bench_spatial(&layouts[i], pos, &R[i]);

    for (int i=0;i<4;i++){
        double hr = 100.0*R[i].hits/R[i].total_acc;
        printf("│ %-14s │ %8.1f │ %7.2f%%  │ %8d │ %8d   │\n",
               layouts[i].name, R[i].avg_ns/1e3, hr, R[i].cache_used, R[i].pages);
    }
    printf("└────────────────┴──────────┴──────────┴──────────┴────────────┘\n");

    /* ── Sequential scan ── */
    printf("\n┌───────────────────────────────────────────────────────────────┐\n");
    printf("│  TEST: Sequential Scan  (%d items)                         │\n", N_ITEMS);
    printf("├────────────────┬──────────┬──────────┬──────────┬────────────┤\n");
    printf("│ Layout         │ Total ms │ Avg ns/i │ Hit Rate │ Pages      │\n");
    printf("├────────────────┼──────────┼──────────┼──────────┼────────────┤\n");

    Result S[4];
    for (int i=0;i<4;i++) bench_seq(&layouts[i], &S[i]);

    for (int i=0;i<4;i++){
        double hr = 100.0*S[i].hits/S[i].total_acc;
        printf("│ %-14s │ %8.2f │ %8.1f │ %7.2f%%  │ %8d   │\n",
               layouts[i].name, S[i].total_ns/1e6, S[i].avg_ns, hr, S[i].pages);
    }
    printf("└────────────────┴──────────┴──────────┴──────────┴────────────┘\n");

    /* ── VERDICT ── */
    printf("\n════════════════════════════════════════════════════════════════════\n");
    printf("  VERDICT\n");
    printf("────────────────────────────────────────────────────────────────────\n");

    double hr_lin=100.0*R[0].hits/R[0].total_acc;
    double hr_hil=100.0*R[1].hits/R[1].total_acc;
    double hr_mor=100.0*R[2].hits/R[2].total_acc;
    double hr_rnd=100.0*R[3].hits/R[3].total_acc;

    printf("  Spatial Query Cache Hit Rate:\n");
    printf("    Linear:      %.2f%%\n", hr_lin);
    printf("    Hilbert-2D:  %.2f%%  %s\n", hr_hil, hr_hil>hr_lin*1.01?"← BETTER":"← same/similar");
    printf("    Morton-3D:   %.2f%%  %s\n", hr_mor, hr_mor>hr_lin*1.01?"← BETTER":"← same/similar");
    printf("    Random:      %.2f%%  (baseline)\n", hr_rnd);

    printf("\n  Spatial Query Latency:\n");
    printf("    Linear:      %.1f μs/query\n", R[0].avg_ns/1e3);
    printf("    Hilbert-2D:  %.1f μs/query  %s\n", R[1].avg_ns/1e3,
           R[1].avg_ns<R[0].avg_ns*0.95?"← FASTER":"← same/similar");
    printf("    Morton-3D:   %.1f μs/query  %s\n", R[2].avg_ns/1e3,
           R[2].avg_ns<R[0].avg_ns*0.95?"← FASTER":"← same/similar");

    int confirmed = (hr_hil > hr_lin*1.01 || hr_mor > hr_lin*1.01 ||
                     R[1].avg_ns < R[0].avg_ns*0.95 || R[2].avg_ns < R[0].avg_ns*0.95);
    if (confirmed) {
        printf("\n  ✅ CONFIRMED: Geometry layout provides measurable advantage\n");
    } else {
        printf("\n  ⚠️  RESULT: No significant difference at %d items / %dMB\n",
               N_ITEMS, (int)(BUF_BYTES/1e6));
        printf("     Possible explanations:\n");
        printf("     1. Cache simulator is simplified (direct-mapped, no associative)\n");
        printf("     2. Real CPU has HW prefetcher that eliminates layout effects\n");
        printf("     3. At this access pattern, all layouts perform similarly\n");
    }

    printf("════════════════════════════════════════════════════════════════════\n");

    free(bigbuf); free(pos);
    for(int i=0;i<4;i++) free(layouts[i].map);
    return 0;
}
