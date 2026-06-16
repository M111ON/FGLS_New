/*
 * llama_pogls_runner_sid_v2.c — SID-aware inference runner with inline tensor scan
 * Build: gcc -O2 -std=c11 -I. -I../collection -I../collection/src -I../collection/core -I../collection/core/core -I../collection/geopixel -I../collection/geopixel/Metatron/core -I../collection/pogls_engine -II:/llama.cpp/include -II:/llama.cpp/ggml/include -o llama_pogls_runner_sid_v2.exe llama_pogls_runner_sid_v2.c I:/llama/llama-b9528-bin-win-vulkan-x64/llama.dll I:/llama/llama-b9528-bin-win-vulkan-x64/ggml.dll I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-base.dll I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-x64.dll -lm
 * Usage: llama_pogls_runner_sid_v2.exe model.gguf [options]
 *
 * KEY DIFFERENCES from v1:
 *   - No DLL dependency (no LoadLibrary/GetProcAddress for sid_tensor_helper.dll)
 *   - Inline memory scan for tensor pointers (from test_swap_all.c)
 *   - Per-decode SID swap: swap tensor->data BETWEEN llama_decode calls
 *   - --sid-face N: select SID face (0=disabled, 1=swap output.weight to cache)
 *
 * Scan flow (after model load):
 *   1. Open GGUF → get tensor names list
 *   2. VirtualQuery → model allocation base
 *   3. Tier 1: scan model struct (64KB) for tensor ptrs matching GGUF names
 *   4. Tier 2: find layers pointer in model struct
 *   5. Tier 3: scan layers heap allocation for remaining tensors
 *   6. Store all found tensor ptrs → per-decode swap
 *
 * SID swap pattern (proven by test_swap_all.c):
 *   swap tensor->data → llama_decode → restore tensor->data
 */

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #define CLOCK_MONOTONIC 0
  typedef struct{long tv_sec;long tv_nsec;}PoglsTime;
  static inline int clock_gettime(int _clk,PoglsTime *ts){
      LARGE_INTEGER f,c;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&c);
      ts->tv_sec=(long)(c.QuadPart/f.QuadPart);
      ts->tv_nsec=(long)(c.QuadPart%f.QuadPart*1000000000LL/f.QuadPart);return 0;}
#else
  #include <time.h>
  typedef struct timespec PoglsTime;
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include "llama.h"
#include "gguf_index.h"
#include "sid_cache.h"
#include "sid_loader.h"
#include "bond_discovery.h"
#include "sid_delta_ring.h"
#include "sid_timetravel.h"

#define MAX_TOKENS_CACHE 4096
#define MAX_CHAT_HISTORY 128
#define MAX_LINE 4096
#define MAX_TENSORS  4096
#define NAME_MAX 64
#define SCAN_LIMIT 0x200000
#define MAX_SID_SWAPS 512
#define SAVE_LOGITS 15

typedef struct{
    float temp,top_p,repeat_penalty;int top_k,repeat_last_n;
    int tokens[MAX_TOKENS_CACHE],count;
}Sampler;

typedef struct{float p;int idx;}SIDProbPair;

static int prob_cmp_desc(const void*a,const void*b){
    float x=((const SIDProbPair*)a)->p,y=((const SIDProbPair*)b)->p;
    return (x>y)?-1:(x<y)?1:0;
}

static int sample_token(const float*raw,int n,Sampler*sp){
    float*p=(float*)malloc((size_t)n*4);memcpy(p,raw,(size_t)n*4);
    if(sp->repeat_penalty!=1.0f&&sp->count>0){
        int s=sp->count>sp->repeat_last_n?sp->count-sp->repeat_last_n:0;
        for(int i=s;i<sp->count;i++){int t=sp->tokens[i];
            if(t>=0&&t<n){if(p[t]<0)p[t]*=sp->repeat_penalty;else p[t]/=sp->repeat_penalty;}}}
    if(sp->temp>0)for(int i=0;i<n;i++)p[i]/=sp->temp;
    float mx=p[0];for(int i=1;i<n;i++)if(p[i]>mx)mx=p[i];
    float s=0;for(int i=0;i<n;i++){p[i]=expf(p[i]-mx);s+=p[i];}
    if(s>0)for(int i=0;i<n;i++)p[i]/=s;
    int do_k=sp->top_k>0&&sp->top_k<n;
    int do_p=sp->top_p<1.0f;
    if(do_k||do_p){
        SIDProbPair*a=(SIDProbPair*)malloc((size_t)n*sizeof(SIDProbPair));
        for(int i=0;i<n;i++){a[i].p=p[i];a[i].idx=i;}
        qsort(a,(size_t)n,sizeof(SIDProbPair),prob_cmp_desc);
        if(do_k){float kt=a[sp->top_k-1].p;for(int i=0;i<n;i++)if(p[i]<kt)p[i]=0;
            s=0;for(int i=0;i<n;i++)s+=p[i];if(s>0)for(int i=0;i<n;i++)p[i]/=s;}
        if(do_p){float c=0;for(int i=0;i<n;i++){if(c>=sp->top_p){for(int j=i;j<n;j++)p[a[j].idx]=0;break;}c+=a[i].p;}
            s=0;for(int i=0;i<n;i++)s+=p[i];if(s>0)for(int i=0;i<n;i++)p[i]/=s;}
        free(a);
    }
    float r=(float)rand()/(float)RAND_MAX,c=0;int tok=0;
    for(int i=0;i<n;i++){c+=p[i];if(r<c){tok=i;break;}}free(p);
    if(sp->count<MAX_TOKENS_CACHE)sp->tokens[sp->count++]=tok;
    return tok;
}

