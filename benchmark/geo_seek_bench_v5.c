/*
 * geo_seek_bench_v5.c — Clean benchmark: 4 metrics only
 *
 * Optimized: pre-compute cone membership per query, then measure access.
 * Access pattern: CLUSTERED RANDOM (real GGUF weight lookup pattern).
 *
 * Build: gcc -O2 -Wall -o geo_seek_bench_v5.exe geo_seek_bench_v5.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define N_ITEMS     (32768)        /* 32K items = 2MB */
#define SLOT_B      (64)
#define BUF_BYTES   ((uint64_t)N_ITEMS * SLOT_B)
#define CACHE_LINE  (64)
#define PAGE_B      (4096)
#define N_LINES     (BUF_BYTES / CACHE_LINE)
#define N_PAGES     (BUF_BYTES / PAGE_B + 1)
#define N_QUERIES   (500)
#define CONE_DEG    (15.0)
#define SEED        (42)

static uint8_t *buf;
typedef struct { float x, y, z; } Vec3;

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

static uint32_t rng_s;
static uint32_t rng(void){rng_s^=rng_s<<13;rng_s^=rng_s>>17;rng_s^=rng_s<<5;return rng_s;}

/* Hilbert 2D */
#define H_N 182  /* 182x182 = 33124 >= 32768 */
static void hrot(int n,int*x,int*y,int rx,int ry){
    if(ry==0){if(rx==1){*x=n-1-*x;*y=n-1-*y;}int t=*x;*x=*y;*y=t;}
}
static int h_xy2d(int n,int x,int y){
    int rx,ry,s,d=0;
    for(s=n/2;s>0;s/=2){rx=(x&s)>0;ry=(y&s)>0;d+=s*s*((3*rx)^ry);hrot(s,&x,&y,rx,ry);}
    return d;
}

/* Morton 3D */
static uint32_t p1(uint32_t n){n=(n|(n<<8))&0xFF00FF;n=(n|(n<<4))&0xF0F0F0F;n=(n|(n<<2))&0x33333333;n=(n|(n<<1))&0x55555555;return n;}
static uint64_t morton3(uint32_t x,uint32_t y,uint32_t z){return((uint64_t)p1(x)<<2)|((uint64_t)p1(y)<<1)|p1(z);}

typedef struct{int*map;char*name;}Layout;

static void build_linear(Layout *L){
    L->map=malloc(N_ITEMS*sizeof(int));L->name="Linear";
    for(int i=0;i<N_ITEMS;i++)L->map[i]=i;
}
static void build_hilbert(Layout *L){
    L->map=malloc(N_ITEMS*sizeof(int));L->name="Hilbert-2D";
    int *d=malloc(N_ITEMS*sizeof(int)),*o=malloc(N_ITEMS*sizeof(int));
    for(int i=0;i<N_ITEMS;i++){d[i]=h_xy2d(H_N,i%H_N,i/H_N);o[i]=i;}
    for(int i=1;i<N_ITEMS;i++){int kd=d[i],ko=o[i],j=i-1;while(j>=0&&d[j]>kd){d[j+1]=d[j];o[j+1]=o[j];j--;}d[j+1]=kd;o[j+1]=ko;}
    for(int i=0;i<N_ITEMS;i++)L->map[o[i]]=i;
    free(d);free(o);
}
static void build_morton3(Layout *L){
    L->map=malloc(N_ITEMS*sizeof(int));L->name="Morton-3D";
    int GR=32;int *d=malloc(N_ITEMS*sizeof(int)),*o=malloc(N_ITEMS*sizeof(int));
    for(int i=0;i<N_ITEMS;i++){int x=i%GR,y=(i/GR)%GR,z=i/(GR*GR);d[i]=(int)morton3(x,y,z);o[i]=i;}
    for(int i=1;i<N_ITEMS;i++){int kd=d[i],ko=o[i],j=i-1;while(j>=0&&d[j]>kd){d[j+1]=d[j];o[j+1]=o[j];j--;}d[j+1]=kd;o[j+1]=ko;}
    for(int i=0;i<N_ITEMS;i++)L->map[o[i]]=i;
    free(d);free(o);
}
static void build_random(Layout *L){
    L->map=malloc(N_ITEMS*sizeof(int));L->name="Random";
    for(int i=0;i<N_ITEMS;i++)L->map[i]=i;
    rng_s=99;for(int i=N_ITEMS-1;i>0;i--){int j=rng()%(i+1);int t=L->map[i];L->map[i]=L->map[j];L->map[j]=t;}
}

