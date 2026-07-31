// silk_screen_encoder.c
// Silk Screen Weight Encoder — Clock-only, Identity Filter, GGUF Read
//
// Architecture:
//   10 boxes (0-9) × 6 directions (+X,-X,+Y,-Y,+Z,-Z) × 1440 ticks
//   = 86,400 weight slots per layer
//   filter[box][dir][tick] = weight  (IDENTITY — lossless)
//   clock = simple counter 0..1439 (NOT stride-37)
//
// Compile: gcc -O2 -std=c11 -Wall -Werror -o silk_screen_encoder.exe silk_screen_encoder.c -lm
// Run:     silk_screen_encoder.exe [model.gguf]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <math.h>
#include <time.h>

#define N_BOXES    10
#define N_DIRS     6
#define CLOCK_MAX  1440
#define LAYER_SLOTS (N_BOXES * N_DIRS * CLOCK_MAX)
#define Q8_MIN     (-128)
#define Q8_MAX     127
#define Q8_RANGE   256
#define GGUF_MAGIC 0x46554747u
#define MAX_TENSORS 256

static const char *DIR_NAMES[N_DIRS] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};

// ── Silk Screen ─────────────────────────────────────────────
typedef struct {
    int8_t filter[N_BOXES][N_DIRS][CLOCK_MAX];
    int    n_ticks, n_layers;
    char   model_name[64];
} SilkScreen;

static void silk_init(SilkScreen *s, const char *name) {
    memset(s, 0, sizeof(SilkScreen));
    s->n_ticks = CLOCK_MAX;
    s->n_layers = 1;
    if (name) strncpy(s->model_name, name, 63);
}

static int8_t silk_decode(const SilkScreen *s, int box, int dir, int tick) {
    if (box < 0 || box >= N_BOXES || dir < 0 || dir >= N_DIRS) return 0;
    return s->filter[box][dir][tick % s->n_ticks];
}

// ── GGUF Reader ─────────────────────────────────────────────
typedef struct {
    uint32_t version;
    uint64_t n_tensors;
    char     *names[MAX_TENSORS];
    uint32_t dtypes[MAX_TENSORS];
    uint64_t offsets[MAX_TENSORS], sizes[MAX_TENSORS];
    uint64_t n_dims[MAX_TENSORS], dims[MAX_TENSORS][8];
} GGUFFile;

static FILE *gguf_fopen(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) return f;
    if (path[0] == '/' && path[2] == '/') {
        char w[512];
        snprintf(w, sizeof(w), "%c:\\%s", path[1], path + 3);
        for (char *p = w; *p; p++) if (*p == '/') *p = '\\';
        f = fopen(w, "rb");
    }
    return f;
}

static int gguf_open(const char *path, GGUFFile *gf) {
    memset(gf, 0, sizeof(GGUFFile));
    FILE *f = gguf_fopen(path);
    if (!f) return -1;

    uint32_t magic, ver; uint64_t nt, nkv;
    fread(&magic, 4, 1, f); fread(&ver, 4, 1, f);
    fread(&nt, 8, 1, f);    fread(&nkv, 8, 1, f);
    if (magic != GGUF_MAGIC) { fclose(f); return -1; }
    gf->version = ver;
    gf->n_tensors = nt < MAX_TENSORS ? nt : MAX_TENSORS;

    for (uint64_t i = 0; i < nkv; i++) {
        uint64_t kl; fread(&kl, 8, 1, f); fseek(f, kl, SEEK_CUR);
        uint32_t vt; fread(&vt, 4, 1, f);
        switch (vt) {
            case 0: case 1: fseek(f, 1, SEEK_CUR); break;
            case 2: case 3: fseek(f, 2, SEEK_CUR); break;
            case 4: case 5: case 6: fseek(f, 4, SEEK_CUR); break;
            case 7: fseek(f, 1, SEEK_CUR); break;
            case 8: { uint64_t sl; fread(&sl, 8, 1, f); fseek(f, sl, SEEK_CUR); break; }
            case 9: {
                uint32_t at; uint64_t al;
                fread(&at, 4, 1, f); fread(&al, 8, 1, f);
                if (at == 8) for (uint64_t j=0;j<al;j++){uint64_t s;fread(&s,8,1,f);fseek(f,s,SEEK_CUR);}
                else fseek(f, al*(at<=3?2:at<=6?4:1), SEEK_CUR);
                break;
            }
            case 10: case 11: case 12: fseek(f, 8, SEEK_CUR); break;
            case 18: case 19: fseek(f, 2, SEEK_CUR); break;
            default: fseek(f, 4, SEEK_CUR); break;
        }
    }

    for (uint64_t i = 0; i < gf->n_tensors; i++) {
        uint64_t nl; fread(&nl, 8, 1, f);
        gf->names[i] = (char*)calloc(nl+1, 1);
        if (!gf->names[i]) {
            printf("names alloc failed\n");
            fclose(f);
            return 1;
        }
        fread(gf->names[i], nl, 1, f);
        uint32_t nd; fread(&nd, 4, 1, f);
        gf->n_dims[i] = nd;
        for (uint32_t j=0; j<nd && j<8; j++) fread(&gf->dims[i][j], 8, 1, f);
        fread(&gf->dtypes[i], 4, 1, f);
        fread(&gf->offsets[i], 8, 1, f);
    }

    for (uint64_t i = 0; i < gf->n_tensors; i++) {
        if (i+1 < gf->n_tensors) gf->sizes[i] = gf->offsets[i+1] - gf->offsets[i];
        else { long c=ftell(f); fseek(f,0,SEEK_END); gf->sizes[i]=ftell(f)-gf->offsets[i]; fseek(f,c,SEEK_SET); }
    }
    fclose(f);
    return 0;
}