static char*chat_format_qwen(const char*roles[],const char*content[],int cc,size_t*out_len){
    size_t total=0;for(int i=0;i<cc;i++)total+=64+strlen(content[i]);
    char*fmt=(char*)calloc(total+128,1);size_t pos=0;
    for(int i=0;i<cc;i++)pos+=sprintf(fmt+pos,"<|im_start|>%s\n%s<|im_end|>\n",roles[i],content[i]);
    pos+=sprintf(fmt+pos,"<|im_start|>assistant\n");
    *out_len=pos;return fmt;
}

static uint64_t xor_hash(const uint8_t *d, size_t n) {
    uint64_t h = 0x811c9dc5c3a14b2dULL;
    for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 0x100000001b3ULL; }
    return h;
}

/* ── Memory-scan helpers (from test_swap_all.c) ── */

static int safe_ptr(const void *p) {
    if (!p || (uintptr_t)p < 0x10000) return 0;
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)
        && mbi.State == MEM_COMMIT
        && (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
}

static int is_valid_tensor_ptr(const void *cand) {
    if (!cand || (uintptr_t)cand < 0x10000) return 0;
    if (!safe_ptr((const char*)cand + 256 + 63)) return 0;
    const char *name = (const char*)cand + 256;
    if (name[0] < 32 || name[0] > 126) return 0;
    int nlen = (int)strnlen(name, 64);
    if (nlen < 3 || nlen >= 64) return 0;
    for (int i = 0; i < nlen; i++) {
        char c = name[i];
        if (c == '\\' || c == '/' || c == '=' || c == ':' || c == ';') return 0;
    }
    if (!((name[0] >= 'a' && name[0] <= 'z') ||
          (name[0] >= 'A' && name[0] <= 'Z') ||
          (name[0] >= '0' && name[0] <= '9') ||
          name[0] == '_' || name[0] == '.')) return 0;
    int64_t ne0; memcpy(&ne0, (const char*)cand + 16, sizeof(ne0));
    if (ne0 <= 0 || ne0 > 10000000) return 0;
    int type; memcpy(&type, cand, sizeof(type));
    if (type < 0 || type > 43) return 0;
    void *data; memcpy(&data, (const char*)cand + 248, sizeof(data));
    if (!data || (uintptr_t)data < 0x10000) return 0;
    return 1;
}

static const char* tensor_name(const void *tensor) { return (const char*)tensor + 256; }
static void* tensor_data(const void *tensor) { void *d; memcpy(&d, (const char*)tensor + 248, sizeof(d)); return d; }
static void tensor_set_data(void *tensor, void *new_data) { memcpy((char*)tensor + 248, &new_data, sizeof(void*)); }

static int name_wanted(const char *name, const GGUFTensorIndex *gidx) {
    for (uint64_t i = 0; i < gidx->n_tensors; i++)
        if (strcmp(name, gidx->names[i]) == 0) return 1;
    return 0;
}

static int scan_region(const uint8_t *start, const uint8_t *end,
    const GGUFTensorIndex *gidx, void **ptrs, int max, int *count)
{
    for (const uint8_t *p = start; p + 8 < end && *count < max; p += 8) {
        void *cand; memcpy(&cand, p, sizeof(cand));
        if (!cand || (uintptr_t)cand < 0x10000) continue;
        if (!is_valid_tensor_ptr(cand)) continue;
        if (!name_wanted(tensor_name(cand), gidx)) continue;
        int dup = 0;
        for (int i = 0; i < *count; i++)
            if (ptrs[i] == cand) { dup = 1; break; }
        if (dup) continue;
        ptrs[(*count)++] = cand;
    }
    return *count;
}