static Vec3 *gen_pos(void){
    Vec3 *p=malloc(N_ITEMS*sizeof(Vec3));
    for(int i=0;i<N_ITEMS;i++){
        double y=1.0-2.0*((double)i/(N_ITEMS-1));
        double r=sqrt(1.0-y*y);double th=2.39996322972865332*i;
        p[i].x=(float)(r*cos(th));p[i].y=(float)y;p[i].z=(float)(r*sin(th));
    }
    return p;
}

/* Trackers */
static uint8_t *cl_touched;static uint32_t *cl_count;
static uint8_t *pg_touched;static int cl_n,pg_n;

static void trackers_reset(void){
    memset(cl_touched,0,N_LINES);memset(cl_count,0,N_LINES*sizeof(uint32_t));
    memset(pg_touched,0,N_PAGES);cl_n=pg_n=0;
}
static void track(uint64_t addr){
    uint64_t cl=addr/CACHE_LINE;uint32_t pg=(uint32_t)(addr/PAGE_B);
    if(!cl_touched[cl]){cl_touched[cl]=1;cl_n++;}cl_count[cl]++;
    if(!pg_touched[pg]){pg_touched[pg]=1;pg_n++;}
}

/* Pre-computed query: which items to access */
typedef struct{int*idx;int n;double cos_th;}Query;

static void build_queries(Vec3 *pos, Query *Q){
    double cos_th=cos(CONE_DEG*M_PI/180.0);
    /* Pre-allocate max possible per query */
    int max_per=(int)(N_ITEMS*CONE_DEG/360.0*2);/* rough upper bound */
    rng_s=SEED;
    for(int q=0;q<N_QUERIES;q++){
        int c=rng()%N_ITEMS;
        Vec3 cv=pos[c];
        Q[q].idx=malloc(max_per*sizeof(int));
        Q[q].n=0;Q[q].cos_th=cos_th;
        for(int i=0;i<N_ITEMS;i++){
            double dot=cv.x*pos[i].x+cv.y*pos[i].y+cv.z*pos[i].z;
            if(dot>cos_th)Q[q].idx[Q[q].n++]=i;
        }
        /* Shuffle access order within cone (realistic: not sequential) */
        for(int i=Q[q].n-1;i>0;i--){
            int j=rng()%i;int t=Q[q].idx[i];Q[q].idx[i]=Q[q].idx[j];Q[q].idx[j]=t;
        }
    }
}

/* Latency array */
typedef struct{double*d;int n,c;}LA;
static void la_init(LA*a,int c){a->d=malloc(c*sizeof(double));a->n=0;a->c=c;}
static void la_push(LA*a,double v){if(a->n<a->c)a->d[a->n++]=v;}
static void la_sort(LA*a){
    for(int i=1;i<a->n;i++){double k=a->d[i];int j=i-1;while(j>=0&&a->d[j]>k){a->d[j+1]=a->d[j];j--;}a->d[j+1]=k;}
}
static double la_avg(LA*a){double s=0;for(int i=0;i<a->n;i++)s+=a->d[i];return s/a->n;}
static double la_p95(LA*a){return a->d[(int)(a->n*0.95)];}
static double la_p99(LA*a){return a->d[(int)(a->n*0.99)];}
static double la_max(LA*a){return a->d[a->n-1];}

typedef struct{
    double avg_ns,p95_ns,p99_ns,max_ns,total_ms;
    int cache_lines_used,page_faults,total_accesses;
    double miss_rate,reuse;
}Result;