static void gguf_close(GGUFFile *gf) {
    for (uint64_t i=0; i<gf->n_tensors; i++) free(gf->names[i]);
}

// ── Bake (identity, lossless) ───────────────────────────────
typedef struct {
    uint64_t total_weights;
    int      n_layers;
    double   encode_ms;
    int      exact_matches, worst_error;
    size_t   raw_bytes, silk_bytes;
    double   ratio;
} BakeResult;

static BakeResult bake(SilkScreen *s, const int8_t *data, uint64_t n) {
    BakeResult r = {0};
    r.total_weights = n;
    r.n_layers = (int)((n + LAYER_SLOTS - 1) / LAYER_SLOTS);
    r.raw_bytes = n;
    r.silk_bytes = sizeof(SilkScreen) * r.n_layers;

    clock_t t0 = clock();
    uint64_t fill = n > LAYER_SLOTS ? LAYER_SLOTS : n;
    for (uint64_t i = 0; i < fill; i++)
        s->filter[i%N_BOXES][(i/N_BOXES)%N_DIRS][(i/(N_BOXES*N_DIRS))%CLOCK_MAX] = data[i];
    s->n_ticks = CLOCK_MAX;
    clock_t t1 = clock();
    r.encode_ms = (double)(t1-t0)/CLOCKS_PER_SEC*1000.0;

    uint64_t exact=0, worst=0;
    for (uint64_t i = 0; i < fill; i++) {
        int8_t d = s->filter[i%N_BOXES][(i/N_BOXES)%N_DIRS][(i/(N_BOXES*N_DIRS))%CLOCK_MAX];
        uint64_t e = (uint64_t)abs((int)data[i]-(int)d);
        if (e==0) exact++;
        if (e>worst) worst=e;
    }
    r.exact_matches = (int)exact;
    r.worst_error = (int)worst;
    r.ratio = r.silk_bytes>0 ? (double)r.raw_bytes/r.silk_bytes : 0;
    return r;
}

// ── Tests ───────────────────────────────────────────────────
static int test_silk_synthetic(void) {
    printf("=== Test 1: Synthetic Identity Check ===\n");
    SilkScreen silk; silk_init(&silk, "synthetic");
    int8_t data[LAYER_SLOTS];
    for (uint64_t i=0; i<LAYER_SLOTS; i++) data[i]=(int8_t)((i*37+13)%Q8_RANGE-128);
    BakeResult r = bake(&silk, data, LAYER_SLOTS);
    printf("  Slots: %d  Exact: %d/%I64u  Worst: %d  Time: %.3fms\n",
           LAYER_SLOTS, r.exact_matches, (unsigned long long)r.total_weights,
           r.worst_error, r.encode_ms);
    printf("  => %s\n\n", r.worst_error==0 ? "PASS" : "FAIL");
    return r.worst_error==0;
}