static void* find_layers_ptr(const void *model_alloc) {
    uint8_t *end = (uint8_t*)model_alloc + 65536;
    for (uint8_t *p = (uint8_t*)model_alloc; p + 8 < end; p += 8) {
        void *cand; memcpy(&cand, p, sizeof(cand));
        if (!cand || (uintptr_t)cand < 0x10000) continue;
        if (!safe_ptr(cand) || !safe_ptr((uint8_t*)cand + 4096 - 1)) continue;
        for (int off = 0; off < 4096; off += 8) {
            void *sub; memcpy(&sub, (uint8_t*)cand + off, sizeof(sub));
            if (!is_valid_tensor_ptr(sub)) continue;
            if (strncmp(tensor_name(sub), "blk.0.", 6) == 0) return cand;
        }
    }
    return NULL;
}

/* ── Found tensor tracking ── */

typedef struct {
    void   *ptr;
    void   *orig_data;
    char    name[64];
    size_t  nbytes;
    int64_t ne[4];
    uint32_t dtype;
} FoundTensor;

static FoundTensor found_tensors[MAX_TENSORS];
static int n_found = 0;

typedef struct {
    int      ft_idx;
    uint8_t *sid_data;
    size_t   sid_size;
    int      is_malloc;
} SIDSwapEntry;

static SIDSwapEntry sid_swaps[MAX_SID_SWAPS];
static int n_sid_swaps = 0;

/* delta array:  parallel to sid_swaps[], ใช้สำหรับ time travel journal
 * sid_timetravel_before_decode() อ่าน arrays นี้เพื่อ push delta entry
 * → runner ไม่ต้อง recompute pointer ทุกครั้ง */
static int    delta_ft_idx[MAX_SID_SWAPS];
static void  *delta_tensor_ptr[MAX_SID_SWAPS];
static void  *delta_orig_data[MAX_SID_SWAPS];
static void  *delta_sid_data[MAX_SID_SWAPS];
static size_t delta_size[MAX_SID_SWAPS];

static SidTimeTravel tt;

static void sid_swap_apply(void) {
    sid_timetravel_before_decode(&tt, n_sid_swaps,
        delta_ft_idx, delta_tensor_ptr, delta_orig_data, delta_sid_data, delta_size);
    for (int i = 0; i < n_sid_swaps; i++) {
        SIDSwapEntry *e = &sid_swaps[i];
        tensor_set_data(found_tensors[e->ft_idx].ptr, e->sid_data);
    }
}

static void sid_swap_restore(void) {
    for (int i = 0; i < n_sid_swaps; i++) {
        SIDSwapEntry *e = &sid_swaps[i];
        tensor_set_data(found_tensors[e->ft_idx].ptr, found_tensors[e->ft_idx].orig_data);
    }
    sid_timetravel_after_decode(&tt);
}

/* ── main ── */

