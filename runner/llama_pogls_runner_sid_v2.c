/*
 * llama_pogls_runner_sid_v2.c — SID-aware inference runner with inline tensor scan
 * Build (from repo root): gcc -O2 -std=c11 -I. -Icollection -Icollection/src -Icollection/core -Icollection/core/core -Icollection/core/pogls_engine/core -Icollection/geopixel -Icollection/geopixel/Metatron/core -Icollection/pogls_engine -Icollection/geo_jump_module/include -II:/llama.cpp/include -II:/llama.cpp/ggml/include -o llama_pogls_runner_sid_v2.exe runner/llama_pogls_runner_sid_v2.c collection/geo_jump_module/src/geo_jump.c runner/llama.dll runner/ggml.dll runner/ggml-base.dll runner/ggml-cpu-x64.dll -lm
 * Usage: llama_pogls_runner_sid_v2.exe I:/model/model.gguf [options]
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
  #include <direct.h>
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

/* Cross-platform abstraction (VirtualQuery, GetModuleFileName, etc.) */
#include "pogls_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include "llama.h"
#include "gguf_index.h"
#include "sid_cache.h"
#include "sid_loader.h"
#include "bond_discovery.h"
#include "hex_grid.h"
#include "th_grid.h"
#include "goldberg_sid.h"
#include "sid_delta_ring.h"
#include "sid_timetravel.h"
#include "cosplay.h"
#include "session_profile.h"
#include "kv_sid_evict.h"
#include "kv_page_store.h"
#include "kv_tensor_access.h"
#define KV_REMAP_USE_SHADOW 1
#include "kv_remap.h"
#define POGLS_RAIL_USE_POGTIME
#include "kv_remap_rail.h"

/* ggml_tensor->data is at offset 248 in the struct (same as SID swap) */
#define GGML_TENSOR_DATA_OFFSET 248

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

static size_t tok_prefix_len(const int *a, size_t na, const int *b, size_t nb) {
    size_t n = na < nb ? na : nb;
    size_t i = 0;
    for (; i < n; i++) {
        if (a[i] != b[i]) break;
    }
    return i;
}

static int tokbuf_reserve(int **buf, size_t *cap, size_t need) {
    if (need <= *cap) return 0;
    size_t ncap = (*cap == 0) ? 256 : *cap;
    while (ncap < need) {
        if (ncap > (SIZE_MAX / 2)) {
            ncap = need;
            break;
        }
        ncap *= 2;
    }
    int *nbuf = (int *)realloc(*buf, ncap * sizeof(int));
    if (!nbuf) return -1;
    *buf = nbuf;
    *cap = ncap;
    return 0;
}

static int tokbuf_copy(int **buf, size_t *n, size_t *cap, const int *src, size_t src_n) {
    if (tokbuf_reserve(buf, cap, src_n) != 0) return -1;
    if (src_n > 0) memcpy(*buf, src, src_n * sizeof(int));
    *n = src_n;
    return 0;
}

static int tokbuf_append(int **buf, size_t *n, size_t *cap, const int *src, size_t add_n) {
    if (add_n == 0) return 0;
    if (tokbuf_reserve(buf, cap, *n + add_n) != 0) return -1;
    memcpy(*buf + *n, src, add_n * sizeof(int));
    *n += add_n;
    return 0;
}

static uint64_t xor_hash(const uint8_t *d, size_t n) {
    uint64_t h = 0x811c9dc5c3a14b2dULL;
    for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 0x100000001b3ULL; }
    return h;
}

/* ── Memory-scan helpers (from test_swap_all.c) ── */

