/*
 * llama_pogls_runner_sid.c — SID-coordinate inference runner
 * Build: gcc -O2 -I. -II:/llama.cpp/include -II:/llama.cpp/ggml/include -o $@ $< I:/llama/llama-b9528-bin-win-vulkan-x64/llama.dll I:/llama/llama-b9528-bin-win-vulkan-x64/ggml.dll I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-base.dll I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-x64.dll -lm
 * Usage: llama_pogls_runner_sid.exe model.gguf [options]
 *
 * ARCHITECTURE:
 *   ┌─────────────────────────────────────────────────────────┐
 *   │  llama_model_load_from_file()       ← WORKS (mmap)     │
 *   │    → mmap → weights → scheduler → compute → logits     │
 *   │                                                         │
 *   │  sid_loader_open() + sid_cache_create()  ← NEW         │
 *   │    → gguf_idx_open() → parse tensor list               │
 *   │    → for each Q8_0 weight: read + cache                │
 *   │    → verify hash(cached) == hash(mmap_direct)          │
 *   │                                                         │
 *   │  TODO: tensor->data swap                               │
 *   │    → for each (gguf_tensor_name → model_tensor_ptr):   │
 *   │        model_tensor->data = sid_cache_get(name)        │
 *   └─────────────────────────────────────────────────────────┘
 *
 * HISTORY:
 *   Jun 14: init_from_user FAILS for Q8_0 models. llama_model_init_from_user()
 *           creates synthetic .scale/.input_scale tensors not in GGUF; callback
 *           data is ignored by internal load_tensors path because files.empty()
 *           skips proper weight metadata linking (llama-model-loader.cpp:1213).
 *           → Use llama_model_load_from_file (works) for all SID integration.
 *
 *   Jun 15: Confirmed root cause: create_tensor with files.empty() (line 1213)
 *           builds fake t_meta without real weight info (weights_map empty).
 *           Backend buffers allocated but computation graph reads from
 *           different memory. memset-fill test (0x42) produced NaN → proof.
 *           → Switch to load_from_file permanently. SID cache sits at
 *             tensor->data swap level, not model-init.
 *
 *           sid_cache + sid_loader integration: pre-load all Q8_0 weights
 *           into TWFaceRewindSid 1440-slot cache. Verify integrity via hash
 *           comparison with direct mmap reads. Ready for tensor->data swap
 *           once model tensor pointers are accessible.
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

#define MAX_TOKENS_CACHE 4096
#define MAX_CHAT_HISTORY 128
#define MAX_LINE 4096

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

int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"Usage: %s model.gguf [--chat] [--ngl N] [options]\n",argv[0]);return 1;}
    const char*gguf_path=NULL,*opt_prompt=NULL;
    int opt_ngl=0,opt_max_new=256,opt_chat=0,opt_count_only=0;
    Sampler sp={.temp=.7f,.top_p=.9f,.repeat_penalty=1.1f,.top_k=40,.repeat_last_n=64};
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--ngl")&&i+1<argc)opt_ngl=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--temp")&&i+1<argc)sp.temp=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--top-p")&&i+1<argc)sp.top_p=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--top-k")&&i+1<argc)sp.top_k=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--repeat-penalty")&&i+1<argc)sp.repeat_penalty=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--max-new")&&i+1<argc)opt_max_new=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--prompt")&&i+1<argc)opt_prompt=argv[++i];
        else if(!strcmp(argv[i],"--chat"))opt_chat=1;
        else if(!strcmp(argv[i],"--count-only"))opt_count_only=1;
        else if(!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")){fprintf(stderr,"Usage: ...\n");return 0;}
        else if(!gguf_path)gguf_path=argv[i];
    }
    if(!gguf_path){fprintf(stderr,"ERROR: missing model.gguf\n");return 1;}

    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");

    fprintf(stderr, "\n--- load_from_file (working path) ---\n");

    struct llama_model_params mp=llama_model_default_params();
    mp.n_gpu_layers=opt_ngl;
    PoglsTime t0,t1;clock_gettime(CLOCK_MONOTONIC,&t0);
    struct llama_model*model=llama_model_load_from_file(gguf_path,mp);
    clock_gettime(CLOCK_MONOTONIC,&t1);
    if(!model){fprintf(stderr,"ERROR: model load\n");llama_backend_free();return 1;}
    double lms=(t1.tv_sec-t0.tv_sec)*1000.0+(t1.tv_nsec-t0.tv_nsec)/1e6;
    fprintf(stderr,"[load] %.0f ms\n",lms);

    // ── if --count-only, skip directly to tensor count ──
    if (opt_count_only) {
        HMODULE hHelper = LoadLibraryA("sid_tensor_helper.dll");
        if (hHelper) {
            typedef int (*c_fn)(void*);
            c_fn cnt  = (c_fn)GetProcAddress(hHelper, "sid_tensor_count");
            c_fn cnta = (c_fn)GetProcAddress(hHelper, "sid_tensor_count_all");
            if (cnt) {
                int d = cnt(model);
                int a = cnta ? cnta(model) : -1;
                fprintf(stderr, "[tensor] direct=%d all=%d\n", d, a);
            }
            FreeLibrary(hHelper);
        }
        llama_model_free(model); llama_backend_free(); return 0;
    }

    fprintf(stderr, "\n--- SID cache init ---\n");

    // compute total Q8_0 weight data size → allocate cache pool
    GGUFTensorIndex gidx;
    if (gguf_idx_open(gguf_path, &gidx) != 0) {
        fprintf(stderr, "ERROR: gguf_idx_open failed\n");
        llama_model_free(model); llama_backend_free(); return 1;
    }
    uint64_t q80_total = 0, max_sz = 0;
    int total_q80 = 0;
    for (uint64_t i = 0; i < gidx.n_tensors; i++) {
        if (gidx.dtypes[i] != GGUF_Q8_0) continue;
        total_q80++;
        q80_total += gidx.sizes[i];
        if (gidx.sizes[i] > max_sz) max_sz = gidx.sizes[i];
    }
    fprintf(stderr, "[sid] Q8_0 tensors: %d, total data: %llu bytes, max tensor: %llu\n",
        total_q80, (unsigned long long)q80_total, (unsigned long long)max_sz);

    SIDCache sid_cache;
    sid_cache_init(&sid_cache, q80_total + (1u << 20)); // pool = all Q8_0 + 1 MB

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
        if (gidx.dtypes[i] != GGUF_Q8_0) continue;
        const char *name = gidx.names[i];
        size_t sz = (size_t)gidx.sizes[i];
        uint64_t off = gidx.offsets[i];

        // read directly from file for verification baseline
        FILE *f = fopen(gguf_path, "rb");
        if (!f) { fprintf(stderr, "  fopen fail\n"); continue; }
        uint64_t abs_off = gguf_idx_tensor_abs_offset(&gidx, i);
        fseek(f, (long)abs_off, SEEK_SET);
        size_t r = fread(verify_buf, 1, sz, f);
        fclose(f);
        if (r != sz) { fprintf(stderr, "  fread fail at idx %llu\n", (unsigned long long)i); continue; }
        uint64_t mmap_hash = xor_hash(verify_buf, sz);

        // cache via sid_loader_load
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

    fprintf(stderr, "[sid] cached+verified: %d/%d\n", cached_ok, total_q80);
    fprintf(stderr, "[sid] file_hits=%llu cache_hits=%llu bytes_read=%llu\n",
        (unsigned long long)slc.file_hits, (unsigned long long)slc.cache_hits,
        (unsigned long long)slc.bytes_read);

    // Second pass: read from cache only (should be all hits now)
    fprintf(stderr, "[sid] cache-only verification...\n");
    int cache_only_ok = 0;
    for (uint64_t i = 0; i < gidx.n_tensors; i++) {
        if (gidx.dtypes[i] != GGUF_Q8_0) continue;
        const char *name = gidx.names[i];
        size_t sz = (size_t)gidx.sizes[i];
        uint8_t *cached_data = NULL; size_t cached_sz = 0;
        if (sid_loader_load(&slc, name, read_buf, &cached_data, &cached_sz) == 0 && cached_sz == sz) {
            cache_only_ok++;
        }
    }
    fprintf(stderr, "[sid] cache-only hits: %d/%d (expected 0 if first pass evicted everything)\n",
        cache_only_ok, total_q80);
    fprintf(stderr, "[sid] file_hits=%llu cache_hits=%llu (second pass)\n",
        (unsigned long long)slc.file_hits, (unsigned long long)slc.cache_hits);

    free(verify_buf);
    free(read_buf);

    // ── tensor→data swap: load helper DLL ──
    typedef int (*sid_fn_find)(void*, const char*, void**, size_t*);
    typedef int (*sid_fn_swap)(void*, const char*, void*, void**);
    typedef int (*sid_fn_count)(void*);
    typedef int (*sid_fn_enum)(void*, char(*)[64], void**, size_t*, int);
    typedef int (*sid_fn_restore)(void*, const char**, void**, int);

    HMODULE hHelper = LoadLibraryA("sid_tensor_helper.dll");
    if (!hHelper) { fprintf(stderr, "ERROR: can't load sid_tensor_helper.dll\n"); sid_loader_close(&slc); gguf_idx_close(&gidx); llama_model_free(model); llama_backend_free(); return 1; }
    sid_fn_find      sid_tensor_find      = (sid_fn_find)     GetProcAddress(hHelper, "sid_tensor_find");
    sid_fn_swap      sid_tensor_swap      = (sid_fn_swap)     GetProcAddress(hHelper, "sid_tensor_swap");
    sid_fn_count     sid_tensor_count     = (sid_fn_count)    GetProcAddress(hHelper, "sid_tensor_count");
    sid_fn_enum      sid_tensor_enum      = (sid_fn_enum)     GetProcAddress(hHelper, "sid_tensor_enum");
    sid_fn_restore   sid_tensor_restore   = (sid_fn_restore)  GetProcAddress(hHelper, "sid_tensor_restore_all");
    typedef int (*sid_fn_find_all)(void*, const char*, void**, size_t*);
    typedef int (*sid_fn_enum_all)(void*, char(*)[64], void**, size_t*, int);
    typedef int (*sid_fn_count_all)(void*);
    sid_fn_find_all  sid_tensor_find_all  = (sid_fn_find_all) GetProcAddress(hHelper, "sid_tensor_find_all");
    sid_fn_enum_all  sid_tensor_enum_all  = (sid_fn_enum_all) GetProcAddress(hHelper, "sid_tensor_enum_all");
    sid_fn_count_all sid_tensor_count_all = (sid_fn_count_all)GetProcAddress(hHelper, "sid_tensor_count_all");
    typedef int (*sid_fn_debug)(void*);
    sid_fn_debug     sid_tensor_debug     = (sid_fn_debug)    GetProcAddress(hHelper, "sid_tensor_debug");
    if (!sid_tensor_find || !sid_tensor_swap) { fprintf(stderr, "ERROR: missing helper symbols\n"); FreeLibrary(hHelper); sid_loader_close(&slc); gguf_idx_close(&gidx); llama_model_free(model); llama_backend_free(); return 1; }

    int n_model_tensors = sid_tensor_count(model);
    int n_all_tensors = sid_tensor_count_all ? sid_tensor_count_all(model) : -1;
    fprintf(stderr, "[tensor] direct=%d all=%d\n", n_model_tensors, n_all_tensors);

    // find output.weight offset in GGUF index → know which tensor name to swap
    int output_idx = -1;
    for (uint64_t i = 0; i < gidx.n_tensors; i++) {
        if (strcmp(gidx.names[i], "output.weight") == 0) { output_idx = (int)i; break; }
    }
    fprintf(stderr, "[swap] output.weight idx=%d size=%llu\n", output_idx,
        output_idx >= 0 ? (unsigned long long)gidx.sizes[output_idx] : 0ULL);

    // ── Phase 1: baseline inference (original mmap data) ──
    // Then swap output.weight → SID cache and verify logits match
    struct llama_context_params cp=llama_context_default_params();
    cp.n_ctx=256;cp.n_threads=4;cp.n_threads_batch=4;cp.n_batch=128;cp.n_ubatch=64;
    struct llama_context *lctx = NULL;

    // pick a fixed prompt for comparison
    const char *test_prompt = opt_prompt ? opt_prompt : "hi";

    // ======== BASELINE ========
    fprintf(stderr, "\n--- [BASELINE] original mmap data ---\n");
    lctx = llama_init_from_model(model, cp);
    if(!lctx){fprintf(stderr,"ERROR: context\n"); return 1;}
    const struct llama_vocab *v = llama_model_get_vocab(model);
    int nv = llama_vocab_n_tokens(v);
    int eos = llama_vocab_eos(v); if(eos==-1) eos=151645;

    int nt_b = llama_tokenize(v, test_prompt, strlen(test_prompt), NULL, 0, true, false);
    int nt_b_abs = nt_b < 0 ? -nt_b : nt_b;
    int *toks_b = (int*)malloc((size_t)nt_b_abs * 4);
    llama_tokenize(v, test_prompt, strlen(test_prompt), toks_b, nt_b_abs, true, false);

    struct llama_batch pb_b = llama_batch_init(nt_b_abs, 0, 1); pb_b.n_tokens = nt_b_abs;
    for(int j=0;j<nt_b_abs;j++){pb_b.token[j]=toks_b[j];pb_b.pos[j]=j;pb_b.n_seq_id[j]=1;pb_b.seq_id[j][0]=0;pb_b.logits[j]=j==nt_b_abs-1?1:0;}
    if(llama_decode(lctx, pb_b)!=0){fprintf(stderr,"[baseline] decode fail\n"); llama_batch_free(pb_b); free(toks_b); return 1;}
    const float *logits_b = llama_get_logits_ith(lctx, -1);
    // save first 10 + last 5 logits for comparison
    #define SAVE_LOGITS 15
    float baseline_logits[SAVE_LOGITS];
    memcpy(baseline_logits, logits_b, SAVE_LOGITS * 4);
    fprintf(stderr,"[baseline] logits[0..4]=%a %a %a %a %a\n", logits_b[0],logits_b[1],logits_b[2],logits_b[3],logits_b[4]);
    fprintf(stderr,"[baseline] logits[%d..%d]=%a %a %a %a %a\n", nv-5,nv-1, logits_b[nv-5],logits_b[nv-4],logits_b[nv-3],logits_b[nv-2],logits_b[nv-1]);

    // ======== SWAP output.weight → SID cache ========
    fprintf(stderr, "\n--- [SWAP] output.weight → SID cache ---\n");
    void *old_output_data = NULL;
    uint8_t *cached_output = NULL; size_t cached_output_sz = 0;
    int found = sid_cache_get(&sid_cache, "output.weight", &cached_output, &cached_output_sz);
    if (found != 0) { fprintf(stderr, "[swap] ERROR: output.weight not in cache!\n"); return 1; }
    fprintf(stderr, "[swap] SID cache has output.weight at %p (%llu bytes)\n",
        cached_output, (unsigned long long)cached_output_sz);

    if (sid_tensor_swap(model, "output.weight", cached_output, &old_output_data) != 0) {
        fprintf(stderr, "[swap] ERROR: sid_tensor_swap failed\n"); return 1;
    }
    fprintf(stderr, "[swap] output.weight data: %p → %p (cache)\n", old_output_data, cached_output);

    // verify the swap: re-find and check
    void *verify_data = NULL; size_t verify_sz = 0;
    if (sid_tensor_find(model, "output.weight", &verify_data, &verify_sz) != 0) {
        fprintf(stderr, "[swap] ERROR: can't find output.weight after swap\n"); return 1;
    }
    fprintf(stderr, "[swap] verify: output.weight data = %p (expected %p) size=%llu\n",
        verify_data, cached_output, (unsigned long long)verify_sz);

    // byte comparison: mmap data vs SID cache data
    fprintf(stderr, "\n--- [COMPARE] mmap vs SID cache bytes ---\n");
    if (old_output_data && cached_output) {
        uint64_t mmap_hash = xor_hash((const uint8_t*)old_output_data, (size_t)verify_sz);
        uint64_t cache_hash = xor_hash((const uint8_t*)cached_output, (size_t)verify_sz);
        int byte_match = 1;
        for (size_t ci = 0; ci < (size_t)verify_sz && ci < 64; ci++) {
            if (((const uint8_t*)old_output_data)[ci] != cached_output[ci]) { byte_match = 0; break; }
        }
        fprintf(stderr, "[cmp] first 64 bytes: %s\n", byte_match ? "MATCH" : "MISMATCH");
        fprintf(stderr, "[cmp] mmap_hash=%016llx  cache_hash=%016llx  nbytes=%llu\n",
            (unsigned long long)mmap_hash, (unsigned long long)cache_hash,
            (unsigned long long)verify_sz);
    }

    // ======== TEST with swapped data ========
    fprintf(stderr, "\n--- [TEST] SID cache data ---\n");
    llama_free(lctx); // discard old context (KV cache + old decode state)
    lctx = llama_init_from_model(model, cp);
    if(!lctx){fprintf(stderr,"ERROR: context (test)\n"); return 1;}

    int nt_t = llama_tokenize(v, test_prompt, strlen(test_prompt), NULL, 0, true, false);
    int nt_t_abs = nt_t < 0 ? -nt_t : nt_t;
    int *toks_t = (int*)malloc((size_t)nt_t_abs * 4);
    llama_tokenize(v, test_prompt, strlen(test_prompt), toks_t, nt_t_abs, true, false);

    struct llama_batch pb_t = llama_batch_init(nt_t_abs, 0, 1); pb_t.n_tokens = nt_t_abs;
    for(int j=0;j<nt_t_abs;j++){pb_t.token[j]=toks_t[j];pb_t.pos[j]=j;pb_t.n_seq_id[j]=1;pb_t.seq_id[j][0]=0;pb_t.logits[j]=j==nt_t_abs-1?1:0;}
    if(llama_decode(lctx, pb_t)!=0){fprintf(stderr,"[test] decode fail\n"); llama_batch_free(pb_t); free(toks_t); return 1;}
    const float *logits_t = llama_get_logits_ith(lctx, -1);
    fprintf(stderr,"[test]    logits[0..4]=%a %a %a %a %a\n", logits_t[0],logits_t[1],logits_t[2],logits_t[3],logits_t[4]);
    fprintf(stderr,"[test]    logits[%d..%d]=%a %a %a %a %a\n", nv-5,nv-1, logits_t[nv-5],logits_t[nv-4],logits_t[nv-3],logits_t[nv-2],logits_t[nv-1]);

    // ======== COMPARE ========
    int match = 1;
    for (int i = 0; i < SAVE_LOGITS; i++) {
        if (baseline_logits[i] != logits_t[i]) {
            // compare as hex float bits
            uint32_t b_bits, t_bits;
            memcpy(&b_bits, &baseline_logits[i], 4);
            memcpy(&t_bits, &logits_t[i], 4);
            if (b_bits != t_bits) {
                fprintf(stderr, "[compare] MISMATCH at logits[%d]: %a (%08x) vs %a (%08x)\n",
                    i, (double)baseline_logits[i], b_bits, (double)logits_t[i], t_bits);
                match = 0; break;
            }
        }
    }
    // also compare full nv range via bit-exact hash
    uint64_t hash_b = xor_hash((const uint8_t*)logits_b, (size_t)nv * 4);
    uint64_t hash_t = xor_hash((const uint8_t*)logits_t, (size_t)nv * 4);
    if (hash_b != hash_t) match = 0;
    fprintf(stderr, "\n=== [SWAP VERDICT] output.weight tensor→data swap: %s ===\n",
        match ? "✅ PASS (logits match)" : "❌ FAIL (logits differ)");
    fprintf(stderr, "    baseline hash=%016llx  test hash=%016llx\n",
        (unsigned long long)hash_b, (unsigned long long)hash_t);

    // restore original pointer so normal inference is unaffected
    void *unused = NULL;
    sid_tensor_swap(model, "output.weight", old_output_data, &unused);

    llama_batch_free(pb_b); free(toks_b);
    llama_batch_free(pb_t); free(toks_t);

    // ── Continue with normal chat/prompt inference ──
    // recreate context for the actual session
    llama_free(lctx);
    lctx = llama_init_from_model(model, cp);
    if(!lctx){fprintf(stderr,"ERROR: context\n");return 1;}
    v = llama_model_get_vocab(model);
    nv = llama_vocab_n_tokens(v);
    eos = llama_vocab_eos(v); if(eos==-1)eos=151645;

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
            if(llama_decode(lctx,pb)!=0){llama_batch_free(pb);free(ta);break;}
            llama_batch_free(pb);
            Sampler gs=sp;gs.count=0;int32_t pos=nt;
            struct llama_batch gb=llama_batch_init(1,0,1);
            gb.n_tokens=1;gb.n_seq_id[0]=1;gb.seq_id[0][0]=0;gb.logits[0]=1;
            for(int i=0;i<opt_max_new;i++){
                int tok=sample_token(llama_get_logits_ith(lctx,-1),nv,&gs);
                if(tok==eos||tok==0)break;
                char b[16];int l=llama_token_to_piece(v,tok,b,16,0,false);
                if(l>0){b[l>15?15:l]=0;printf("%s",b);fflush(stdout);}
                gb.token[0]=tok;gb.pos[0]=pos++;if(llama_decode(lctx,gb)!=0)break;
            }
            llama_batch_free(gb);printf("\n");free(ta);
        }
        for(int i=0;i<cc;i++)free(content[i]);
    }

    if(opt_prompt){
        int ntokens=llama_tokenize(v,opt_prompt,strlen(opt_prompt),NULL,0,true,false);
        fprintf(stderr,"[dbg] ntokens=%d\n",ntokens);
        int nt=ntokens<0?-ntokens:ntokens;
        if(nt<=0){fprintf(stderr,"tokenize fail\n");return 1;}
        int*toks=(int*)malloc((size_t)nt*4);
        llama_tokenize(v,opt_prompt,strlen(opt_prompt),toks,nt,true,false);
        fprintf(stderr,"[dbg] nt=%d first_toks:",nt);
        for(int j=0;j<nt&&j<8;j++){char b[16];int l=llama_token_to_piece(v,toks[j],b,16,0,false);b[l>15?15:l]=0;fprintf(stderr," %d(%s)",toks[j],b);}
        fprintf(stderr,"\n");
        struct llama_batch pb=llama_batch_init(nt,0,1);pb.n_tokens=nt;
        for(int j=0;j<nt;j++){pb.token[j]=toks[j];pb.pos[j]=j;pb.n_seq_id[j]=1;pb.seq_id[j][0]=0;pb.logits[j]=j==nt-1?1:0;}
        if(llama_decode(lctx,pb)!=0){fprintf(stderr,"[dbg] decode fail\n");llama_batch_free(pb);free(toks);return 1;}
        fprintf(stderr,"[dbg] decode ok\n");
        const float*logits=llama_get_logits_ith(lctx,-1);
        fprintf(stderr,"[dbg] logits[0..4]=%a %a %a %a %a\n",logits[0],logits[1],logits[2],logits[3],logits[4]);
        fprintf(stderr,"[dbg] logits[151643..151647]=%a %a %a %a %a\n",logits[151643],logits[151644],logits[151645],logits[151646],logits[151647]);

        llama_batch_free(pb);
        Sampler gs=sp;gs.count=0;int32_t pos=nt;
        struct llama_batch gb=llama_batch_init(1,0,1);
        gb.n_tokens=1;gb.n_seq_id[0]=1;gb.seq_id[0][0]=0;gb.logits[0]=1;
        for(int i=0;i<opt_max_new;i++){
            int tok=sample_token(llama_get_logits_ith(lctx,-1),nv,&gs);
            fprintf(stderr,"[dbg] i=%d tok=%d eos=%d\n",i,tok,eos);
            if(tok==eos||tok==0)break;
            char b[16];int l=llama_token_to_piece(v,tok,b,16,0,false);
            if(l>0){b[l>15?15:l]=0;printf("%s",b);fflush(stdout);}
            gb.token[0]=tok;gb.pos[0]=pos++;if(llama_decode(lctx,gb)!=0)break;
        }
        llama_batch_free(gb);printf("\n");free(toks);
    }

    sid_loader_close(&slc);
    gguf_idx_close(&gidx);
    FreeLibrary(hHelper);
    llama_free(lctx);llama_model_free(model);llama_backend_free();
    return 0;
}