static void bench(Layout*L,Vec3*pos,Query*Q,Result*R){
    trackers_reset();
    LA lat;la_init(&lat,N_QUERIES*2000);
    double t0=now_ns();

    for(int q=0;q<N_QUERIES;q++){
        for(int k=0;k<Q[q].n;k++){
            uint64_t addr=(uint64_t)L->map[Q[q].idx[k]]*SLOT_B;
            double ta=now_ns();
            buf[addr]=(uint8_t)q;
            double tb=now_ns();
            track(addr);
            la_push(&lat,tb-ta);
        }
    }

    R->total_ms=(now_ns()-t0)/1e6;
    la_sort(&lat);
    R->avg_ns=la_avg(&lat);R->p95_ns=la_p95(&lat);R->p99_ns=la_p99(&lat);
    R->max_ns=la_max(&lat);R->total_accesses=lat.n;
    R->cache_lines_used=cl_n;R->page_faults=pg_n;
    R->miss_rate=(double)cl_n/lat.n*100;
    uint64_t tt=0;for(uint64_t i=0;i<N_LINES;i++)tt+=cl_count[i];
    R->reuse=(double)tt/cl_n;
    free(lat.d);
}

int main(void){
    printf("══════════════════════════════════════════════════════════════════════════════════\n");
    printf("  GEO SEEK BENCH v5 — 4 Metrics: Seek Time · Cache Miss · Page Fault · Latency\n");
    printf("  Items: %d (%.1f MB)  |  Cache line: %dB  |  Page: %dB\n",
           N_ITEMS,BUF_BYTES/1e6,CACHE_LINE,PAGE_B);
    printf("  Queries: %d  |  Cone: %.0f°  |  Pattern: Clustered Random (shuffled)\n",
           N_QUERIES,CONE_DEG);
    printf("══════════════════════════════════════════════════════════════════════════════════\n\n");

    buf=malloc(BUF_BYTES);memset(buf,0xAA,BUF_BYTES);
    Vec3 *pos=gen_pos();
    cl_touched=calloc(N_LINES,1);cl_count=calloc(N_LINES,sizeof(uint32_t));
    pg_touched=calloc(N_PAGES,1);

    Layout L[4];
    build_linear(&L[0]);build_hilbert(&L[1]);
    build_morton3(&L[2]);build_random(&L[3]);

    Query *Q=malloc(N_QUERIES*sizeof(Query));
    build_queries(pos,Q);

    /* Report cone sizes */
    int total_acc=0;
    for(int q=0;q<N_QUERIES;q++)total_acc+=Q[q].n;
    printf("  Cone membership: avg %.0f items/query, total %d accesses\n\n",
           (double)total_acc/N_QUERIES,total_acc);

    Result R[4];
    for(int i=0;i<4;i++)bench(&L[i],pos,Q,&R[i]);

    /* TABLE 1: Seek Time */
    printf("┌──────────────────────────────────────────────────────────────────────────┐\n");
    printf("│  1. SEEK TIME (per-access latency, nanoseconds)                        │\n");
    printf("├────────────────┬──────────┬──────────┬──────────┬──────────┬───────────┤\n");
    printf("│ Layout         │ Avg ns   │ P95 ns   │ P99 ns   │ Max ns   │ Total ms  │\n");
    printf("├────────────────┼──────────┼──────────┼──────────┼──────────┼───────────┤\n");
    for(int i=0;i<4;i++){
        printf("│ %-14s │ %8.1f │ %8.0f │ %8.0f │ %8.0f │ %9.1f │\n",
               L[i].name,R[i].avg_ns,R[i].p95_ns,R[i].p99_ns,R[i].max_ns,R[i].total_ms);
    }
    printf("└────────────────┴──────────┴──────────┴──────────┴──────────┴───────────┘\n");

    /* TABLE 2: Cache Miss */
    printf("\n┌──────────────────────────────────────────────────────────────────────────┐\n");
    printf("│  2. CACHE BEHAVIOR (cache line = %dB, total %d lines = %.0f KB)        │\n",
           CACHE_LINE,(int)N_LINES,N_LINES*CACHE_LINE/1024.0);
    printf("├────────────────┬──────────┬──────────┬──────────┬──────────────────────┤\n");
    printf("│ Layout         │ Lines    │ Miss Rate│ Reuse    │ Total Accesses       │\n");
    printf("├────────────────┼──────────┼──────────┼──────────┼──────────────────────┤\n");
    for(int i=0;i<4;i++){
        printf("│ %-14s │ %8d │ %7.2f%% │ %6.2fx  │ %20d │\n",
               L[i].name,R[i].cache_lines_used,R[i].miss_rate,R[i].reuse,R[i].total_accesses);
    }
    printf("└────────────────┴──────────┴──────────┴──────────┴──────────────────────┘\n");

    /* TABLE 3: Page Fault */
    printf("\n┌──────────────────────────────────────────────────────────────────────────┐\n");
    printf("│  3. PAGE FAULTS (distinct 4KB pages touched)                           │\n");
    printf("├────────────────┬──────────┬─────────────────────────────────────────────┤\n");
    printf("│ Layout         │ Pages    │ Memory Footprint                            │\n");
    printf("├────────────────┼──────────┼─────────────────────────────────────────────┤\n");
    for(int i=0;i<4;i++){
        printf("│ %-14s │ %8d │ %8.1f MB                                    │\n",
               L[i].name,R[i].page_faults,R[i].page_faults*4.0/1024);
    }
    printf("└────────────────┴──────────┴─────────────────────────────────────────────┘\n");

    /* TABLE 4: Latency Distribution */
    printf("\n┌──────────────────────────────────────────────────────────────────────────┐\n");
    printf("│  4. LATENCY DISTRIBUTION (ns) — vs Linear baseline                     │\n");
    printf("├────────────────┬──────────┬──────────┬──────────┬──────────────────────┤\n");
    printf("│ Layout         │ Avg      │ P95      │ P99      │ Delta vs Linear      │\n");
    printf("├────────────────┼──────────┼──────────┼──────────┼──────────────────────┤\n");
    for(int i=0;i<4;i++){
        const char *tag="(baseline)";
        if(i>0){
            double d=(R[i].avg_ns-R[0].avg_ns)/R[0].avg_ns*100;
            static char buf2[32];sprintf(buf2,"%+.1f%%",d);
            tag=buf2;
        }
        printf("│ %-14s │ %8.1f │ %8.0f │ %8.0f │ %-20s│\n",
               L[i].name,R[i].avg_ns,R[i].p95_ns,R[i].p99_ns,tag);
    }
    printf("└────────────────┴──────────┴──────────┴──────────┴──────────────────────┘\n");

    /* VERDICT */
    printf("\n══════════════════════════════════════════════════════════════════════════════════\n");
    printf("  VERDICT\n");
    printf("──────────────────────────────────────────────────────────────────────────────────\n");

    int best=0;
    for(int i=1;i<4;i++)if(R[i].avg_ns<R[best].avg_ns)best=i;
    double spd=(R[0].avg_ns-R[best].avg_ns)/R[0].avg_ns*100;

    printf("  🏆 Fastest: %s (%.1f ns avg)\n",L[best].name,R[best].avg_ns);
    if(best!=0)
        printf("  ✅ %s is %.1f%% faster than Linear for clustered access\n",L[best].name,spd);
    else
        printf("  ⚠️  Linear is fastest — geometry advantage may appear at larger scale\n");

    printf("\n  Summary:\n");
    for(int i=0;i<4;i++){
        double d=(R[i].avg_ns-R[0].avg_ns)/R[0].avg_ns*100;
        printf("    %-14s  Seek: %7.1f ns  Cache miss: %5.2f%%  Pages: %5d  %s\n",
               L[i].name,R[i].avg_ns,R[i].miss_rate,R[i].page_faults,
               i==0?"(baseline)":(d<0?"← FASTER":d>0?"← slower":"≈"));
    }
    printf("══════════════════════════════════════════════════════════════════════════════════\n");

    free(buf);free(pos);
    for(int i=0;i<4;i++)free(L[i].map);
    for(int q=0;q<N_QUERIES;q++)free(Q[q].idx);free(Q);
    free(cl_touched);free(cl_count);free(pg_touched);
    return 0;
}