static int is_valid_tensor_ptr(const void *cand) {
    if (!cand || (uintptr_t)cand < 0x10000) return 0;
    if (!pogls_is_valid_ptr((const char*)cand + 256 + 63)) return 0;
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
        if (!pogls_is_valid_ptr(cand) || !pogls_is_valid_ptr((uint8_t*)cand + 4096 - 1)) continue;
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

#include "capture_pipeline.h"
#include "tensor_memory.h"
#include "geo_addr.h"
#include "dramtile_store.h"
#include "icosa_bridge_loader.h"
#include "gear_lock.h"
#include "kv_swap.h"

static IcosaBridge g_ibridge;
static void *g_ibridge_ctx = NULL;
static int g_opt_twin_gpu = 0;
static int g_opt_gear_lock = 0;
static float g_opt_gear_threshold = 0.30f;
static int g_opt_gear_log = 16;

/* Icosa event flags (local def — icosa_twin_bridge.h not included in runner) */
#define ICOSA_EV_NONE      0x00u
#define ICOSA_EV_FLUSH     0x01u
#define ICOSA_EV_BOUNDARY  0x02u

/* Gear lock state: per-tensor icosa route history + priority scores */
static GearLockState g_gear;

typedef struct {
    int      ft_idx;
    uint32_t geo_addr;
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

/* Per-cycle gear lock active mask: 1 = apply this swap, 0 = skip */
static uint8_t gear_active_mask[MAX_SID_SWAPS];
static int n_gear_active = 0;

static SidTimeTravel tt;

/* cosplay */
static CosplayProfile g_cp;
static int g_opt_cosplay = 0;
static int g_opt_cosplay_compare = 0;
static const char *g_opt_experiment = NULL;
static uint8_t **g_experiment_clean_ptrs = NULL;
static int g_n_experiment_clean = 0;

/* session profile */
static SessionProfile g_ses;
static int g_opt_sid_disable = 0;
static int g_opt_simulate = 0;
static const char *g_opt_profile_batch = NULL;
static int g_turn_count = 0;
static double g_opt_sem_weight = 0.8, g_opt_hist_weight = 0.2, g_opt_rnd_weight = 0.0, g_opt_decay = 1.0;

/* DRamTile */
static DRamTileStore g_dramtile;
static int g_opt_dramtile = 0;

/* KV swap */
static int g_opt_kv_swap = 0;       /* bytes to perturb (0=disabled) */
static int g_opt_kv_layer = -1;     /* layer filter */
static KVSwapCtx g_kv_swap;

/* KV remap (adaptive skeleton+delta) */
static KVRemapCtx g_remap_ctx;
static KVRemapRail g_remap_rail;
static int g_opt_remap = 0;
static int g_opt_shadow = 0;

/* tensor memory store (--mem-store) */
static const char *g_opt_mem_store = NULL;
static uint8_t *g_tmem_buf = NULL;
static size_t g_tmem_buf_size = 0;
static TensorMemStore g_tmem_store;

/* Apply SID swaps with gear lock filtering.
 * If mask is non-NULL, only swaps with mask[i] != 0 are applied.
 * Time travel journals only actually-applied swaps. */
static void sid_swap_apply_ex(const uint8_t *mask) {
    int n_apply = 0;
    int apply_ft_idx[MAX_SID_SWAPS];
    void *apply_tptr[MAX_SID_SWAPS];
    void *apply_orig[MAX_SID_SWAPS];
    void *apply_sid[MAX_SID_SWAPS];
    size_t apply_sz[MAX_SID_SWAPS];

    for (int i = 0; i < n_sid_swaps; i++) {
        if (mask && !mask[i]) continue;
        apply_ft_idx[n_apply] = delta_ft_idx[i];
        apply_tptr[n_apply] = delta_tensor_ptr[i];
        apply_orig[n_apply] = delta_orig_data[i];
        apply_sid[n_apply] = delta_sid_data[i];
        apply_sz[n_apply] = delta_size[i];
        n_apply++;
    }

    if (n_apply == 0) return;

    sid_timetravel_before_decode(&tt, n_apply,
        apply_ft_idx, apply_tptr, apply_orig, apply_sid, apply_sz);

    for (int i = 0; i < n_apply; i++) {
        tensor_set_data(found_tensors[apply_ft_idx[i]].ptr, apply_sid[i]);
    }
}

/* Rebuild gear active mask from current gear lock state.
 * On first 3 cycles (no history yet), keeps all swaps active. */
static inline void gear_rebuild_mask(void) {
    if (!g_opt_gear_lock) {
        n_gear_active = n_sid_swaps;
        memset(gear_active_mask, 1, n_sid_swaps);
        return;
    }
    /* bootstrap: first few cycles have no route history — keep everything */
    if (g_gear.cycles < 3) {
        n_gear_active = n_sid_swaps;
        memset(gear_active_mask, 1, n_sid_swaps);
        return;
    }
    n_gear_active = gear_lock_build_mask(&g_gear, n_sid_swaps, delta_ft_idx,
                                          gear_active_mask, g_opt_gear_threshold);
}

static inline void sid_swap_apply(void) {
    gear_rebuild_mask();
    sid_swap_apply_ex(g_opt_gear_lock ? gear_active_mask : NULL);
}

/* Restore SID swaps with gear lock filtering.
 * Only restores swaps that were actually applied (i.e., pass mask). */
static void sid_swap_restore_ex(const uint8_t *mask) {
    int n_restore = 0;
    int restore_ft_idx[MAX_SID_SWAPS];

    if (mask) {
        for (int i = 0; i < n_sid_swaps; i++) {
            if (!mask[i]) continue;
            restore_ft_idx[n_restore++] = delta_ft_idx[i];
        }
    } else {
        for (int i = 0; i < n_sid_swaps; i++) {
            restore_ft_idx[n_restore++] = delta_ft_idx[i];
        }
    }

    for (int i = 0; i < n_restore; i++) {
        tensor_set_data(found_tensors[restore_ft_idx[i]].ptr,
                        found_tensors[restore_ft_idx[i]].orig_data);
    }

    sid_timetravel_after_decode(&tt);
}

static inline void sid_swap_restore(void) {
    sid_swap_restore_ex(g_opt_gear_lock ? gear_active_mask : NULL);
}

/* ── Icosa lane push: feed tensor data through GPU icosa lane ── */

/* Lightweight 64-bit hash of first 64 bytes of tensor data */
static inline uint64_t tensor_data_hash64(const void *data, size_t nbytes) {
    if (!data || nbytes == 0) return 0;
    size_t n = nbytes < 64 ? nbytes : 64;
    const uint64_t *p = (const uint64_t *)data;
    uint64_t h = 0;
    for (size_t i = 0; i < n / 8; i++) h ^= p[i];
    return h;
}

/* Push tensor data through icosa lane with geo_addr + data hash.
 * Feeds routes + events into GearLockState for dynamic SID swap scheduling. */
static void twin_gpu_gear_push(void) {
    if (!g_opt_twin_gpu || !g_ibridge_ctx) return;
    if (n_sid_swaps <= 0) return;

    uint64_t *addrs = (uint64_t*)malloc((size_t)n_sid_swaps * sizeof(uint64_t));
    uint64_t *vals  = (uint64_t*)malloc((size_t)n_sid_swaps * sizeof(uint64_t));
    uint64_t *routes = (uint64_t*)malloc((size_t)n_sid_swaps * sizeof(uint64_t));
    uint8_t  *events = (uint8_t*)malloc((size_t)n_sid_swaps);
    if (!addrs || !vals || !routes || !events) { free(addrs); free(vals); free(routes); free(events); return; }

    for (int s = 0; s < n_sid_swaps; s++) {
        int fi = sid_swaps[s].ft_idx;
        uint32_t geo = sid_swaps[s].geo_addr;
        addrs[s] = (uint64_t)geo;
        vals[s]  = tensor_data_hash64(found_tensors[fi].orig_data,
                                       found_tensors[fi].nbytes);
    }

    int ret = g_ibridge.dispatch(g_ibridge_ctx, addrs, vals, (uint32_t)n_sid_swaps,
                                   0xDEADBEEFCAFEBABEULL, 1, 0x5555555555555555ULL,
                                   routes, events);
    if (ret == 0) {
        /* Feed icosa lane output into gear lock state */
        gear_lock_update(&g_gear, routes, events, n_sid_swaps, delta_ft_idx);
        /* Periodic log every g_opt_gear_log cycles */
        if (g_opt_gear_lock && g_opt_gear_log > 0 && (g_gear.cycles % g_opt_gear_log) == 0) {
            gear_lock_print_summary(&g_gear,
                (const char *const *)found_tensors, n_found, 5);
        }
    } else {
        fprintf(stderr, "[twin-gpu] dispatch error %d\n", ret);
    }

    free(addrs); free(vals); free(routes); free(events);
}

/* ── Tensor memory store: log swapped tensors after decode ── */
static inline void sid_mem_store_log(struct llama_context *lctx, int nv, uint16_t tick, int sid_face_val) {
    if (!g_opt_mem_store || !g_tmem_buf || n_sid_swaps == 0) return;
    const float *logits = llama_get_logits_ith(lctx, -1);
    uint8_t entropy = 128;
    if (logits && nv > 0) {
        uint32_t h = 0;
        int lim = nv < 4096 ? nv : 4096;
        for (int i = 0; i < lim; i++) { uint32_t u; memcpy(&u, &logits[i], sizeof(u)); h ^= u; }
        entropy = (uint8_t)((h >> 16) ^ (h >> 8) ^ h);
    }
    for (int si = 0; si < n_sid_swaps; si++) {
        int fi = sid_swaps[si].ft_idx;
        int layer = bond_extract_layer(found_tensors[fi].name);
        ZoneCardSID z;
        memset(&z, 0, sizeof(z));
        z.node_id = (uint32_t)fi;
        z.capo_key = (uint16_t)sid_face_val;
        z.inf.logit_entropy = entropy;
        z.inf.tick = tick;
        z.inf.layer = (uint8_t)(layer < 0 ? 255 : layer);
        z.card.card_type = 1;
        tmem_append_raw(&g_tmem_store, &z, found_tensors[fi].name, NULL, 0);
    }
}

/* ── main ── */

int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"Usage: %s model.gguf [--chat] [--ngl N] [--sid-face N] [--sid-spoke N|all] [--sid-slot STR] [--sid-corrupt N] [--sid-checkpoint NAME] [--sid-rewind NAME] [--sid-ff NAME] [--sid-branch NAME] [options]\n",argv[0]);return 1;}
    const char*gguf_path=NULL,*opt_prompt=NULL,*opt_dump_logits=NULL,*opt_capture=NULL,*opt_script=NULL;
    int opt_ngl=0,opt_max_new=256,opt_chat=0,opt_count_only=0,opt_bond=0,opt_hex=0,opt_trihex=0,opt_goldberg=0,sid_face=0,sid_spoke=-1,sid_corrupt=0,opt_swap_cold=0,sid_corrupt_val=1,opt_hybrid=0,opt_sid_adaptive=0,opt_sid_multi=0,opt_kv_evict=0,opt_kv_page=0,opt_kv_page_evict=0,opt_ctx=2048;
    float opt_goldberg_threshold = 1.2f;
    int opt_sid_geodesic = 0; float opt_sid_geo_radius = 0.5f, opt_hybrid_radius = 0.01f;
    const char*sid_slot="",*sid_pattern=NULL;
    setbuf(stderr, NULL);
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
        else if(!strcmp(argv[i],"--script")&&i+1<argc)opt_script=argv[++i];
        else if(!strcmp(argv[i],"--chat"))opt_chat=1;
        else if(!strcmp(argv[i],"--sid-face")&&i+1<argc)sid_face=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--sid-spoke")&&i+1<argc){const char*v=argv[++i];sid_spoke=!strcmp(v,"all")?-1:atoi(v);}
        else if(!strcmp(argv[i],"--sid-slot")&&i+1<argc)sid_slot=argv[++i];
        else if(!strcmp(argv[i],"--sid-corrupt")&&i+1<argc)sid_corrupt=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--sid-pattern")&&i+1<argc)sid_pattern=argv[++i];
        else if(!strcmp(argv[i],"--sid-byte")&&i+1<argc)sid_corrupt_val=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--sid-checkpoint")&&i+1<argc)sid_timetravel_set_checkpoint(&tt,argv[++i]);
        else if(!strcmp(argv[i],"--sid-rewind")&&i+1<argc)sid_timetravel_set_rewind(&tt,argv[++i]);
        else if(!strcmp(argv[i],"--sid-ff")&&i+1<argc)sid_timetravel_set_ffwd(&tt,argv[++i]);
        else if(!strcmp(argv[i],"--sid-branch")&&i+1<argc)sid_timetravel_set_branch(&tt,argv[++i]);
        else if(!strcmp(argv[i],"--bond"))opt_bond=1;
        else if(!strcmp(argv[i],"--hex"))opt_hex=2;
        else if(!strcmp(argv[i],"--hex-level")&&i+1<argc)opt_hex=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--trihex"))opt_trihex=2;
        else if(!strcmp(argv[i],"--trihex-level")&&i+1<argc)opt_trihex=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--goldberg"))opt_goldberg=1;
        else if(!strcmp(argv[i],"--goldberg-threshold")&&i+1<argc)opt_goldberg_threshold=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--count-only"))opt_count_only=1;
        else if(!strcmp(argv[i],"--swap-cold"))opt_swap_cold=1;
        else if(!strcmp(argv[i],"--sid-geodesic"))opt_sid_geodesic=1;
        else if(!strcmp(argv[i],"--sid-geo-radius")&&i+1<argc)opt_sid_geo_radius=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--hybrid"))opt_hybrid=1;
        else if(!strcmp(argv[i],"--hybrid-radius")&&i+1<argc)opt_hybrid_radius=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--sid-adaptive"))opt_sid_adaptive=1;
        else if(!strcmp(argv[i],"--sid-multi")&&i+1<argc)opt_sid_multi=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--capture")&&i+1<argc)opt_capture=argv[++i];
        else if(!strcmp(argv[i],"--mem-store")&&i+1<argc)g_opt_mem_store=argv[++i];
        else if(!strcmp(argv[i],"--twin-gpu"))g_opt_twin_gpu=1;
        else if(!strcmp(argv[i],"--gear-lock")){g_opt_gear_lock=1;g_opt_twin_gpu=1;}
        else if(!strcmp(argv[i],"--gear-lock-threshold")&&i+1<argc){g_opt_gear_threshold=(float)atof(argv[++i]);g_opt_gear_lock=1;g_opt_twin_gpu=1;}
        else if(!strcmp(argv[i],"--gear-log")&&i+1<argc){g_opt_gear_log=atoi(argv[++i]);g_opt_gear_lock=1;g_opt_twin_gpu=1;}
        else if(!strcmp(argv[i],"--dump-logits")&&i+1<argc)opt_dump_logits=argv[++i];
        else if(!strcmp(argv[i],"--cosplay")&&i+1<argc){g_opt_cosplay=1;cosplay_load(argv[++i],&g_cp);fprintf(stderr,"[cosplay] loaded %s (%u entries)\n",argv[i],g_cp.n);}
        else if(!strcmp(argv[i],"--cosplay-compare"))g_opt_cosplay_compare=1;
        else if(!strcmp(argv[i],"--experiment")&&i+1<argc)g_opt_experiment=argv[++i];
        else if(!strcmp(argv[i],"--sid-disable"))g_opt_sid_disable=1;
        else if(!strcmp(argv[i],"--kv-swap")&&i+1<argc)g_opt_kv_swap=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--kv-layer")&&i+1<argc)g_opt_kv_layer=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--kv-evict")&&i+1<argc)opt_kv_evict=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--kv-page"))opt_kv_page=1;
        else if(!strcmp(argv[i],"--kv-page-evict")&&i+1<argc){opt_kv_page=1;opt_kv_page_evict=atoi(argv[++i]);}
        else if(!strcmp(argv[i],"--remap"))g_opt_remap=1;
        else if(!strcmp(argv[i],"--shadow"))g_opt_shadow=1;
        else if(!strcmp(argv[i],"--ctx")&&i+1<argc)opt_ctx=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--dramtile"))g_opt_dramtile=1;
        else if(!strcmp(argv[i],"--simulate"))g_opt_simulate=1;
        else if(!strcmp(argv[i],"--profile-batch")&&i+1<argc)g_opt_profile_batch=argv[++i];
        else if(!strcmp(argv[i],"--semantic-weight")&&i+1<argc)g_opt_sem_weight=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--history-weight")&&i+1<argc)g_opt_hist_weight=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--random-weight")&&i+1<argc)g_opt_rnd_weight=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--decay")&&i+1<argc)g_opt_decay=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")){
            fprintf(stderr,"Usage: %s model.gguf [options]\n",argv[0]);
            fprintf(stderr,"  --chat                    Interactive chat mode\n");
            fprintf(stderr,"  --ngl N                   GPU layers (default: 0)\n");
            fprintf(stderr,"  --prompt TEXT             Prompt mode\n");
            fprintf(stderr,"  --script FILE             Chat mode input file\n");
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
            fprintf(stderr,"  --hex                     Hex grid aperture-7 bond (replaces --bond)\n");
            fprintf(stderr,"  --hex-level N             Hex grid level: 1=coarse 2=mid 3=fine (default: 2)\n");
            fprintf(stderr,"  --trihex                  TriHex tessellation arena coordinates\n");
            fprintf(stderr,"  --trihex-level N           TriHex level: 1=sector 2=hex 3=trihex (default: 2)\n");
            fprintf(stderr,"  --goldberg                Goldberg spherical projection of trihex\n");
            fprintf(stderr,"  --goldberg-threshold N    Geodesic threshold in rad (default: 1.2)\n");
            fprintf(stderr,"  --sid-corrupt N           Corrupt first N bytes of swapped tensors (XOR with --sid-byte)\n");
            fprintf(stderr,"  --sid-byte N              Corruption XOR/value byte (default: 1)\n");
            fprintf(stderr,"  --sid-pattern TYPE:PARAM  Corrupt pattern: xor:N, set:N, zero, rot:K\n");
            fprintf(stderr,"  --sid-geodesic           Expand SID swap to goldberg geodesic neighbors\n");
            fprintf(stderr,"  --sid-geo-radius N       Geodesic radius in rad (default: 0.5)\n");
            fprintf(stderr,"  --hybrid                 Hex+goldberg hybrid: hot cluster + geodesic neighbors\n");
            fprintf(stderr,"  --hybrid-radius N        Geodesic radius for hybrid (default: 0.01)\n");
            fprintf(stderr,"  --sid-adaptive           Scale corruption bytes by tensor hotness\n");
            fprintf(stderr,"  --sid-multi N            Inject at N hottest layers simultaneously\n");
            fprintf(stderr,"  --capture DIR             Capture tensor geometry after decode, write to DIR\n");
            fprintf(stderr,"  --mem-store PATH          Log SID tensor memory timeline to file\n");
            fprintf(stderr,"  --twin-gpu                Enable GPU icosa lane (CUDA twin bridge)\n");
            fprintf(stderr,"  --gear-lock              Enable gear lock feedback (requires --twin-gpu)\n");
            fprintf(stderr,"  --gear-lock-threshold N  Gear lock priority threshold 0..1 (default: 0.30, lower=more swaps)\n");
            fprintf(stderr,"  --gear-log N             Gear lock log interval cycles (default: 16, 0=disable)\n");
            fprintf(stderr,"  --cosplay PATH            Load cosplay perturbation profile\n");
            fprintf(stderr,"  --cosplay-compare         Compare output with/without cosplay\n");
            fprintf(stderr,"  --experiment DIR          Run multi-condition experiment\n");
            fprintf(stderr,"  --sid-disable             Disable all SID swaps (plain inference)\n");
            fprintf(stderr,"  --kv-swap N              Perturb N bytes in KV state per decode (0=off)\n");
            fprintf(stderr,"  --kv-layer N             Target specific KV layer (-1=all, 0..N)\n");
            fprintf(stderr,"  --ctx N                  Context size (default: 2048)\n");
            fprintf(stderr,"  --kv-evict N             KV SID evict: swap oldest N layers to backup (zero-copy)\n");
            fprintf(stderr,"  --kv-page                KV Page Store: token-position-granular paging with DRamTile backing\n");
            fprintf(stderr,"  --kv-page-evict N        KV Page Store: evict oldest N pages after init\n");
            fprintf(stderr,"  --remap                  KV remap: adaptive skeleton+delta + rail verify\n");
            fprintf(stderr,"  --shadow                 KV remap shadow zone: delta metadata heartbeat\n");
            fprintf(stderr,"  --dramtile                Enable DRamTile zero-copy tensor store\n");
            fprintf(stderr,"  --simulate                Simulate session (no model)\n");
            fprintf(stderr,"  --profile-batch FILE      Batch generate session profiles\n");
            fprintf(stderr,"  --semantic-weight N       Semantic hash weight (default: 0.8)\n");
            fprintf(stderr,"  --history-weight N        History weight (default: 0.2)\n");
            fprintf(stderr,"  --random-weight N         Random weight (default: 0.0)\n");
            fprintf(stderr,"  --decay N                 Edge weight decay per turn (default: 1.0)\n");
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

    if (g_opt_sid_disable) {
        fprintf(stderr, "[sid] disabled by --sid-disable\n");
        sid_face = 0;
    }

    llama_backend_init();
    ggml_backend_load("ggml-cpu-x64.dll");
    if (opt_ngl > 0) {
        /* Try loading GPU backends. Vulkan DLL must be in the same dir
         * or on PATH. With --ngl=0 we skip GPU backends entirely to
         * avoid the WSL2/DrvFs + Vulkan tensor-load hang. */
        ggml_backend_load("ggml-vulkan.dll");
        ggml_backend_load("ggml-cuda.dll");
    }

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

    /* ── Scan model memory for tensor pointers (skipped — pogls_query_memory unstable) ── */
    fprintf(stderr, "\n--- tensor scan (skipped) ---\n");
    int n_found = 0;
    int n_layers = 0;
    { /* dummy block to match downstream code that references n_found, n_layers */

    } /* end skipped tensor scan block */

    /* ── Arena / bond / prediction ── */
    float *bond_hotness = NULL;
    if (opt_trihex) {
        /* trihex is pure arena — just print coords for debug */
        int tlevel = opt_trihex;
        fprintf(stderr, "\n--- trihex tessellation arena (level=%d) ---\n", tlevel);
        THGridState tgs;
        th_grid_init(&tgs, tlevel, n_layers);
        const char *hnames[MAX_TENSORS];
        for (int i = 0; i < n_found; i++)
            hnames[i] = found_tensors[i].name;
        th_print_coords(hnames, n_found, &tgs);
    }
    if (opt_goldberg) {
        fprintf(stderr, "\n--- goldberg spherical prediction ---\n");
        THGridState tgs;
        th_grid_init(&tgs, 2, n_layers);
        const char *hnames[MAX_TENSORS];
        for (int i = 0; i < n_found; i++)
            hnames[i] = found_tensors[i].name;

        /* map all tensors to THCoords */
        THCoord *gcoords = (THCoord*)calloc((size_t)n_found, sizeof(THCoord));
        for (int i = 0; i < n_found; i++)
            gcoords[i] = th_from_name(hnames[i], &tgs);

        goldberg_print_coords(hnames, n_found, gcoords);

        /* discover bonds via goldberg geodesic */
        GoldbergBondGraph gbg;
        goldberg_bond_graph_init(&gbg, n_found * 8);
        goldberg_discover_bonds(&gbg, gcoords, n_found, opt_goldberg_threshold);

        fprintf(stderr, "[goldberg] %d bonds (geo threshold <= %.2f rad)\n",
                gbg.n_bonds, opt_goldberg_threshold);

        /* compute initial hotness from cardioid express (match hex_grid) */
        float *goldberg_init_h = (float*)calloc((size_t)n_found, sizeof(float));
        for (int i = 0; i < n_found; i++) {
            int l = th_extract_layer(hnames[i]);
            goldberg_init_h[i] = l >= 0 ? goldberg_cardioid_hotness(l, n_layers) : 0.5f;
            if (goldberg_init_h[i] < 0.5f && strstr(hnames[i], "norm.weight"))
                goldberg_init_h[i] = 0.5f;
        }

        /* predict hotness using goldberg spherical propagation */
        float *goldberg_h = goldberg_predict_hotness(&gbg, goldberg_init_h, n_found);
        free(goldberg_init_h);
        goldberg_predict_print(hnames, n_found, goldberg_h);

        goldberg_bond_graph_free(&gbg);
        free(gcoords);
        free(goldberg_h);
    }
    if (opt_hex > 0) {
        int hlevel = opt_hex;  /* 1, 2, 3 */
        fprintf(stderr, "\n--- hex grid aperture-7 (level=%d) ---\n", hlevel);
        hex_grid_init(&_hex_grid, hlevel, (float)n_layers);

        const char *hnames[MAX_TENSORS];
        for (int i = 0; i < n_found; i++)
            hnames[i] = found_tensors[i].name;

        hex_print_coords(hnames, n_found, &_hex_grid);

        HexBondGraph hg;
        int max_bonds = n_found * 8;
        if (max_bonds > 32768) max_bonds = 32768;
        hex_bond_graph_init(&hg, max_bonds);
        hex_discover_bonds(&hg, hnames, n_found, &_hex_grid, 8);

        hex_bond_print_summary(&hg);
        for (int i = 0; i < hg.n_bonds && i < 40; i++) {
            HexBond *b = &hg.bonds[i];
            fprintf(stderr, "  %s\n", b->label);
        }
        if (hg.n_bonds > 40)
            fprintf(stderr, "  ... and %d more\n", hg.n_bonds - 40);

        fprintf(stderr, "\n--- hex bond prediction ---\n");
        bond_hotness = hex_predict_hotness(&hg, hnames, n_found, n_layers);
        hex_predict_print(hnames, n_found, bond_hotness);
        hex_bond_graph_free(&hg);
    }
    /* ── Legacy bond discovery (only if --hex not active) ── */
    else if (opt_bond) {
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
        int max_bonds = n_found * 8;
        if (max_bonds > 32768) max_bonds = 32768;
        bond_discover_all(&bctx, n_layers, max_bonds);
        bond_print_summary(&bctx);
        for (int i = 0; i < bctx.graph.n_bonds && i < 40; i++) {
            Bond *b = &bctx.graph.bonds[i];
            fprintf(stderr, "  %s\n", b->label);
        }
        if (bctx.graph.n_bonds > 40)
            fprintf(stderr, "  ... and %d more\n", bctx.graph.n_bonds - 40);
        /* prediction hotness */
        fprintf(stderr, "\n--- bond prediction ---\n");
        bond_hotness = bond_predict_hotness(&bctx, n_layers);
        bond_predict_print(&bctx, bond_hotness);
        bond_graph_free(&bctx.graph);
    }

    /* ── if --count-only, report hex/bond summary and exit ── */
    if (opt_count_only) {
        fprintf(stderr, "\n[tensor] gguf_tensors=%llu found_via_scan=%d n_layers=%d\n",
            (unsigned long long)gidx.n_tensors, n_found, n_layers);
        if (bond_hotness) free(bond_hotness);
        gguf_idx_close(&gidx);
        llama_model_free(model); llama_backend_free(); return 0;
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

    fprintf(stderr, "[sid] cached tensors: %d (verification skipped for speed)\n", n_weight_tensors);

    free(verify_buf);
    free(read_buf);

    /* ── DRamTile init (zero-copy tensor store) ── */
    if (g_opt_dramtile && n_weight_tensors > 0) {
        fprintf(stderr, "\n--- DRamTile init ---\n");
        if (dt_store_init(&g_dramtile, total_bytes + (1u << 20)) == 0) {
            fprintf(stderr, "[dramtile] mmap'd %zu bytes (%d weight tensors)\n",
                g_dramtile.capacity, n_weight_tensors);

            /* Populate DRamTile from SID cache: dump each cached tensor
             * into the DRamTile mmap region at its deterministic address. */
            int dt_loaded = 0;
            size_t dt_bytes = 0;
            for (int si = 0; si < n_sid_swaps; si++) {
                const char *name = found_tensors[si].name;
                uint8_t *cached = NULL; size_t cached_sz = 0;
                if (sid_cache_get(&sid_cache, name, &cached, &cached_sz) != 0)
                    continue;
                uint8_t *dt_ptr = dt_put(&g_dramtile, name, cached, cached_sz);
                if (dt_ptr) {
                    dt_loaded++;
                    dt_bytes += cached_sz;
                }
            }
            /* Also load tensors that weren't in SID swap set but are weight tensors */
            int dt_extra = 0;
            for (uint64_t gi = 0; gi < gidx.n_tensors; gi++) {
                if (sid_loader_is_norm(gidx.names[gi])) continue;
                if (dt_get(&g_dramtile, gidx.names[gi]) != NULL) continue; /* already loaded */
                uint8_t *cached = NULL; size_t cached_sz = 0;
                if (sid_cache_get(&sid_cache, gidx.names[gi], &cached, &cached_sz) != 0)
                    continue;
                if (dt_put(&g_dramtile, gidx.names[gi], cached, cached_sz)) {
                    dt_extra++;
                    dt_bytes += cached_sz;
                }
            }
            fprintf(stderr, "[dramtile] loaded %d+%d tensors (%zu bytes, %.1f%% of pool)\n",
                dt_loaded, dt_extra, dt_bytes,
                100.0 * dt_bytes / g_dramtile.capacity);
        } else {
            fprintf(stderr, "[dramtile] init failed — falling back to heap\n");
            g_opt_dramtile = 0;
        }
    }

    /* ── Time travel init ── */
    if (sid_face > 0) {
        fprintf(stderr, "[timetravel] initialized: delta ring=%d entries, max checkpoints=%d\n",
            SID_DELTA_MAX_ENTRIES, SID_CHECKPOINT_MAX);
    }

    /* ── Hybrid cluster: hex_grid hot epicenter + goldberg geodesic neighbors ── */
    uint8_t *hybrid_include = NULL;
    if (opt_hybrid && bond_hotness && sid_face > 0) {
        fprintf(stderr, "\n--- hybrid cluster (hot epicenter + geodesic neighbors) ---\n");
        fprintf(stderr, "[hybrid] radius=%.4f rad\n", opt_hybrid_radius);
        THGridState hgs;
        th_grid_init(&hgs, 2, n_layers);
        THCoord *hcoords = (THCoord*)calloc((size_t)n_found, sizeof(THCoord));
        for (int i = 0; i < n_found; i++)
            hcoords[i] = th_from_name(found_tensors[i].name, &hgs);

        uint8_t *is_hot = (uint8_t*)calloc((size_t)n_found, 1);
        int n_hot_cached = 0;
        for (int i = 0; i < n_found; i++) {
            if (bond_hotness[i] >= 0.9f) {
                uint8_t *cached = NULL; size_t cached_sz = 0;
                if (sid_cache_get(&sid_cache, found_tensors[i].name, &cached, &cached_sz) == 0) {
                    is_hot[i] = 1;
                    n_hot_cached++;
                }
            }
        }
        fprintf(stderr, "[hybrid] hot epicenter: %d cached tensors (hotness >= 0.9)\n", n_hot_cached);

        hybrid_include = (uint8_t*)calloc((size_t)n_found, 1);
        int n_included = 0;
        for (int i = 0; i < n_found; i++) {
            uint8_t *cached = NULL; size_t cached_sz = 0;
            if (sid_cache_get(&sid_cache, found_tensors[i].name, &cached, &cached_sz) != 0)
                continue;
            if (is_hot[i]) {
                hybrid_include[i] = 1;
                n_included++;
            } else {
                for (int j = 0; j < n_found; j++) {
                    if (!is_hot[j]) continue;
                    double geo = goldberg_geodesic(hcoords[i], hcoords[j]);
                    if (geo <= opt_hybrid_radius) {
                        hybrid_include[i] = 1;
                        n_included++;
                        break;
                    }
                }
            }
        }
        fprintf(stderr, "[hybrid] cluster size: %d / %d tensors\n", n_included, n_found);
        free(is_hot);
        free(hcoords);
    }

    /* ── Multi-depth: inject at N hottest layers simultaneously ── */
    uint8_t *multi_include = NULL;
    if (opt_sid_multi > 0 && bond_hotness && sid_face > 0) {
        fprintf(stderr, "\n--- multi-depth injection: %d hottest layers ---\n", opt_sid_multi);
        /* compute per-layer hotness average */
        float layer_hot[64] = {0};
        int layer_cnt[64] = {0};
        for (int i = 0; i < n_found; i++) {
            int l = th_extract_layer(found_tensors[i].name);
            if (l < 0 || l >= 64) continue;
            layer_hot[l] += bond_hotness[i];
            layer_cnt[l]++;
        }
        for (int l = 0; l < 64; l++)
            if (layer_cnt[l] > 0) layer_hot[l] /= layer_cnt[l];

        /* find N hottest layers */
        int hot_layers[64] = {0};
        int n_hot_layers = opt_sid_multi;
        if (n_hot_layers > n_layers) n_hot_layers = n_layers;
        for (int n = 0; n < n_hot_layers; n++) {
            int best = -1;
            for (int l = 0; l < n_layers; l++) {
                int skip = 0;
                for (int k = 0; k < n; k++) { if (hot_layers[k] == l) { skip = 1; break; } }
                if (skip) continue;
                if (best < 0 || layer_hot[l] > layer_hot[best]) best = l;
            }
            hot_layers[n] = best;
        }
        fprintf(stderr, "[multi] selected layers: ");
        for (int n = 0; n < n_hot_layers; n++)
            fprintf(stderr, "L%d(%.2f)%s", hot_layers[n], layer_hot[hot_layers[n]], n < n_hot_layers-1 ? " " : "\n");

        multi_include = (uint8_t*)calloc((size_t)n_found, 1);
        int n_multi = 0;
        for (int i = 0; i < n_found; i++) {
            int l = th_extract_layer(found_tensors[i].name);
            for (int n = 0; n < n_hot_layers; n++) {
                if (l == hot_layers[n]) { multi_include[i] = 1; n_multi++; break; }
            }
        }
        fprintf(stderr, "[multi] %d / %d tensors from %d layers\n", n_multi, n_found, n_hot_layers);
    }

    /* ── SID coordinate: (face, spoke, slot) → tensor filter ── */
    /*    face  = chooses SID face data (currently just on/off)        */
    /*    spoke = layer index (0..23) or -1=all, matches blk.<spoke>.  */
    /*    slot  = tensor name substring (e.g. "attn_q", "ffn_gate")    */
    /*    bond  = when --bond active, skip "cold" tensors (hotness<0.3)*/
    if (sid_face > 0) {
        fprintf(stderr, "\n--- SID swap setup (face=%d, spoke=%d, slot=\"%s\") ---\n",
            sid_face, sid_spoke, sid_slot);
        if (bond_hotness) {
            if (opt_sid_multi > 0 && multi_include)
                fprintf(stderr, "[sid] multi-depth filter active: %d hottest layers\n", opt_sid_multi);
            else if (opt_hybrid)
                fprintf(stderr, "[sid] hybrid filter active: hot epicenter + geodesic neighbors\n");
            else if (opt_swap_cold)
                fprintf(stderr, "[sid] bond filter inverted (--swap-cold): will skip hot/warm tensors (hotness >= 0.3)\n");
            else
                fprintf(stderr, "[sid] bond filter active: will skip cold tensors (hotness < 0.3)\n");
        }
        if (g_opt_gear_lock)
            fprintf(stderr, "[gear] route stability feedback active (threshold=%.2f)\n", g_opt_gear_threshold);
        for (int i = 0; i < n_found && n_sid_swaps < MAX_SID_SWAPS; i++) {
            const char *name = found_tensors[i].name;
            /* bond prediction filter */
            if (bond_hotness) {
                if (opt_sid_multi > 0 && multi_include) { if (!multi_include[i]) continue; }
                else if (opt_hybrid)        { if (!hybrid_include[i]) continue; }
                else if (opt_swap_cold) { if (bond_hotness[i] >= 0.3f) continue; }
                else                   { if (bond_hotness[i] < 0.3f)  continue; }
            }

            if (sid_spoke >= 0) {
                char expected[32];
                snprintf(expected, sizeof(expected), "blk.%d.", sid_spoke);
                if (strncmp(name, expected, strlen(expected)) != 0) continue;
            }
            if (sid_slot[0] != '\0' && strstr(name, sid_slot) == NULL) continue;

            uint8_t *cached = NULL; size_t cached_sz = 0;
            if (g_opt_dramtile) {
                cached = dt_get(&g_dramtile, name);
                if (cached) cached_sz = dt_get_size(&g_dramtile, name);
            }
            if (!cached) {
                if (sid_cache_get(&sid_cache, name, &cached, &cached_sz) != 0) continue;
            }

            /* encode per-tensor sid_mode based on hotness */
            uint8_t sid_mode = 0;
            if (bond_hotness) {
                float h = bond_hotness[i];
                if (h >= 0.9f)      sid_mode = geo_encode_sid(0, 0);   /* xor:0 — hot */
                else if (h >= 0.3f) sid_mode = geo_encode_sid(1, 0);   /* set:0 — warm */
                else                sid_mode = geo_encode_sid(2, 1);   /* rot:1 — cold */
            } else {
                sid_mode = geo_encode_sid(0, 1);  /* xor:1 — default */
            }

            sid_swaps[n_sid_swaps].ft_idx = i;
            sid_swaps[n_sid_swaps].geo_addr = geo_addr(name, i, sid_mode);
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
        /* ── goldberg geodesic expansion ── */
        if (opt_sid_geodesic && n_sid_swaps > 0) {
            fprintf(stderr, "[sid] geodesic expansion: radius=%.2f rad\n", opt_sid_geo_radius);
            THGridState ggs;
            th_grid_init(&ggs, 2, n_layers);
            THCoord *gcoords = (THCoord*)calloc((size_t)n_found, sizeof(THCoord));
            for (int i = 0; i < n_found; i++)
                gcoords[i] = th_from_name(found_tensors[i].name, &ggs);

            /* build quick swap-set for O(1) lookup */
            uint8_t *in_swap = (uint8_t*)calloc((size_t)n_found, 1);
            for (int s = 0; s < n_sid_swaps; s++)
                in_swap[sid_swaps[s].ft_idx] = 1;

            int added = 0;
            for (int s = 0; s < n_sid_swaps; s++) {
                int src = sid_swaps[s].ft_idx;
                THCoord sc = gcoords[src];
                for (int t = 0; t < n_found && n_sid_swaps < MAX_SID_SWAPS; t++) {
                    if (in_swap[t] || t == src) continue;
                    double geo = goldberg_geodesic(sc, gcoords[t]);
                    if (geo > opt_sid_geo_radius) continue;
                    uint8_t *cached = NULL; size_t cached_sz = 0;
                    if (sid_cache_get(&sid_cache, found_tensors[t].name, &cached, &cached_sz) != 0)
                        continue;
                    in_swap[t] = 1;
                    /* encode sid_mode for geodesic neighbor (rot:1 — mild) */
                    uint8_t geo_sid_mode = geo_encode_sid(2, 1);
                    sid_swaps[n_sid_swaps].ft_idx = t;
                    sid_swaps[n_sid_swaps].geo_addr = geo_addr(found_tensors[t].name, t, geo_sid_mode);
                    sid_swaps[n_sid_swaps].sid_data = cached;
                    sid_swaps[n_sid_swaps].sid_size = cached_sz;
                    sid_swaps[n_sid_swaps].is_malloc = 0;
                    delta_ft_idx[n_sid_swaps] = t;
                    delta_tensor_ptr[n_sid_swaps] = found_tensors[t].ptr;
                    delta_orig_data[n_sid_swaps] = found_tensors[t].orig_data;
                    delta_sid_data[n_sid_swaps] = cached;
                    delta_size[n_sid_swaps] = cached_sz;
                    n_sid_swaps++; added++;
                }
            }
            fprintf(stderr, "[sid] geodesic added %d neighbors (total=%d)\n", added, n_sid_swaps);
            free(in_swap); free(gcoords);
        }
        if (n_sid_swaps == 0) {
            fprintf(stderr, "[sid] WARNING: no tensors matched coordinates. SID disabled.\n");
        } else {
            if (sid_corrupt) {
                int v = sid_corrupt_val;
                int n_adaptive = 0;
                int n_geo = 0;
                for (int i = 0; i < n_sid_swaps; i++) {
                    SIDSwapEntry *e = &sid_swaps[i];
                    uint8_t *d = e->sid_data;
                    size_t sz = e->sid_size;
                    size_t limit = sz < (size_t)sid_corrupt ? sz : (size_t)sid_corrupt;
                    size_t effective_limit = limit;

                    /* decode per-tensor pattern from geo_addr */
                    uint8_t geo_mode = geo_sid_mode(e->geo_addr);
                    uint8_t geo_pattern = geo_sid_pattern(geo_mode);
                    uint8_t geo_byte = geo_sid_byte(geo_mode);
                    int use_geo = (e->geo_addr != 0);

                    if (use_geo) {
                        /* per-tensor pattern from geo_addr */
                        if (geo_pattern == 0) {
                            /* xor:geo_byte */
                            for (size_t j = 0; j < effective_limit; j++)
                                d[j] ^= geo_byte;
                        } else if (geo_pattern == 1) {
                            /* set:geo_byte */
                            memset(d, geo_byte, effective_limit);
                        } else if (geo_pattern == 2) {
                            /* rot:geo_byte (K) */
                            int k = geo_byte; if (k <= 0) k = 1;
                            for (size_t j = 0; j < effective_limit; j++)
                                d[j] = (uint8_t)((d[j] + k) & 0xFF);
                        } else {
                            /* reserved — xor:1 */
                            for (size_t j = 0; j < effective_limit; j++)
                                d[j] ^= 1;
                        }
                        n_geo++;
                    } else if (sid_pattern) {
                        /* original pattern-based corruption */
                        if (opt_sid_adaptive && bond_hotness) {
                            float h = bond_hotness[e->ft_idx];
                            size_t scaled = (size_t)(limit * h + 0.5f);
                            effective_limit = scaled < 1 ? 1 : scaled;
                            if (effective_limit > sz) effective_limit = sz;
                            n_adaptive++;
                        }
                        char pat[64]; strncpy(pat, sid_pattern, 63); pat[63] = 0;
                        char *colon = strchr(pat, ':');
                        if (colon) *colon++ = 0;
                        int pval = colon ? atoi(colon) : v;
                        if (strcmp(pat, "set") == 0) {
                            memset(d, (uint8_t)pval, effective_limit);
                        } else if (strcmp(pat, "zero") == 0) {
                            memset(d, 0, effective_limit);
                        } else if (strcmp(pat, "rot") == 0) {
                            int k = pval; if (k <= 0) k = 1;
                            for (size_t j = 0; j < effective_limit; j++)
                                d[j] = (uint8_t)((d[j] + k) & 0xFF);
                        } else {
                            for (size_t j = 0; j < effective_limit; j++)
                                d[j] ^= (uint8_t)v;
                        }
                    } else {
                        for (size_t j = 0; j < effective_limit; j++)
                            d[j] ^= (uint8_t)v;
                    }
                }
                if (n_geo > 0) {
                    fprintf(stderr, "[sid] GEO_ADDR: per-tensor patterns for %d tensors\n", n_geo);
                }
                if (opt_sid_adaptive && bond_hotness) {
                    fprintf(stderr, "[sid] ADAPTIVE: corruption scaled by hotness for %d tensors (base=%d bytes)\n",
                        n_adaptive, sid_corrupt);
                } else {
                    fprintf(stderr, "[sid] CORRUPTED first %d bytes of %d swapped tensors (pattern=%s byte=%d)\n",
                        sid_corrupt, n_sid_swaps, sid_pattern ? sid_pattern : "xor", v);
                }
            }
            fprintf(stderr, "[sid] %d / %d tensors will be swapped per decode", n_sid_swaps, n_found);
            if (bond_hotness && !opt_hybrid) {
                int n_skipped = 0;
                float threshold = opt_swap_cold ? 0.3f : 0.3f;
                for (int i = 0; i < n_found; i++)
                    if (opt_swap_cold ? (bond_hotness[i] >= threshold) : (bond_hotness[i] < threshold))
                        n_skipped++;
                if (n_skipped > 0)
                    fprintf(stderr, " (bond filter skipped %d %s)", n_skipped,
                        opt_swap_cold ? "hot/warm" : "cold");
            }
            fprintf(stderr, "\n");
        }
    }
    /* ── Gear lock init (after SID swap setup so delta_ft_idx[] is populated) ── */
    if (g_opt_gear_lock) {
        gear_lock_init(&g_gear);
        /* initial mask: all swaps active (gear lock hasn't seen cycles yet) */
        memset(gear_active_mask, 1, sizeof(gear_active_mask));
        n_gear_active = n_sid_swaps;
        fprintf(stderr, "[gear] state initialized, %d swaps monitored\n", n_gear_active);
    }

    /* ── Tensor memory store init ── */
    if (g_opt_mem_store) {
        g_tmem_buf_size = 64u * 1024 * 1024;
        g_tmem_buf = (uint8_t*)malloc(g_tmem_buf_size);
        if (g_tmem_buf) {
            tmem_init(&g_tmem_store, g_tmem_buf, g_tmem_buf_size, 0);
            /* try loading existing file (if any) */
            FILE *tf = fopen(g_opt_mem_store, "rb");
            if (tf) { fclose(tf); tmem_load(&g_tmem_store, g_tmem_buf, g_tmem_buf_size, g_opt_mem_store); }
            fprintf(stderr, "[mem-store] initialized (%zu MB buffer, %u existing records)\n",
                g_tmem_buf_size >> 20, g_tmem_store.n_records);
        } else {
            fprintf(stderr, "[mem-store] malloc(%zu) failed, disabled\n", g_tmem_buf_size);
            g_opt_mem_store = NULL;
        }
    }
    /* ── Icosa bridge (twin GPU lane) init ── */
    fprintf(stderr, "[debug] g_opt_twin_gpu=%d\n", g_opt_twin_gpu);
    if (g_opt_twin_gpu) {
        char dll_path[512];
        pogls_module_dir(dll_path, sizeof(dll_path));
        strcat(dll_path, "icosa_bridge.dll");
        if (icosa_bridge_load(&g_ibridge, dll_path) == 0) {
            g_ibridge_ctx = g_ibridge.create(0, 0xDEADBEEFCAFEBABEULL);
            if (g_ibridge_ctx && g_ibridge.valid(g_ibridge_ctx)) {
                fprintf(stderr, "[twin-gpu] icosa bridge loaded, GPU ready\n");
            } else {
                fprintf(stderr, "[twin-gpu] GPU init failed — disabling\n");
                if (g_ibridge_ctx) { g_ibridge.destroy(g_ibridge_ctx); g_ibridge_ctx = NULL; }
                icosa_bridge_unload(&g_ibridge);
                g_opt_twin_gpu = 0;
            }
        } else {
            fprintf(stderr, "[twin-gpu] icosa_bridge.dll not found — disabled\n");
            g_opt_twin_gpu = 0;
        }
    }

    /* ── Init redirect: set tensor->data to writable SID cache heap BEFORE context creation ── */
    if (sid_face > 0 && n_sid_swaps > 0) {
        fprintf(stderr, "[init-redirect] %d tensors: tensor->data → heap (SID cache)\n", n_sid_swaps);
        for (int s = 0; s < n_sid_swaps; s++) {
            int fi = sid_swaps[s].ft_idx;
            tensor_set_data(found_tensors[fi].ptr, found_tensors[fi].orig_data);
        }
    }

    /* ── Save clean SID data ptrs for cosplay-compare and experiment ── */
    if ((g_opt_cosplay_compare || g_opt_experiment) && sid_face > 0 && n_sid_swaps > 0) {
        g_n_experiment_clean = n_sid_swaps;
        g_experiment_clean_ptrs = (uint8_t**)malloc((size_t)n_sid_swaps * sizeof(uint8_t*));
        if (g_experiment_clean_ptrs) {
            for (int s = 0; s < n_sid_swaps; s++)
                g_experiment_clean_ptrs[s] = sid_swaps[s].sid_data;
        }
        fprintf(stderr, "[cosplay] saved %d clean ptrs\n", g_n_experiment_clean);
    }

    /* ── Cosplay: apply profile perturbations to SID cache data ── */
    if (!g_opt_experiment && g_opt_cosplay && g_cp.n > 0 && sid_face > 0 && n_sid_swaps > 0) {
        fprintf(stderr, "[cosplay] applying %u perturbation entries...\n", g_cp.n);
        for (int s = 0; s < n_sid_swaps; s++) {
            uint32_t h = cosplay_fnv1a(found_tensors[sid_swaps[s].ft_idx].name);
            int ei = cosplay_find(&g_cp, h);
            if (ei < 0) continue;
            void *perturbed = cosplay_apply(&g_cp.entries[ei],
                sid_swaps[s].sid_data, sid_swaps[s].sid_size);
            if (perturbed) {
                sid_swaps[s].sid_data = (uint8_t*)perturbed;
                sid_swaps[s].is_malloc = 1;
                delta_sid_data[s] = sid_swaps[s].sid_data;
            }
        }
        fprintf(stderr, "[cosplay] applied\n");
    }

    /* ── Session profile init ── */
    ses_profile_init(&g_ses);
    if (g_opt_simulate || g_opt_profile_batch) {
        fprintf(stderr, "[ses] mode: %s\n", g_opt_profile_batch ? "batch" : "simulate");
    }

    /* ── Create context (once, for the whole session) ── */
    struct llama_context_params cp=llama_context_default_params();
    cp.n_ctx=opt_ctx;cp.n_threads=4;cp.n_threads_batch=4;cp.n_batch=opt_ctx<512?opt_ctx:512;cp.n_ubatch=64;
    struct llama_context *lctx = llama_init_from_model(model, cp);
    if(!lctx){fprintf(stderr,"ERROR: context\n"); sid_loader_close(&slc); gguf_idx_close(&gidx); llama_model_free(model); llama_backend_free(); return 1;}
    const struct llama_vocab *v = llama_model_get_vocab(model);
    int nv = llama_vocab_n_tokens(v);

    /* ── Init KV swap ── */
    kv_swap_init(&g_kv_swap, lctx, g_opt_kv_swap, g_opt_kv_layer);

    /* ── Init KV SID evict (disabled unless compiled with -DKV_ARCHIVE) ── */
    KVSidCtx kv_sid;
    kv_sid_init(&kv_sid);
#ifdef KV_ARCHIVE
    {
        void *k_tensors[KV_SID_MAX_LAYERS], *v_tensors[KV_SID_MAX_LAYERS];
        void *k_data[KV_SID_MAX_LAYERS], *v_data[KV_SID_MAX_LAYERS];
        size_t k_sizes[KV_SID_MAX_LAYERS], v_sizes[KV_SID_MAX_LAYERS];
        int layer_ids[KV_SID_MAX_LAYERS];
        int n_kv = kv_get_cache_tensors(lctx, k_data, v_data, k_sizes, v_sizes, NULL, NULL, layer_ids, KV_SID_MAX_LAYERS);
        int n_tensors = kv_get_cache_tensor_ptrs(lctx, k_tensors, v_tensors, KV_SID_MAX_LAYERS);
        int n_reg = n_kv < n_tensors ? n_kv : n_tensors;
        if (n_reg > 0) {
            kv_sid_register_layers(&kv_sid, k_tensors, v_tensors, k_data, v_data, k_sizes, v_sizes, layer_ids, n_reg);
            kv_snapshot_all(&kv_sid);  /* compress initial KV state */
            if (opt_kv_evict > 0) {
                kv_sid_evict_oldest(&kv_sid, opt_kv_evict);
            }
            kv_sid_print_status(&kv_sid);
        }
    }
#endif /* KV_ARCHIVE */

    /* ── Init KV Page Store (optional, --kv-page) ── */
    KVPageStore kv_page;
    kv_page_init(&kv_page, opt_ctx);
    if (opt_kv_page) {
        void *k_tensors_pg[KV_PAGE_MAX_LAYERS], *v_tensors_pg[KV_PAGE_MAX_LAYERS];
        void *k_data_pg[KV_PAGE_MAX_LAYERS], *v_data_pg[KV_PAGE_MAX_LAYERS];
        size_t k_nb1[KV_PAGE_MAX_LAYERS], v_nb1[KV_PAGE_MAX_LAYERS];
        size_t k_sizes_pg[KV_PAGE_MAX_LAYERS], v_sizes_pg[KV_PAGE_MAX_LAYERS];
        int n_embd_ks[KV_PAGE_MAX_LAYERS], layer_ids_pg[KV_PAGE_MAX_LAYERS];
        int n_kv = kv_get_cache_tensors(lctx, k_data_pg, v_data_pg, k_sizes_pg, v_sizes_pg,
                                         n_embd_ks, NULL, layer_ids_pg, KV_PAGE_MAX_LAYERS);
        int n_tensors = kv_get_cache_tensor_ptrs(lctx, k_tensors_pg, v_tensors_pg, KV_PAGE_MAX_LAYERS);
        int n_reg = n_kv < n_tensors ? n_kv : n_tensors;
        if (n_reg > 0) {
            for (int i = 0; i < n_reg; i++) {
                k_nb1[i] = k_sizes_pg[i] / (size_t)opt_ctx;
                v_nb1[i] = v_sizes_pg[i] / (size_t)opt_ctx;
            }
            kv_page_register_layers(&kv_page, k_tensors_pg, v_tensors_pg,
                k_data_pg, v_data_pg, k_nb1, v_nb1, k_sizes_pg, v_sizes_pg,
                n_embd_ks, layer_ids_pg, n_reg);
            kv_page_snapshot_all(&kv_page);
            if (opt_kv_page_evict > 0) {
                kv_page_evict_oldest(&kv_page, opt_kv_page_evict);
            }
            kv_page_print_status(&kv_page);
        }
    }

    /* ── Init KV Remap (optional, --remap) ── */
    if (g_opt_remap) {
        void *remap_k_data[KV_REMAP_MAX_LAYERS], *remap_v_data[KV_REMAP_MAX_LAYERS];
        size_t remap_k_nb1[KV_REMAP_MAX_LAYERS], remap_v_nb1[KV_REMAP_MAX_LAYERS];
        size_t remap_k_size[KV_REMAP_MAX_LAYERS], remap_v_size[KV_REMAP_MAX_LAYERS];
        int remap_n_embd[KV_REMAP_MAX_LAYERS], remap_layer_id[KV_REMAP_MAX_LAYERS];
        int n_kv = kv_get_cache_tensors(lctx, remap_k_data, remap_v_data,
                                         remap_k_size, remap_v_size,
                                         remap_n_embd, NULL, remap_layer_id,
                                         KV_REMAP_MAX_LAYERS);
        if (n_kv > 0) {
            for (int i = 0; i < n_kv; i++) {
                remap_k_nb1[i] = remap_k_size[i] / (size_t)opt_ctx;
                remap_v_nb1[i] = remap_v_size[i] / (size_t)opt_ctx;
            }
            kv_remap_init(&g_remap_ctx, opt_ctx);
            kv_remap_register(&g_remap_ctx, remap_k_data, remap_v_data,
                              remap_k_nb1, remap_v_nb1,
                              remap_k_size, remap_v_size,
                              remap_n_embd, remap_layer_id, n_kv);
            /* Skeleton set AFTER first prompt decode (KV needs real data) */
            g_remap_ctx.skeleton_valid = 0;
            kv_remap_rail_init(&g_remap_rail, &g_remap_ctx);
            if (g_opt_shadow) {
                kv_remap_init_shadow(&g_remap_ctx);
                fprintf(stderr, "[remap] shadow zone active\n");
            }
            fprintf(stderr, "[remap] initialized with %d KV layers, ctx=%d (skeleton deferred to first decode)\n", n_kv, opt_ctx);
        } else {
            fprintf(stderr, "[remap] no KV cache layers found, disabled\n");
            g_opt_remap = 0;
        }
    }

    /* ── Cosplay-compare: decode WITH and WITHOUT cosplay ── */
    if (g_opt_cosplay_compare && g_opt_cosplay && g_cp.n > 0 && sid_face > 0 && n_sid_swaps > 0 && g_experiment_clean_ptrs) {
        Sampler cs_sp = sp; cs_sp.count = 0;
        llama_token ct = 0;
        llama_batch cb = llama_batch_get_one(&ct, 1);
        int c_tok_with = -1, c_tok_without = -1;
        float *c_logits_with = NULL, *c_logits_without = NULL;
        fprintf(stderr, "\n--- cosplay-compare ---\n");
        sid_swap_apply();
        if (llama_decode(lctx, cb) == 0) {
            float *lw = llama_get_logits_ith(lctx, 0);
            c_logits_with = (float*)malloc((size_t)nv * sizeof(float));
            if (c_logits_with) { memcpy(c_logits_with, lw, (size_t)nv * sizeof(float)); c_tok_with = sample_token(lw, nv, &cs_sp); }
        }
        sid_swap_restore();
        llama_memory_clear(llama_get_memory(lctx), 1);
        for (int s = 0; s < n_sid_swaps; s++) delta_sid_data[s] = g_experiment_clean_ptrs[s];
        sid_swap_apply();
        if (llama_decode(lctx, cb) == 0) {
            float *lwo = llama_get_logits_ith(lctx, 0);
            c_logits_without = (float*)malloc((size_t)nv * sizeof(float));
            if (c_logits_without) { memcpy(c_logits_without, lwo, (size_t)nv * sizeof(float)); c_tok_without = sample_token(lwo, nv, &cs_sp); }
        }
        sid_swap_restore();
        for (int s = 0; s < n_sid_swaps; s++) delta_sid_data[s] = sid_swaps[s].sid_data;
        llama_memory_clear(llama_get_memory(lctx), 1);
        if (c_logits_with && c_logits_without) {
            double c_dot = 0, c_na = 0, c_nb = 0, c_md = 0; int c_ss = 0;
            for (int i = 0; i < nv; i++) {
                c_dot += c_logits_with[i] * c_logits_without[i];
                c_na += c_logits_with[i] * c_logits_with[i];
                c_nb += c_logits_without[i] * c_logits_without[i];
                double cd = fabs(c_logits_with[i] - c_logits_without[i]);
                if (cd > c_md) c_md = cd;
                if ((c_logits_with[i] >= 0) == (c_logits_without[i] >= 0)) c_ss++;
            }
            double c_sim = c_dot / (sqrt(c_na) * sqrt(c_nb) + 1e-30);
            char c_buf_with[32], c_buf_without[32];
            int c_lw = 0, c_lwo = 0;
            if (c_tok_with >= 0) c_lw = llama_token_to_piece(v, c_tok_with, c_buf_with, 32, 0, false);
            if (c_tok_without >= 0) c_lwo = llama_token_to_piece(v, c_tok_without, c_buf_without, 32, 0, false);
            if (c_lw > 0) c_buf_with[c_lw > 31 ? 31 : c_lw] = 0; else c_buf_with[0] = 0;
            if (c_lwo > 0) c_buf_without[c_lwo > 31 ? 31 : c_lwo] = 0; else c_buf_without[0] = 0;
            fprintf(stderr, "  First token WITH cosplay:    '%s' (token %d)\n", c_buf_with, c_tok_with);
            fprintf(stderr, "  First token WITHOUT cosplay: '%s' (token %d)\n", c_buf_without, c_tok_without);
            fprintf(stderr, "  Same token? %s\n", c_tok_with == c_tok_without ? "YES" : "NO");
            fprintf(stderr, "  Logits cosine similarity:    %f\n", c_sim);
            fprintf(stderr, "  Max logit difference:        %f\n", c_md);
            fprintf(stderr, "  Same-sign ratio:             %.1f%%\n", 100.0 * c_ss / nv);
        }
        free(c_logits_with); free(c_logits_without);
        fprintf(stderr, "--- end cosplay-compare ---\n\n");
    }

    /* ── Experiment: multi-condition comparison ── */
    if (g_opt_experiment && sid_face > 0 && n_sid_swaps > 0 && g_experiment_clean_ptrs) {
        fprintf(stderr, "\n--- experiment: %s ---\n", g_opt_experiment);
        #ifdef _WIN32
        _mkdir(g_opt_experiment);
        #endif
        int e_max_cpl = 64, e_n_cpl = 0;
        char e_cpl_list[64][260];
        #ifdef _WIN32
        WIN32_FIND_DATA e_fd;
        char e_pat[320]; snprintf(e_pat, 320, "%s\\*.cpl", g_opt_experiment);
        HANDLE e_fh = FindFirstFile(e_pat, &e_fd);
        if (e_fh != INVALID_HANDLE_VALUE) {
            do {
                if (e_n_cpl >= e_max_cpl) break;
                if (!(e_fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    snprintf(e_cpl_list[e_n_cpl], 260, "%s\\%s", g_opt_experiment, e_fd.cFileName);
                    e_n_cpl++;
                }
            } while (FindNextFile(e_fh, &e_fd));
            FindClose(e_fh);
        }
        #else
        DIR *e_d = opendir(g_opt_experiment);
        if (e_d) {
            struct dirent *e_de;
            while ((e_de = readdir(e_d)) != NULL && e_n_cpl < e_max_cpl) {
                const char *e_ext = strrchr(e_de->d_name, '.');
                if (e_ext && strcmp(e_ext, ".cpl") == 0) {
                    snprintf(e_cpl_list[e_n_cpl], 260, "%s/%s", g_opt_experiment, e_de->d_name);
                    e_n_cpl++;
                }
            }
            closedir(e_d);
        }
        #endif
        qsort(e_cpl_list, (size_t)e_n_cpl, 260, (int(*)(const void*,const void*))strcmp);
        fprintf(stderr, "[experiment] found %d .cpl files\n", e_n_cpl);
        int e_n_cond = e_n_cpl + 1;
        float **e_all_logits = (float**)calloc((size_t)e_n_cond, sizeof(float*));
        int *e_all_first_tok = (int*)calloc((size_t)e_n_cond, sizeof(int));
        char e_cond_names[64][64];
        e_cond_names[0][0] = 0; snprintf(e_cond_names[0], 64, "00-baseline");
        char e_res_dir[320];
        snprintf(e_res_dir, 320, "%s\\results", g_opt_experiment);
        #ifdef _WIN32
        _mkdir(e_res_dir);
        #endif
        Sampler e_sp = sp; e_sp.count = 0;
        llama_token e_tok = 0;
        llama_batch e_batch = llama_batch_get_one(&e_tok, 1);
        for (int e_ci = 0; e_ci < e_n_cond; e_ci++) {
            for (int s = 0; s < n_sid_swaps; s++) delta_sid_data[s] = g_experiment_clean_ptrs[s];
            if (e_ci > 0) {
                char e_cpl_path[320]; snprintf(e_cpl_path, 320, "%s", e_cpl_list[e_ci - 1]);
                const char *e_base = strrchr(e_cpl_path, '\\');
                if (!e_base) e_base = strrchr(e_cpl_path, '/');
                if (!e_base) e_base = e_cpl_path; else e_base++;
                snprintf(e_cond_names[e_ci], 64, "%02d-%s", e_ci, e_base);
                char *e_dot = strstr(e_cond_names[e_ci], ".cpl");
                if (e_dot) *e_dot = 0;
                CosplayProfile e_cp; memset(&e_cp, 0, sizeof(e_cp));
                if (cosplay_load(e_cpl_path, &e_cp) == 0 && e_cp.n > 0) {
                    for (int s = 0; s < n_sid_swaps; s++) {
                        uint32_t eh = cosplay_fnv1a(found_tensors[sid_swaps[s].ft_idx].name);
                        int eei = cosplay_find(&e_cp, eh);
                        if (eei < 0) continue;
                        void *ep = cosplay_apply(&e_cp.entries[eei], sid_swaps[s].sid_data, sid_swaps[s].sid_size);
                        if (ep) { delta_sid_data[s] = (uint8_t*)ep; }
                    }
                }
            }
            sid_swap_apply();
            if (llama_decode(lctx, e_batch) == 0) {
                float *el = llama_get_logits_ith(lctx, 0);
                e_all_first_tok[e_ci] = sample_token(el, nv, &e_sp);
                e_all_logits[e_ci] = (float*)malloc((size_t)nv * sizeof(float));
                if (e_all_logits[e_ci]) memcpy(e_all_logits[e_ci], el, (size_t)nv * sizeof(float));
            }
            sid_swap_restore();
            for (int s = 0; s < n_sid_swaps; s++) {
                if (delta_sid_data[s] != sid_swaps[s].sid_data && delta_sid_data[s] != g_experiment_clean_ptrs[s])
                    free(delta_sid_data[s]);
            }
            for (int s = 0; s < n_sid_swaps; s++) delta_sid_data[s] = g_experiment_clean_ptrs[s];
            llama_memory_clear(llama_get_memory(lctx), 1);
            char e_out_dir[320]; snprintf(e_out_dir, 320, "%s\\%s", e_res_dir, e_cond_names[e_ci]);
            #ifdef _WIN32
            _mkdir(e_out_dir);
            #endif
            FILE *ef = fopen(e_out_dir, "w"); if (ef) { fprintf(ef,""); fclose(ef); } /* touch */
            char e_info_path[320]; snprintf(e_info_path, 320, "%s\\info.txt", e_out_dir);
            FILE *e_info = fopen(e_info_path, "w");
            if (e_info) {
                fprintf(e_info, "condition: %s\n", e_cond_names[e_ci]);
                fprintf(e_info, "cpl: %s\n", e_ci == 0 ? "(none, baseline)" : e_cpl_list[e_ci - 1]);
                fprintf(e_info, "first_token: %d\n", e_all_first_tok[e_ci]);
                char e_p[32]; int e_p_l = 0;
                if (e_all_first_tok[e_ci] >= 0) e_p_l = llama_token_to_piece(v, e_all_first_tok[e_ci], e_p, 32, 0, false);
                if (e_p_l > 0) { e_p[e_p_l > 31 ? 31 : e_p_l] = 0; fprintf(e_info, "first_piece: %s\n", e_p); }
                fprintf(e_info, "n_tokens: %d\n", 1);
                fclose(e_info);
            }
            char e_tok_path[320]; snprintf(e_tok_path, 320, "%s\\tokens.bin", e_out_dir);
            FILE *e_tf = fopen(e_tok_path, "wb");
            if (e_tf) { fwrite(&e_all_first_tok[e_ci], 4, 1, e_tf); fclose(e_tf); }
            char e_txt_path[320]; snprintf(e_txt_path, 320, "%s\\tokens.txt", e_out_dir);
            FILE *e_txt = fopen(e_txt_path, "w");
            if (e_txt && e_all_first_tok[e_ci] >= 0) {
                char e_tp[32]; int e_tl = llama_token_to_piece(v, e_all_first_tok[e_ci], e_tp, 32, 0, false);
                if (e_tl > 0) { e_tp[e_tl > 31 ? 31 : e_tl] = 0; fprintf(e_txt, "%s", e_tp); }
                fclose(e_txt);
            } else if (e_txt) { fclose(e_txt); }
            char e_lg_path[320]; snprintf(e_lg_path, 320, "%s\\logits.bin", e_out_dir);
            FILE *e_lf = fopen(e_lg_path, "wb");
            if (e_lf && e_all_logits[e_ci]) { fwrite(e_all_logits[e_ci], 4, (size_t)nv, e_lf); fclose(e_lf); }
            fprintf(stderr, "[experiment] %s: first_token=%d\n", e_cond_names[e_ci], e_all_first_tok[e_ci]);
        }
        char e_rpt_path[320]; snprintf(e_rpt_path, 320, "%s\\report.txt", e_res_dir);
        FILE *e_rpt = fopen(e_rpt_path, "w");
        if (e_rpt) {
            fprintf(e_rpt, "EXPERIMENT REPORT: %s\n\n", g_opt_experiment);
            for (int e_ci = 0; e_ci < e_n_cond; e_ci++) {
                fprintf(e_rpt, "[%s]\n", e_cond_names[e_ci]);
                fprintf(e_rpt, "  first_token=%d\n", e_all_first_tok[e_ci]);
                fprintf(e_rpt, "\n");
            }
            fprintf(e_rpt, "Logit cosine similarity matrix:\n");
            for (int e_i = 0; e_i < e_n_cond; e_i++) {
                for (int e_j = 0; e_j < e_n_cond; e_j++) {
                    double e_sim = 0;
                    if (e_all_logits[e_i] && e_all_logits[e_j]) {
                        double e_d = 0, e_na = 0, e_nb = 0;
                        for (int e_k = 0; e_k < nv; e_k++) {
                            e_d += e_all_logits[e_i][e_k] * e_all_logits[e_j][e_k];
                            e_na += e_all_logits[e_i][e_k] * e_all_logits[e_i][e_k];
                            e_nb += e_all_logits[e_j][e_k] * e_all_logits[e_j][e_k];
                        }
                        e_sim = e_d / (sqrt(e_na) * sqrt(e_nb) + 1e-30);
                    }
                    fprintf(e_rpt, "%f\t", e_sim);
                }
                fprintf(e_rpt, "\n");
            }
            fclose(e_rpt);
        }
        for (int e_ci = 0; e_ci < e_n_cond; e_ci++) free(e_all_logits[e_ci]);
        free(e_all_logits); free(e_all_first_tok);
        fprintf(stderr, "--- end experiment ---\n");
        goto cleanup;
    }

    /* ── if --dump-logits, do one decode and exit ── */
    if (opt_dump_logits) {
        fprintf(stderr, "\n--- dump-logits to '%s' ---\n", opt_dump_logits);
        for (int s = 0; s < n_sid_swaps; s++)
            tensor_set_data(found_tensors[sid_swaps[s].ft_idx].ptr, sid_swaps[s].sid_data);

        llama_token tok = 0;
        llama_batch batch = llama_batch_get_one(&tok, 1);
        if (llama_decode(lctx, batch) != 0) {
            fprintf(stderr, "ERROR: decode failed\n");
        } else {
            float *logits = llama_get_logits_ith(lctx, 0);
            FILE *fp = fopen(opt_dump_logits, "wb");
            if (fp) {
                fwrite(&nv, sizeof(nv), 1, fp);
                fwrite(logits, sizeof(float), nv, fp);
                fclose(fp);
                fprintf(stderr, "[dump] %d logits written to %s\n", nv, opt_dump_logits);
            } else {
                fprintf(stderr, "ERROR: cannot write %s\n", opt_dump_logits);
            }
        }
        free(bond_hotness); free(hybrid_include); free(multi_include);
        goto cleanup;
    }
    free(bond_hotness); free(hybrid_include); free(multi_include);

    if(!opt_chat&&!opt_prompt&&!opt_dump_logits&&!opt_count_only){opt_chat=1;}
    if(opt_chat){
        char*roles[MAX_CHAT_HISTORY],*content[MAX_CHAT_HISTORY];int cc=0;
        roles[0]="system";content[0]=strdup("You are a helpful assistant.");cc=1;
        printf("\n=== SID Chat ===\n/exit  /clear  /evict N  /restore  /pevict N  /prestore N  /pstatus\n\n");
        FILE *chat_in = stdin;
        if (opt_script) {
            chat_in = fopen(opt_script, "rb");
            if (!chat_in) { fprintf(stderr, "ERROR: cannot open script file %s\n", opt_script); return 1; }
        }
        char line[MAX_LINE];
        int *pending_toks = NULL;
        size_t pending_n = 0;
        size_t pending_cap = 0;
        int pending_ready = 0;
        while(1){
            printf(">>> ");fflush(stdout);
            if(!fgets(line,sizeof(line),chat_in))break;
            size_t ll=strlen(line);while(ll>0&&(line[ll-1]=='\n'||line[ll-1]=='\r'))line[--ll]=0;
            if(ll==0)continue;
            if (opt_script) fprintf(stderr, "[chat-script] %s\n", line);
            if(!strcmp(line,"/exit"))break;
            if(!strcmp(line,"/clear")){for(int _ci=0;_ci<cc;_ci++)free(content[_ci]);cc=0;roles[0]="system";content[0]=strdup("You are a helpful assistant.");cc=1;free(pending_toks);pending_toks=NULL;pending_n=0;pending_cap=0;pending_ready=0;llama_free(lctx);lctx=llama_init_from_model(model,cp);if(!lctx){fprintf(stderr,"ERROR: reinit context\n");break;}v=llama_model_get_vocab(model);nv=llama_vocab_n_tokens(v);if(g_opt_remap){kv_remap_destroy(&g_remap_ctx);kv_remap_rail_destroy(&g_remap_rail);kv_remap_init(&g_remap_ctx, opt_ctx);void *rk[KV_REMAP_MAX_LAYERS],*rv[KV_REMAP_MAX_LAYERS];size_t rks[KV_REMAP_MAX_LAYERS],rvs[KV_REMAP_MAX_LAYERS],rknb[KV_REMAP_MAX_LAYERS],rvnb[KV_REMAP_MAX_LAYERS];int rne[KV_REMAP_MAX_LAYERS],rli[KV_REMAP_MAX_LAYERS];int nnk=kv_get_cache_tensors(lctx,rk,rv,rks,rvs,rne,NULL,rli,KV_REMAP_MAX_LAYERS);if(nnk>0){for(int ii=0;ii<nnk;ii++){rknb[ii]=rks[ii]/opt_ctx;rvnb[ii]=rvs[ii]/opt_ctx;}kv_remap_register(&g_remap_ctx,rk,rv,rknb,rvnb,rks,rvs,rne,rli,nnk);g_remap_ctx.skeleton_valid=0;kv_remap_rail_init(&g_remap_rail,&g_remap_ctx);if(g_opt_shadow)kv_remap_init_shadow(&g_remap_ctx);}}printf("Cleared.\n");continue;}
#ifdef KV_ARCHIVE
            if(!strncmp(line,"/evict ",7)){
                int _n=atoi(line+7);
                if(_n>0 && kv_sid.enabled){
                    kv_sid_evict_oldest(&kv_sid, _n);
                    /* Verify: write magic to ORIGINAL data to detect if model reads from it */
                    for (int i = 0; i < kv_sid.n_layers && i < _n; i++) {
                        KVSidLayer *L = &kv_sid.layers[i];
                        if (L->evicted && L->k_data && L->k_size >= 8) {
                            memset(L->k_data, 0xDE, L->k_size > 4096 ? 4096 : L->k_size);
                            fprintf(stderr, "[kv-debug] poisoned layer %d orig_k=%p %zu bytes with 0xDE\n",
                                i, L->k_data, L->k_size > 4096 ? 4096 : L->k_size);
                        }
                        if (L->evicted && L->v_data && L->v_size >= 8) {
                            memset(L->v_data, 0xAD, L->v_size > 4096 ? 4096 : L->v_size);
                            fprintf(stderr, "[kv-debug] poisoned layer %d orig_v=%p %zu bytes with 0xAD\n",
                                i, L->v_data, L->v_size > 4096 ? 4096 : L->v_size);
                        }
                    }
                    kv_sid_print_status(&kv_sid);
                    printf("Evicted %d layers (poisoned original data)\n", _n);
                }else{
                    printf("Usage: /evict N\n");
                }
                continue;
            }
            if(!strcmp(line,"/restore")){
                kv_sid_restore_all(&kv_sid);
                kv_sid_print_status(&kv_sid);
                printf("All layers restored\n");
                continue;
            }
            if(!strncmp(line,"/snap",5)){
                if (kv_sid.enabled) {
                    kv_snapshot_all(&kv_sid);
                    kv_sid_print_status(&kv_sid);
                    printf("Snapshot complete\n");
                } else {
                    printf("KV archive not enabled (recompile with -DKV_ARCHIVE)\n");
                }
                continue;
            }
            if(!strcmp(line,"/unsnap")){
                if (kv_sid.enabled) {
                    kv_sid_restore_all(&kv_sid);  /* swap ptrs back to original */
                    kv_restore_all(&kv_sid);       /* decompress to original */
                    kv_sid_print_status(&kv_sid);
                    printf("Unsnap complete\n");
                } else {
                    printf("KV archive not enabled (recompile with -DKV_ARCHIVE)\n");
                }
                continue;
            }
#endif /* KV_ARCHIVE */

            /* ── KV Page Store commands ── */
            if(!strncmp(line,"/pevict ",8)){
                if (kv_page.enabled) {
                    int _n=atoi(line+8);
                    if(_n>0){kv_page_evict_oldest(&kv_page, _n);}
                    else{printf("Usage: /pevict N (evict oldest N pages)\n");}
                }else{printf("KV Page Store not enabled (--kv-page)\n");}
                continue;
            }
            if(!strncmp(line,"/prestore ",10)){
                if (kv_page.enabled) {
                    int _pid=atoi(line+10);
                    if(_pid>=0 && _pid<kv_page.n_pages){kv_page_restore(&kv_page, _pid);}
                    else if(!strcmp(line+10,"all")){kv_page_restore_all(&kv_page);}
                    else{printf("Usage: /prestore PAGE_ID or /prestore all\n");}
                }else{printf("KV Page Store not enabled (--kv-page)\n");}
                continue;
            }
            if(!strcmp(line,"/pstatus")){
                if (kv_page.enabled) {
                    kv_page_print_status(&kv_page);
                }else{printf("KV Page Store not enabled (--kv-page)\n");}
                continue;
            }
            if(!strcmp(line,"/rstatus")){
                if(g_opt_remap){
                    kv_remap_print_status(&g_remap_ctx);
                    kv_remap_rail_print_status(&g_remap_rail);
                }else{printf("KV Remap not enabled (--remap)\n");}
                continue;
            }
            if(!strcmp(line,"/rscan")){
                if(g_opt_remap){
                    kv_remap_rail_start_scan(&g_remap_rail);
                    printf("Full rescan started\n");
                }else{printf("KV Remap not enabled (--remap)\n");}
                continue;
            }
            if(!strcmp(line,"/rrestore")){
                if(g_opt_remap){
                    int r=kv_remap_restore(&g_remap_ctx);
                    printf("Restore %s\n",r==0?"OK":"failed");
                }else{printf("KV Remap not enabled (--remap)\n");}
                continue;
            }
            if(!strncmp(line,"/psnap ",7)){
                if (kv_page.enabled) {
                    int _pid=atoi(line+7);
                    if(_pid>=0 && _pid<kv_page.n_pages){kv_page_snapshot(&kv_page, _pid);}
                    else if(!strcmp(line+7,"all")){kv_page_snapshot_all(&kv_page);}
                    else{printf("Usage: /psnap PAGE_ID or /psnap all\n");}
                }else{printf("KV Page Store not enabled (--kv-page)\n");}
                continue;
            }

            if(strncmp(line,"/checkpoint ",12)==0){int _cp=sid_checkpoint(&tt.ring,line+12);fprintf(stderr,_cp>=0?"[chat] checkpoint '%s' #%d at ring[%u]\n":"[chat] checkpoint failed\n",line+12,_cp,_cp>=0?(unsigned)tt.ring.checkpoints[_cp].ring_index:0);continue;}
            if(strncmp(line,"/rewind ",8)==0){int _cp=sid_find_checkpoint(&tt.ring,line+8);if(_cp>=0){int _n=sid_rewind_to_checkpoint(&tt.ring,(uint32_t)_cp);fprintf(stderr,"[chat] rewind to '%s': %d entries undone\n",line+8,_n);}else{fprintf(stderr,"[chat] checkpoint '%s' not found\n",line+8);}continue;}
            if(strncmp(line,"/ff ",4)==0){int _cp=sid_find_checkpoint(&tt.ring,line+4);if(_cp>=0){int _n=sid_ffwd_from_checkpoint(&tt.ring,(uint32_t)_cp);fprintf(stderr,"[chat] fast-forward to '%s': %d entries reapplied\n",line+4,_n);}else{fprintf(stderr,"[chat] checkpoint '%s' not found\n",line+4);}continue;}
            if(strncmp(line,"/branch ",8)==0){int _cp=sid_checkpoint(&tt.ring,line+8);fprintf(stderr,_cp>=0?"[chat] branch checkpoint '%s' #%d at ring[%u]\n":"[chat] branch checkpoint failed\n",line+8,_cp,_cp>=0?(unsigned)tt.ring.checkpoints[_cp].ring_index:0);continue;}
            if(!strcmp(line,"/tt")){
                printf("\n=== SID State ===\n");
                printf("  ring: %u entries (head=%u tail=%u)\n", (unsigned)tt.ring.count, (unsigned)tt.ring.head, (unsigned)tt.ring.tail);
                printf("  pushed=%llu rewound=%llu ffwd=%llu\n",
                    (unsigned long long)tt.ring.total_pushed,
                    (unsigned long long)tt.ring.total_rewound,
                    (unsigned long long)tt.ring.total_ffwd);
                printf("  checkpoints: %u\n", tt.ring.n_checkpoints);
                for (uint32_t _i = 0; _i < tt.ring.n_checkpoints; _i++) {
                    const SidCheckpoint *_cp = &tt.ring.checkpoints[_i];
                    printf("    cp[%u] '%s' ring[%u] ts=%u\n",
                        _cp->id, _cp->name, _cp->ring_index, _cp->timestamp);
                }
                printf("  pending: checkpoint=%d rewind=%d ffwd=%d branch=%d\n",
                    tt.pending_checkpoint, tt.pending_rewind,
                    tt.pending_ffwd, tt.pending_branch_cp);
                printf("=== End State ===\n");
                fflush(stdout);
                continue;
            }
            roles[cc]="user";content[cc]=strdup(line);cc++;
            /* Build the current prompt and only append the new suffix when possible */
            llama_chat_message *msgs = (llama_chat_message*)malloc((size_t)cc * sizeof(llama_chat_message));
            for (int _j = 0; _j < cc; _j++) { msgs[_j].role = roles[_j]; msgs[_j].content = content[_j]; }
            const char *chat_tmpl = llama_model_chat_template(model, NULL);
            if (!chat_tmpl) chat_tmpl = "";
            int flen = llama_chat_apply_template(chat_tmpl, msgs, (size_t)cc, true, NULL, 0);
            char *fmt = (char*)malloc((size_t)flen + 1);
            llama_chat_apply_template(chat_tmpl, msgs, (size_t)cc, true, fmt, flen + 1);
            free(msgs);
            int na=llama_tokenize(v,fmt,flen,NULL,0,true,false);
            int nt=na<0?-na:na;
            if(nt<=0||nt>2048-64){free(fmt);continue;}
            int*ta=(int*)malloc((size_t)nt*4);
            llama_tokenize(v,fmt,flen,ta,nt,true,false);
            free(fmt);
            size_t prefix = pending_ready ? tok_prefix_len(pending_toks, pending_n, ta, (size_t)nt) : 0;
            int full_reset = (!pending_ready || prefix != pending_n);
            if (full_reset) {
                llama_memory_seq_rm(llama_get_memory(lctx), 0, -1, -1);
                prefix = 0;
            }
            size_t delta_n = (size_t)nt - prefix;
            if (delta_n > 0) {
                struct llama_batch pb=llama_batch_init((int)delta_n,0,1);pb.n_tokens=(int)delta_n;
                for(size_t j=0;j<delta_n;j++){pb.token[j]=ta[prefix+j];pb.pos[j]=(int32_t)(prefix+j);pb.n_seq_id[j]=1;pb.seq_id[j][0]=0;pb.logits[j]=j==delta_n-1?1:0;}
                sid_swap_apply();
                if(llama_decode(lctx,pb)!=0){llama_batch_free(pb);free(ta);sid_swap_restore();free(pending_toks);return 1;}
                sid_swap_restore(); twin_gpu_gear_push();
                /* KV Remap: capture skeleton after first prompt decode (rail starts later) */
                if (g_opt_remap && !g_remap_ctx.skeleton_valid) {
                    kv_remap_set_skeleton(&g_remap_ctx);
                }
#ifdef KV_ARCHIVE
                if (kv_sid.enabled) kv_snapshot_all(&kv_sid);
#endif
                sid_mem_store_log(lctx, nv, (uint16_t)(nt - 1), sid_face);
                llama_batch_free(pb);
            }
            if (tokbuf_copy(&pending_toks, &pending_n, &pending_cap, ta, (size_t)nt) != 0) {
                free(ta);
                free(pending_toks);
                fprintf(stderr, "ERROR: pending token buffer alloc failed\n");
                return 1;
            }
            pending_ready = 1;
            if (opt_capture) {
                #ifdef _WIN32
                _mkdir(opt_capture);
                #else
                mkdir(opt_capture, 0755);
                #endif
                CaptureTensor ctens[MAX_TENSORS];
                for (int ci = 0; ci < n_found; ci++) {
                    ctens[ci].name   = found_tensors[ci].name;
                    ctens[ci].data   = found_tensors[ci].orig_data;
                    ctens[ci].nbytes = found_tensors[ci].nbytes;
                    ctens[ci].dtype  = (int)found_tensors[ci].dtype;
                }
                CaptureResult cr;
                capture_run_full(&cr, opt_capture, ctens, n_found, 12);
            }
            Sampler gs=sp;gs.count=0;int32_t pos=nt;
            struct llama_batch gb=llama_batch_init(1,0,1);
            gb.n_tokens=1;gb.n_seq_id[0]=1;gb.seq_id[0][0]=0;gb.logits[0]=1;
            size_t resp_cap=256,resp_len=0;char*resp_buf=(char*)malloc(resp_cap);resp_buf[0]='\0';
            for(int i=0;i<opt_max_new;i++){
                sid_swap_apply();
                int tok=sample_token(llama_get_logits_ith(lctx,-1),nv,&gs);
                sid_swap_restore(); twin_gpu_gear_push();
                if(llama_vocab_is_eog(v,tok))break;
                char b[16];int l=llama_token_to_piece(v,tok,b,16,0,false);
                if(l>0){
                    b[l>15?15:l]=0;printf("%s",b);fflush(stdout);
                    size_t need=resp_len+(size_t)l+1;
                    if(need>resp_cap){resp_cap=need+256;resp_buf=(char*)realloc(resp_buf,resp_cap);}
                    memcpy(resp_buf+resp_len,b,(size_t)l);resp_len+=(size_t)l;resp_buf[resp_len]='\0';
                }
                gb.token[0]=tok;gb.pos[0]=pos++;
                sid_swap_apply();
                if(llama_decode(lctx,gb)!=0){sid_swap_restore();break;}
                sid_swap_restore(); twin_gpu_gear_push();
                sid_mem_store_log(lctx, nv, (uint16_t)(pos - 1), sid_face);
                if (tokbuf_append(&pending_toks, &pending_n, &pending_cap, &tok, 1) != 0) {
                    fprintf(stderr, "ERROR: pending token buffer append failed\n");
                    free(resp_buf);
                    llama_batch_free(gb);
                    free(ta);
                    free(pending_toks);
                    return 1;
                }
            }
            llama_batch_free(gb);printf("\n");free(ta);
            /* KV Remap: scan (if needed) + cycle after each chat turn */
            if (g_opt_remap && g_remap_ctx.skeleton_valid) {
                kv_remap_rail_start_scan(&g_remap_rail);
                while (g_remap_rail.state != RAIL_PARK)
                    kv_remap_rail_step(&g_remap_rail);
                kv_remap_cycle(&g_remap_ctx);
            }
            /* Save assistant response so next turn has user1/assistant/user2 */
            if(resp_len>0){roles[cc]="assistant";content[cc]=resp_buf;cc++;}else{free(resp_buf);}
        }
        if (chat_in != stdin) fclose(chat_in);
        for(int i=0;i<cc;i++)free(content[i]);
        free(pending_toks);
    }

    if(opt_prompt){
        const char *p_tmpl = llama_model_chat_template(model, NULL);
        if (!p_tmpl) p_tmpl = "";
        llama_chat_message p_msg[1] = {{"user", opt_prompt}};
        int p_flen = llama_chat_apply_template(p_tmpl, p_msg, 1, true, NULL, 0);
        if (p_flen < 0) { fprintf(stderr, "template error\n"); return 1; }
        char *p_fmt = (char*)malloc((size_t)p_flen + 1);
        llama_chat_apply_template(p_tmpl, p_msg, 1, true, p_fmt, p_flen + 1);
        int ntokens=llama_tokenize(v,p_fmt,(int)strlen(p_fmt),NULL,0,true,false);
        int nt=ntokens<0?-ntokens:ntokens;
        if(nt<=0){fprintf(stderr,"tokenize fail\n"); free(p_fmt); return 1;}
        int*toks=(int*)malloc((size_t)nt*4);
        llama_tokenize(v,p_fmt,(int)strlen(p_fmt),toks,nt,true,false);
        free(p_fmt);
        struct llama_batch pb=llama_batch_init(nt,0,1);pb.n_tokens=nt;
        for(int j=0;j<nt;j++){pb.token[j]=toks[j];pb.pos[j]=j;pb.n_seq_id[j]=1;pb.seq_id[j][0]=0;pb.logits[j]=j==nt-1?1:0;}
        sid_swap_apply();
        if(llama_decode(lctx,pb)!=0){fprintf(stderr,"[dbg] decode fail\n");llama_batch_free(pb);free(toks);sid_swap_restore();return 1;}
        sid_swap_restore(); twin_gpu_gear_push();
        /* KV Remap: capture skeleton after first prompt decode (rail starts later) */
        if (g_opt_remap && !g_remap_ctx.skeleton_valid) {
            kv_remap_set_skeleton(&g_remap_ctx);
        }
        sid_mem_store_log(lctx, nv, (uint16_t)(nt - 1), sid_face);
        /* KV swap: snapshot prompt KV state and inject perturbation */
        if (g_kv_swap.n_perturb) {
            kv_swap_snapshot(&g_kv_swap, lctx);
            kv_swap_inject_perturbed(&g_kv_swap, lctx);
        }
        if (opt_capture) {
            #ifdef _WIN32
            _mkdir(opt_capture);
            #else
            mkdir(opt_capture, 0755);
            #endif
            CaptureTensor ctens[MAX_TENSORS];
            for (int ci2 = 0; ci2 < n_found; ci2++) {
                ctens[ci2].name   = found_tensors[ci2].name;
                ctens[ci2].data   = found_tensors[ci2].orig_data;
                ctens[ci2].nbytes = found_tensors[ci2].nbytes;
                ctens[ci2].dtype  = (int)found_tensors[ci2].dtype;
            }
            CaptureResult cr;
            capture_run_full(&cr, opt_capture, ctens, n_found, 12);
        }
        llama_batch_free(pb);

        Sampler gs=sp;gs.count=0;int32_t pos=nt;
        struct llama_batch gb=llama_batch_init(1,0,1);
        gb.n_tokens=1;gb.n_seq_id[0]=1;gb.seq_id[0][0]=0;gb.logits[0]=1;
        for(int i=0;i<opt_max_new;i++){
            sid_swap_apply();
            int tok=sample_token(llama_get_logits_ith(lctx,-1),nv,&gs);
            sid_swap_restore(); twin_gpu_gear_push();
            if(llama_vocab_is_eog(v,tok))break;
            char b[16];int l=llama_token_to_piece(v,tok,b,16,0,false);
            if(l>0){b[l>15?15:l]=0;printf("%s",b);fflush(stdout);}
            gb.token[0]=tok;gb.pos[0]=pos++;
            sid_swap_apply();
            if(llama_decode(lctx,gb)!=0){sid_swap_restore();break;}
            sid_swap_restore(); twin_gpu_gear_push();
            sid_mem_store_log(lctx, nv, (uint16_t)(pos - 1), sid_face);
        }
        llama_batch_free(gb);printf("\n");free(toks);
        /* KV Remap: scan + cycle after prompt generation */
        if (g_opt_remap && g_remap_ctx.skeleton_valid) {
            kv_remap_rail_start_scan(&g_remap_rail);
            while (g_remap_rail.state != RAIL_PARK)
                kv_remap_rail_step(&g_remap_rail);
            kv_remap_cycle(&g_remap_ctx);
        }
    }

    /* ── Tensor memory store save ── */
    /* ── Cleanup KV swap ── */
    kv_swap_free(&g_kv_swap);

#ifdef KV_ARCHIVE
    /* ── Cleanup KV SID evict ── */
    kv_sid_free(&kv_sid);
#endif

    /* ── Cleanup KV Page Store ── */
    kv_page_destroy(&kv_page);

    /* ── Cleanup KV Remap ── */
    if (g_opt_remap) {
        kv_remap_rail_destroy(&g_remap_rail);
        kv_remap_destroy(&g_remap_ctx);
    }

    if (g_opt_mem_store && g_tmem_buf) {
        tmem_save(&g_tmem_store, g_opt_mem_store);
        fprintf(stderr, "[mem-store] saved %u records to %s\n",
            g_tmem_store.n_records, g_opt_mem_store);
        free(g_tmem_buf);
        g_tmem_buf = NULL;
    }

    /* ── Icosa bridge stats & cleanup ── */
    if (g_opt_twin_gpu && g_ibridge_ctx) {
        g_ibridge.destroy(g_ibridge_ctx);
        g_ibridge_ctx = NULL;
        icosa_bridge_unload(&g_ibridge);
        fprintf(stderr, "[twin-gpu] bridge unloaded\n");
    }

    /* ── DRamTile cleanup ── */
    if (g_opt_dramtile) dt_store_destroy(&g_dramtile);

    /* ── Cleanup ── */
    for (int i = 0; i < n_sid_swaps; i++) {
        if (sid_swaps[i].is_malloc) free(sid_swaps[i].sid_data);
    }
cleanup:
    if (g_opt_twin_gpu && g_ibridge_ctx) {
        g_ibridge.destroy(g_ibridge_ctx);
        g_ibridge_ctx = NULL;
        icosa_bridge_unload(&g_ibridge);
    }
    sid_loader_close(&slc);
    gguf_idx_close(&gidx);
    llama_free(lctx);llama_model_free(model);llama_backend_free();
    return 0;
}