int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"Usage: %s model.gguf [--chat] [--ngl N] [--sid-face N] [--sid-spoke N|all] [--sid-slot STR] [--sid-corrupt N] [--sid-checkpoint NAME] [--sid-rewind NAME] [--sid-ff NAME] [--sid-branch NAME] [options]\n",argv[0]);return 1;}
    const char*gguf_path=NULL,*opt_prompt=NULL;
    int opt_ngl=0,opt_max_new=256,opt_chat=0,opt_count_only=0,opt_bond=0,sid_face=0,sid_spoke=-1,sid_corrupt=0;
    const char*sid_slot="";
    Sampler sp={.temp=.7f,.top_p=.9f,.repeat_penalty=1.1f,.top_k=40,.repeat_last_n=64};
    sid_timetravel_init(&tt);
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--ngl")&&i+1<argc)opt_ngl=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--temp")&&i+1<argc)sp.temp=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--top-p")&&i+1<argc)sp.top_p=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--top-k")&&i+1<argc)sp.top_k=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--repeat-penalty")&&i+1<argc)sp.repeat_penalty=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--max-new")&&i+1<argc)opt_max_new=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--prompt")&&i+1<argc)opt_prompt=argv[++i];
        else if(!strcmp(argv[i],"--chat"))opt_chat=1;
        else if(!strcmp(argv[i],"--sid-face")&&i+1<argc)sid_face=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--sid-spoke")&&i+1<argc){const char*v=argv[++i];sid_spoke=!strcmp(v,"all")?-1:atoi(v);}
        else if(!strcmp(argv[i],"--sid-slot")&&i+1<argc)sid_slot=argv[++i];
        else if(!strcmp(argv[i],"--sid-corrupt")&&i+1<argc)sid_corrupt=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--sid-checkpoint")&&i+1<argc)sid_timetravel_set_checkpoint(&tt,argv[++i]);
        else if(!strcmp(argv[i],"--sid-rewind")&&i+1<argc)sid_timetravel_set_rewind(&tt,argv[++i]);
        else if(!strcmp(argv[i],"--sid-ff")&&i+1<argc)sid_timetravel_set_ffwd(&tt,argv[++i]);
        else if(!strcmp(argv[i],"--sid-branch")&&i+1<argc)sid_timetravel_set_branch(&tt,argv[++i]);
        else if(!strcmp(argv[i],"--bond"))opt_bond=1;
        else if(!strcmp(argv[i],"--count-only"))opt_count_only=1;
        else if(!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")){
            fprintf(stderr,"Usage: %s model.gguf [options]\n",argv[0]);
            fprintf(stderr,"  --chat                    Interactive chat mode\n");
            fprintf(stderr,"  --ngl N                   GPU layers (default: 0)\n");
            fprintf(stderr,"  --prompt TEXT             Prompt mode\n");
            fprintf(stderr,"  --temp N                  Temperature (default: 0.7)\n");
            fprintf(stderr,"  --top-p N                 Top-p sampling (default: 0.9)\n");
            fprintf(stderr,"  --top-k N                 Top-k sampling (default: 40)\n");
            fprintf(stderr,"  --max-new N               Max new tokens (default: 256)\n");
            fprintf(stderr,"  --sid-face N              SID face on/off (0=off, 1=on)\n");
            fprintf(stderr,"  --sid-spoke N|all         SID layer filter (all or 0..23)\n");
            fprintf(stderr,"  --sid-slot STR            SID tensor name substring\n");
            fprintf(stderr,"  --sid-corrupt N           Flip N bytes of each swapped SID tensor\n");
            fprintf(stderr,"  --sid-checkpoint NAME     Save time-travel checkpoint\n");
            fprintf(stderr,"  --sid-rewind NAME         Rewind to checkpoint\n");
            fprintf(stderr,"  --sid-ff NAME             Fast-forward from checkpoint\n");
            fprintf(stderr,"  --sid-branch NAME         Branch from checkpoint\n");
            fprintf(stderr,"  --bond                    Discover tensor topology bonds\n");
            fprintf(stderr,"  --count-only              Report tensor counts and exit\n");
            fprintf(stderr,"Chat commands:\n");
            fprintf(stderr,"  /checkpoint NAME          Save checkpoint\n");
            fprintf(stderr,"  /rewind NAME              Rewind to checkpoint\n");
            fprintf(stderr,"  /ff NAME                  Fast-forward from checkpoint\n");
            fprintf(stderr,"  /branch NAME              Branch from checkpoint\n");
            fprintf(stderr,"  /tt                       Show time travel state\n");
            fprintf(stderr,"  /clear                    Clear chat history\n");
            fprintf(stderr,"  /exit                     Exit\n");
            return 0;
        }
        else if(!gguf_path)gguf_path=argv[i];
    }
    if(!gguf_path){fprintf(stderr,"ERROR: missing model.gguf\n");return 1;}

    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");

    fprintf(stderr, "\n--- load_from_file ---\n");

    struct llama_model_params mp=llama_model_default_params();
    mp.n_gpu_layers=opt_ngl;
    PoglsTime t0,t1;clock_gettime(CLOCK_MONOTONIC,&t0);
    struct llama_model*model=llama_model_load_from_file(gguf_path,mp);
    clock_gettime(CLOCK_MONOTONIC,&t1);
    if(!model){fprintf(stderr,"ERROR: model load\n");llama_backend_free();return 1;}
    double lms=(t1.tv_sec-t0.tv_sec)*1000.0+(t1.tv_nsec-t0.tv_nsec)/1e6;
    fprintf(stderr,"[load] %.0f ms\n",lms);

    /* ── Open GGUF index ── */
    GGUFTensorIndex gidx;
    if (gguf_idx_open(gguf_path, &gidx) != 0) {
        fprintf(stderr, "ERROR: gguf_idx_open failed\n");
        llama_model_free(model); llama_backend_free(); return 1;
    }

    /* ── Scan model memory for tensor pointers ── */
    fprintf(stderr, "\n--- tensor scan ---\n");

    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(model, &mbi, sizeof(mbi));
    uint8_t *base = (uint8_t*)mbi.BaseAddress;
    uint8_t *base_end = base + mbi.RegionSize;
    if (base_end - base > 65536) base_end = base + 65536;

    void *ptrs[MAX_TENSORS] = {0};
    int n = 0;
    scan_region(base, base_end, &gidx, ptrs, MAX_TENSORS, &n);
    fprintf(stderr, "[scan] Tier 1 (model struct): %d tensors (base=%p end=%p)\n", n, (void*)base, (void*)base_end);

    void *layers = find_layers_ptr((const uint8_t*)model);
    if (layers) {
        MEMORY_BASIC_INFORMATION lmbi;
        if (VirtualQuery(layers, &lmbi, sizeof(lmbi)) && lmbi.State == MEM_COMMIT) {
            uint8_t *ls = (uint8_t*)lmbi.BaseAddress;
            uint8_t *le = ls + lmbi.RegionSize;
            if (le > ls + SCAN_LIMIT) le = ls + SCAN_LIMIT;
            scan_region(ls, le, &gidx, ptrs, MAX_TENSORS, &n);
            fprintf(stderr, "[scan] Tier 2 (layers heap): %d tensors\n", n);
        }
    } else {
        fprintf(stderr, "[scan] No layers ptr found (Tier 2 skipped)\n");
    }

    /* populate found_tensors from ptrs */
    for (int i = 0; i < n; i++) {
        found_tensors[i].ptr = ptrs[i];
        found_tensors[i].orig_data = tensor_data(ptrs[i]);
        int64_t ne[4]; memcpy(ne, (const char*)ptrs[i] + 16, sizeof(ne));
        size_t nb[4]; memcpy(nb, (const char*)ptrs[i] + 48, sizeof(nb));
        size_t nb_total = (size_t)ne[0] * nb[0];
        for (int j = 1; j < 4; j++) { size_t ni = (size_t)ne[j] * nb[j]; if (ni > nb_total) nb_total = ni; }
        found_tensors[i].nbytes = nb_total;
        memcpy(found_tensors[i].ne, ne, sizeof(ne));
        int t; memcpy(&t, ptrs[i], sizeof(t));
        found_tensors[i].dtype = (uint32_t)t;
        size_t nl = strnlen(tensor_name(ptrs[i]), 63);
        memcpy(found_tensors[i].name, tensor_name(ptrs[i]), nl);
        found_tensors[i].name[nl] = 0;
    }
    n_found = n;
    fprintf(stderr, "[scan] Total found: %d / %llu GGUF tensors\n", n_found,
        (unsigned long long)gidx.n_tensors);

    /* ── if --count-only, report and exit ── */
    if (opt_count_only) {
        fprintf(stderr, "\n[tensor] gguf_tensors=%llu found_via_scan=%d\n",
            (unsigned long long)gidx.n_tensors, n_found);
        gguf_idx_close(&gidx);
        llama_model_free(model); llama_backend_free(); return 0;
    }

    /* ── Bond discovery ── */
    if (opt_bond) {
        fprintf(stderr, "\n--- bond discovery ---\n");
        BondTensorInfo binfo[MAX_TENSORS];
        BondCtx bctx = { .tensors = binfo, .n_tensors = n_found };
        for (int i = 0; i < n_found; i++) {
            binfo[i].ptr = found_tensors[i].ptr;
            binfo[i].orig_data = found_tensors[i].orig_data;
            binfo[i].nbytes = found_tensors[i].nbytes;
            memcpy(binfo[i].name, found_tensors[i].name, NAME_MAX);
            memcpy(binfo[i].ne, found_tensors[i].ne, sizeof(binfo[i].ne));
            binfo[i].dtype = found_tensors[i].dtype;
        }
        bond_discover_all(&bctx);
        bond_print_summary(&bctx);
        for (int i = 0; i < bctx.graph.n_bonds && i < 40; i++) {
            Bond *b = &bctx.graph.bonds[i];
            fprintf(stderr, "  %s\n", b->label);
        }
        if (bctx.graph.n_bonds > 40)
            fprintf(stderr, "  ... and %d more\n", bctx.graph.n_bonds - 40);
    }

    /* ── SID cache init (quant-agnostic: cache ALL non-norm tensors) ── */
    fprintf(stderr, "\n--- SID cache init ---\n");

    uint64_t total_bytes = 0, max_sz = 0;
    int n_weight_tensors = 0;
    for (uint64_t i = 0; i < gidx.n_tensors; i++) {
        if (sid_loader_is_norm(gidx.names[i])) continue;
        n_weight_tensors++;
        total_bytes += gidx.sizes[i];
        if (gidx.sizes[i] > max_sz) max_sz = gidx.sizes[i];
    }
    fprintf(stderr, "[sid] weight tensors: %d, total data: %llu bytes, max tensor: %llu\n",
        n_weight_tensors, (unsigned long long)total_bytes, (unsigned long long)max_sz);

    SIDCache sid_cache;
    sid_cache_init(&sid_cache, total_bytes + (1u << 20));

    SIDLoaderCtx slc;
    if (sid_loader_open(&slc, gguf_path, &sid_cache) != 0) {
        fprintf(stderr, "ERROR: sid_loader_open failed\n");
        gguf_idx_close(&gidx);
        llama_model_free(model); llama_backend_free(); return 1;
    }

    uint8_t *read_buf = (uint8_t*)malloc((size_t)max_sz);
    uint8_t *verify_buf = (uint8_t*)malloc((size_t)max_sz);
    if (!read_buf || !verify_buf) {
        fprintf(stderr, "ERROR: OOM\n");
        free(read_buf); free(verify_buf); sid_loader_close(&slc);
        gguf_idx_close(&gidx);
        llama_model_free(model); llama_backend_free(); return 1;
    }

    int cached_ok = 0;
    for (uint64_t i = 0; i < gidx.n_tensors; i++) {
        if (sid_loader_is_norm(gidx.names[i])) continue;
        const char *name = gidx.names[i];
        size_t sz = (size_t)gidx.sizes[i];
        FILE *f = fopen(gguf_path, "rb");
        if (!f) { fprintf(stderr, "  fopen fail\n"); continue; }
        uint64_t abs_off = gguf_idx_tensor_abs_offset(&gidx, i);
        fseek(f, (long)abs_off, SEEK_SET);
        size_t r = fread(verify_buf, 1, sz, f);
        fclose(f);
        if (r != sz) { fprintf(stderr, "  fread fail at idx %llu name=%s sz=%llu abs_off=%llu r=%llu\n", (unsigned long long)i, name, (unsigned long long)sz, (unsigned long long)abs_off, (unsigned long long)r); continue; }
        uint64_t mmap_hash = xor_hash(verify_buf, sz);
        uint8_t *cached_data = NULL; size_t cached_sz = 0;
        if (sid_loader_load(&slc, name, read_buf, &cached_data, &cached_sz) == 0) {
            uint64_t cache_hash = xor_hash(cached_data, cached_sz);
            if (cache_hash == mmap_hash && cached_sz == sz) {
                cached_ok++;
            } else {
                fprintf(stderr, "  HASH MISMATCH %s: mmap=%016llx cache=%016llx sz=%llu/%llu\n",
                    name, (unsigned long long)mmap_hash, (unsigned long long)cache_hash,
                    (unsigned long long)sz, (unsigned long long)cached_sz);
            }
        }
    }

    fprintf(stderr, "[sid] cached+verified: %d/%d\n", cached_ok, n_weight_tensors);
    fprintf(stderr, "[sid] file_hits=%llu cache_hits=%llu bytes_read=%llu\n",
        (unsigned long long)slc.file_hits, (unsigned long long)slc.cache_hits,
        (unsigned long long)slc.bytes_read);

    /* second pass: cache-only verification */
    fprintf(stderr, "[sid] cache-only verification...\n");
    int cache_only_ok = 0;
    for (uint64_t i = 0; i < gidx.n_tensors; i++) {
        if (sid_loader_is_norm(gidx.names[i])) continue;
        const char *name = gidx.names[i];
        size_t sz = (size_t)gidx.sizes[i];
        uint8_t *cached_data = NULL; size_t cached_sz = 0;
        if (sid_loader_load(&slc, name, read_buf, &cached_data, &cached_sz) == 0 && cached_sz == sz) {
            cache_only_ok++;
        }
    }
    fprintf(stderr, "[sid] cache-only hits: %d/%d\n", cache_only_ok, n_weight_tensors);
    fprintf(stderr, "[sid] file_hits=%llu cache_hits=%llu (second pass)\n",
        (unsigned long long)slc.file_hits, (unsigned long long)slc.cache_hits);

    free(verify_buf);
    free(read_buf);

    /* ── Time travel init ── */
    if (sid_face > 0) {
        fprintf(stderr, "[timetravel] initialized: delta ring=%d entries, max checkpoints=%d\n",
            SID_DELTA_MAX_ENTRIES, SID_CHECKPOINT_MAX);
    }

    /* ── SID coordinate: (face, spoke, slot) → tensor filter ── */
    /*    face  = chooses SID face data (currently just on/off)        */
    /*    spoke = layer index (0..23) or -1=all, matches blk.<spoke>.  */
    /*    slot  = tensor name substring (e.g. "attn_q", "ffn_gate")    */
    if (sid_face > 0) {
        fprintf(stderr, "\n--- SID swap setup (face=%d, spoke=%d, slot=\"%s\") ---\n",
            sid_face, sid_spoke, sid_slot);
        for (int i = 0; i < n_found && n_sid_swaps < MAX_SID_SWAPS; i++) {
            const char *name = found_tensors[i].name;

            if (sid_spoke >= 0) {
                char expected[32];
                snprintf(expected, sizeof(expected), "blk.%d.", sid_spoke);
                if (strncmp(name, expected, strlen(expected)) != 0) continue;
            }
            if (sid_slot[0] != '\0' && strstr(name, sid_slot) == NULL) continue;

            uint8_t *cached = NULL; size_t cached_sz = 0;
            if (sid_cache_get(&sid_cache, name, &cached, &cached_sz) != 0) continue;

            sid_swaps[n_sid_swaps].ft_idx = i;
            sid_swaps[n_sid_swaps].sid_data = cached;
            sid_swaps[n_sid_swaps].sid_size = cached_sz;
            sid_swaps[n_sid_swaps].is_malloc = 0;

            delta_ft_idx[n_sid_swaps] = i;
            delta_tensor_ptr[n_sid_swaps] = found_tensors[i].ptr;
            delta_orig_data[n_sid_swaps] = found_tensors[i].orig_data;
            delta_sid_data[n_sid_swaps] = cached;
            delta_size[n_sid_swaps] = cached_sz;
            n_sid_swaps++;
        }
        if (n_sid_swaps == 0) {
            fprintf(stderr, "[sid] WARNING: no tensors matched coordinates. SID disabled.\n");
        } else {
            if (sid_corrupt) {
                for (int i = 0; i < n_sid_swaps; i++) {
                    SIDSwapEntry *e = &sid_swaps[i];
                    uint8_t *d = e->sid_data;
                    size_t sz = e->sid_size;
                    for (size_t j = 0; j < sz && j < (size_t)sid_corrupt; j++)
                        d[j] ^= 0x01;
                }
                fprintf(stderr, "[sid] CORRUPTED first %d bytes of %d swapped tensors\n", sid_corrupt, n_sid_swaps);
            }
            fprintf(stderr, "[sid] %d / %d tensors will be swapped per decode\n", n_sid_swaps, n_found);
        }
    }

    /* ── Create context (once, for the whole session) ── */
    struct llama_context_params cp=llama_context_default_params();
    cp.n_ctx=256;cp.n_threads=4;cp.n_threads_batch=4;cp.n_batch=128;cp.n_ubatch=64;
    struct llama_context *lctx = llama_init_from_model(model, cp);
    if(!lctx){fprintf(stderr,"ERROR: context\n"); sid_loader_close(&slc); gguf_idx_close(&gidx); llama_model_free(model); llama_backend_free(); return 1;}
    const struct llama_vocab *v = llama_model_get_vocab(model);
    int nv = llama_vocab_n_tokens(v);
    int eos = llama_vocab_eos(v); if(eos==-1) eos=151645;

    if(opt_chat){
        char*roles[MAX_CHAT_HISTORY],*content[MAX_CHAT_HISTORY];int cc=0;
        roles[0]="system";content[0]="You are a helpful assistant.";cc=1;
        printf("\n=== SID Chat (Qwen format) ===\n/exit  /clear\n\n");
        char line[MAX_LINE];
        while(1){
            printf(">>> ");fflush(stdout);
            if(!fgets(line,sizeof(line),stdin))break;
            size_t ll=strlen(line);while(ll>0&&(line[ll-1]=='\n'||line[ll-1]=='\r'))line[--ll]=0;
            if(ll==0)continue;
            if(!strcmp(line,"/exit"))break;
            if(!strcmp(line,"/clear")){cc=0;roles[0]="system";content[0]="You are a helpful assistant.";cc=1;printf("Cleared.\n");continue;}
            if(!strcmp(line,"/tt")){sid_timetravel_print_state(&tt);continue;}
            if(strncmp(line,"/checkpoint ",12)==0){sid_timetravel_set_checkpoint(&tt,line+12);fprintf(stderr,"[chat] /checkpoint %s\n",line+12);continue;}
            if(strncmp(line,"/rewind ",8)==0){sid_timetravel_set_rewind(&tt,line+8);fprintf(stderr,"[chat] /rewind %s\n",line+8);continue;}
            if(strncmp(line,"/ff ",4)==0){sid_timetravel_set_ffwd(&tt,line+4);fprintf(stderr,"[chat] /ff %s\n",line+4);continue;}
            if(strncmp(line,"/branch ",8)==0){sid_timetravel_set_branch(&tt,line+8);fprintf(stderr,"[chat] /branch %s\n",line+8);continue;}
            roles[cc]="user";content[cc]=strdup(line);cc++;
            size_t flen=0;char*fmt=chat_format_qwen((const char**)roles,(const char**)content,cc,&flen);
            int na=llama_tokenize(v,fmt,flen,NULL,0,false,false);
            int nt=na<0?-na:na;
            if(nt<=0||nt>2048-64){free(fmt);continue;}
            int*ta=(int*)malloc((size_t)nt*4);
            llama_tokenize(v,fmt,flen,ta,nt,false,false);
            free(fmt);
            struct llama_batch pb=llama_batch_init(nt,0,1);pb.n_tokens=nt;
            for(int j=0;j<nt;j++){pb.token[j]=ta[j];pb.pos[j]=j;pb.n_seq_id[j]=1;pb.seq_id[j][0]=0;pb.logits[j]=j==nt-1?1:0;}
            if (n_sid_swaps > 0) sid_swap_apply();
            if(llama_decode(lctx,pb)!=0){llama_batch_free(pb);free(ta);break;}
            if (n_sid_swaps > 0) sid_swap_restore();
            llama_batch_free(pb);
            Sampler gs=sp;gs.count=0;int32_t pos=nt;
            struct llama_batch gb=llama_batch_init(1,0,1);
            gb.n_tokens=1;gb.n_seq_id[0]=1;gb.seq_id[0][0]=0;gb.logits[0]=1;
            for(int i=0;i<opt_max_new;i++){
                if (n_sid_swaps > 0) sid_swap_apply();
                int tok=sample_token(llama_get_logits_ith(lctx,-1),nv,&gs);
                if (n_sid_swaps > 0) sid_swap_restore();
                if(tok==eos||tok==0)break;
                char b[16];int l=llama_token_to_piece(v,tok,b,16,0,false);
                if(l>0){b[l>15?15:l]=0;printf("%s",b);fflush(stdout);}
                gb.token[0]=tok;gb.pos[0]=pos++;
                if (n_sid_swaps > 0) sid_swap_apply();
                if(llama_decode(lctx,gb)!=0){if (n_sid_swaps > 0) sid_swap_restore();break;}
                if (n_sid_swaps > 0) sid_swap_restore();
            }
            llama_batch_free(gb);printf("\n");free(ta);
        }
        for(int i=0;i<cc;i++)free(content[i]);
    }

    if(opt_prompt){
        int ntokens=llama_tokenize(v,opt_prompt,strlen(opt_prompt),NULL,0,true,false);
        int nt=ntokens<0?-ntokens:ntokens;
        if(nt<=0){fprintf(stderr,"tokenize fail\n"); return 1;}
        int*toks=(int*)malloc((size_t)nt*4);
        llama_tokenize(v,opt_prompt,strlen(opt_prompt),toks,nt,true,false);
        struct llama_batch pb=llama_batch_init(nt,0,1);pb.n_tokens=nt;
        for(int j=0;j<nt;j++){pb.token[j]=toks[j];pb.pos[j]=j;pb.n_seq_id[j]=1;pb.seq_id[j][0]=0;pb.logits[j]=j==nt-1?1:0;}
        if (n_sid_swaps > 0) sid_swap_apply();
        if(llama_decode(lctx,pb)!=0){fprintf(stderr,"[dbg] decode fail\n");llama_batch_free(pb);free(toks);return 1;}
        if (n_sid_swaps > 0) sid_swap_restore();
        llama_batch_free(pb);

        Sampler gs=sp;gs.count=0;int32_t pos=nt;
        struct llama_batch gb=llama_batch_init(1,0,1);
        gb.n_tokens=1;gb.n_seq_id[0]=1;gb.seq_id[0][0]=0;gb.logits[0]=1;
        for(int i=0;i<opt_max_new;i++){
            if (n_sid_swaps > 0) sid_swap_apply();
            int tok=sample_token(llama_get_logits_ith(lctx,-1),nv,&gs);
            if (n_sid_swaps > 0) sid_swap_restore();
            if(tok==eos||tok==0)break;
            char b[16];int l=llama_token_to_piece(v,tok,b,16,0,false);
            if(l>0){b[l>15?15:l]=0;printf("%s",b);fflush(stdout);}
            gb.token[0]=tok;gb.pos[0]=pos++;
            if (n_sid_swaps > 0) sid_swap_apply();
            if(llama_decode(lctx,gb)!=0){if (n_sid_swaps > 0) sid_swap_restore();break;}
            if (n_sid_swaps > 0) sid_swap_restore();
        }
        llama_batch_free(gb);printf("\n");free(toks);
    }

    /* ── Cleanup ── */
    for (int i = 0; i < n_sid_swaps; i++) {
        if (sid_swaps[i].is_malloc) free(sid_swaps[i].sid_data);
    }
    sid_loader_close(&slc);
    gguf_idx_close(&gidx);
    llama_free(lctx);llama_model_free(model);llama_backend_free();
    return 0;
}