static int test_silk_gguf(const char *path) {
    printf("=== Test 2: GGUF Real Model Bake ===\n  %s\n", path);
    GGUFFile gf;
    if (gguf_open(path, &gf)<0) { printf("  FAIL: open\n\n"); return 0; }

    int best=-1; uint64_t best_n=0;
    for (uint64_t i=0; i<gf.n_tensors; i++) {
        uint64_t n=0;
        switch(gf.dtypes[i]) {
            case 8: n=gf.sizes[i]/34*32; break;
            case 0: n=gf.sizes[i]/4; break;
            case 1: n=gf.sizes[i]/2; break;
            default: n=gf.sizes[i]; break;
        }
        if (n>=720 && n>best_n) { best=(int)i; best_n=n; }
    }
    if (best<0) { printf("  FAIL: no tensor\n\n"); gguf_close(&gf); return 0; }

    printf("  tensor[%d]: %s  dtype=%u  ~%I64u int8\n",
           best, gf.names[best], gf.dtypes[best], (unsigned long long)best_n);

    uint64_t cap = LAYER_SLOTS;
    if (cap > gf.sizes[best]) cap = gf.sizes[best];
    int8_t *buf = (int8_t*)calloc(cap+256, 1);
    FILE *f = gguf_fopen(path);
    if (!f) { free(buf); gguf_close(&gf); return 0; }
    fseek(f, gf.offsets[best], SEEK_SET);

    int nr=0; uint32_t dt=gf.dtypes[best];
    if (dt==8) {
        for (uint64_t b=0; b<gf.sizes[best]/34 && nr<(int)cap; b++) {
            fseek(f,2,SEEK_CUR);
            for (int j=0;j<32&&nr<(int)cap;j++) fread(&buf[nr++],1,1,f);
        }
    } else if (dt==0) {
        for (uint64_t i=0; i<gf.sizes[best]/4 && nr<(int)cap; i++) {
            float v; fread(&v,4,1,f); buf[nr++]=(int8_t)(v*127.0f);
        }
    } else { nr=(int)fread(buf,1,cap,f); }
    fclose(f);

    if (nr<720) { printf("  FAIL: %d values\n\n",nr); free(buf); gguf_close(&gf); return 0; }
    printf("  Read %d int8 values\n", nr);

    SilkScreen silk; silk_init(&silk, gf.names[best]);
    BakeResult r = bake(&silk, buf, nr);

    printf("\n  ---- Results ----\n");
    printf("  Baked:    %I64u weights, %d layers\n", (unsigned long long)r.total_weights, r.n_layers);
    printf("  Exact:    %d/%I64u (100%%=lossless)  Worst: %d\n",
           r.exact_matches, (unsigned long long)r.total_weights, r.worst_error);
    printf("  Storage:  %I64u bytes silk, %I64u bytes raw  Ratio: %.4f:1\n",
           (unsigned long long)r.silk_bytes, (unsigned long long)r.raw_bytes, r.ratio);
    printf("  Time:     %.3f ms\n", r.encode_ms);

    printf("\n  Sample (box=%d, tick=0..2):\n", N_BOXES/2);
    for (int t=0; t<3; t++) {
        printf("    t=%d:", t);
        for (int d=0;d<N_DIRS;d++) printf(" %s=%d", DIR_NAMES[d], silk_decode(&silk,5,d,t));
        printf("\n");
    }

    printf("\n  Verify:\n");
    uint64_t ve=0,vt=0;
    for (uint64_t i=0; i<(uint64_t)nr && i<LAYER_SLOTS; i++) {
        if (silk_decode(&silk,i%N_BOXES,(i/N_BOXES)%N_DIRS,(i/(N_BOXES*N_DIRS))%CLOCK_MAX)==buf[i]) ve++;
        vt++;
    }
    printf("  %I64u/%I64u exact (%.1f%%) — %s\n",
           (unsigned long long)ve,(unsigned long long)vt,
           100.0*ve/(vt?vt:1), ve==vt?"LOSSLESS ✓":"BUG!");
    printf("  => %s\n\n", ve==vt?"PASS":"FAIL");

    free(buf); gguf_close(&gf);
    return ve==vt;
}

static void test_scale(void) {
    printf("=== Scale Analysis ===\n");
    printf("  Layer: %d slots (10×6×1440)\n\n", LAYER_SLOTS);
    struct { const char *n; uint64_t w; } M[] = {
        {"Qwen3-0.6B",600000000ULL},{"Qwen2.5-3B",3000000000ULL},
        {"Qwen2.5-7B",7000000000ULL},{"Qwen2.5-14B",14000000000ULL},
        {"Qwen2.5-30B",30000000000ULL},{"Qwen2.5-72B",72000000000ULL},
    };
    printf("  %-16s %12s %8s %10s\n","Model","Weights","Layers","Silk(GB)");
    for (int i=0;i<6;i++) {
        int l=(int)((M[i].w+LAYER_SLOTS-1)/LAYER_SLOTS);
        printf("  %-16s %12I64u %8d %10.2f\n",
               M[i].n,(unsigned long long)M[i].w,l,(double)l*sizeof(SilkScreen)/1e9);
    }
    printf("  Value = structured access (60 read heads/tick), NOT compression\n\n");
}

int main(int argc, char **argv) {
    printf("============================================================\n");
    printf("  Silk Screen Encoder — Clock-only, Identity, Lossless\n");
    printf("============================================================\n\n");

    int ok=0, tot=0;
    tot++; ok+=test_silk_synthetic();
    if (argc>=2) { tot++; ok+=test_silk_gguf(argv[1]); }
    else printf("[SKIP] No GGUF path. Usage: silk_screen_encoder.exe model.gguf\n\n");
    test_scale();

    printf("RESULT: %d/%d PASS\n", ok, tot);
    return ok==tot?0:1;
}
