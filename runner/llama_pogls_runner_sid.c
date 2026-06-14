/*
 * llama_pogls_runner_sid.c — SID-coordinate inference runner
 * Build: gcc -O2 -I. -I<llama_inc> -o $@ $< llama.dll ggml.dll ... -lm
 * Usage: llama_pogls_runner_sid.exe model.gguf [options]
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
#include "llama.h"
#include "ggml-backend.h"
#include "gguf.h"

#define SID_IMPLEMENTATION
#include "../collection/sid.h"
#include "gguf_index.h"
#include "sid_loader.h"
#include "sid_cache.h"

#define MAX_TOKENS_CACHE 4096
#define MAX_CHAT_HISTORY 128
#define MAX_LINE 4096

typedef struct{
    float temp,top_p,repeat_penalty;int top_k,repeat_last_n;
    int tokens[MAX_TOKENS_CACHE],count;
}Sampler;

static int sample_token(const float*L,int n,Sampler*sp){
    float*p=(float*)malloc(n*4);memcpy(p,L,n*4);
    if(sp->repeat_penalty!=1.0f&&sp->count>0){
        int s=sp->count>sp->repeat_last_n?sp->count-sp->repeat_last_n:0;
        for(int i=s;i<sp->count;i++){int t=sp->tokens[i];
            if(t>=0&&t<n){if(p[t]<0)p[t]*=sp->repeat_penalty;else p[t]/=sp->repeat_penalty;}}}
    if(sp->temp>0)for(int i=0;i<n;i++)p[i]/=sp->temp;
    float mx=p[0];for(int i=1;i<n;i++)if(p[i]>mx)mx=p[i];
    float s=0;for(int i=0;i<n;i++){p[i]=expf(p[i]-mx);s+=p[i];}
    if(s>0)for(int i=0;i<n;i++)p[i]/=s;
    int k=sp->top_k>0&&sp->top_k<n,p=sp->top_p<1;
    ProbPair *pairs = NULL;
    int do_k = (sp->top_k > 0 && sp->top_k < n);
    int do_p = (sp->top_p < 1.0f);
    if (do_k || do_p) {
        pairs = (ProbPair*)malloc(n * sizeof(ProbPair));
        for (int i = 0; i < n; i++) { pairs[i].p = probs[i]; pairs[i].idx = i; }
        qsort(pairs, n, sizeof(ProbPair), prob_cmp_desc);
        if (do_k) {
            float kth = pairs[sp->top_k - 1].p;
            for (int i = 0; i < n; i++) if (probs[i] < kth) probs[i] = 0;
            float sum2 = 0; for (int i = 0; i < n; i++) sum2 += probs[i];
            if (sum2 > 0) for (int i = 0; i < n; i++) probs[i] /= sum2;
        }
        if (do_p) {
            float cum = 0;
            for (int i = 0; i < n; i++) {
                if (cum >= sp->top_p) { for (int j = i; j < n; j++) probs[pairs[j].idx] = 0; break; }
                cum += pairs[i].p;
            }
            float sum2 = 0; for (int i = 0; i < n; i++) sum2 += probs[i];
            if (sum2 > 0) for (int i = 0; i < n; i++) probs[i] /= sum2;
        }
        free(pairs);
    }
    float r=(float)rand()/(float)RAND_MAX,c=0;int tok=0;
    for(int i=0;i<n;i++){c+=p[i];if(r<c){tok=i;break;}}free(p);
    if(sp->count<MAX_TOKENS_CACHE)sp->tokens[sp->count++]=tok;
    return tok;
}

typedef struct{
    SIDLoaderCtx*loader;SIDCache*cache;SIDStore*twidx;
    uint64_t bytes_read,tensors_set;
    uint8_t*read_buf;size_t read_buf_sz;
}SidRunnerCtx;

static void set_tensor_sid_cb(struct ggml_tensor*t,void*ud){
    SidRunnerCtx*rc=(SidRunnerCtx*)ud;if(!t||!t->name[0]||!t->data)return;
    size_t sz=ggml_nbytes(t);
    if(sz>rc->read_buf_sz){rc->read_buf=(uint8_t*)realloc(rc->read_buf,sz);rc->read_buf_sz=sz;}
    uint8_t*s;size_t ss;
    if(sid_loader_load(rc->loader,t->name,rc->read_buf,&s,&ss)==0){
        ggml_backend_tensor_set(t,s,0,sz<ss?sz:ss);
        rc->tensors_set++;rc->bytes_read+=ss;}
}

int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"Usage: %s model.gguf [--twidx model.twidx] [--chat] [options]\n",argv[0]);return 1;}
    const char*gguf_path=NULL,*twidx_path=NULL,*opt_prompt=NULL;
    int opt_ngl=0,opt_max_new=256,opt_chat=0;
    Sampler sp={.7f,.9f,1.1f,40,64};
    uint64_t cache_mb=256;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--twidx")&&i+1<argc)twidx_path=argv[++i];
        else if(!strcmp(argv[i],"--ngl")&&i+1<argc)opt_ngl=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--temp")&&i+1<argc)sp.temp=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--top-p")&&i+1<argc)sp.top_p=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--top-k")&&i+1<argc)sp.top_k=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--repeat-penalty")&&i+1<argc)sp.repeat_penalty=(float)atof(argv[++i]);
        else if(!strcmp(argv[i],"--max-new")&&i+1<argc)opt_max_new=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--prompt")&&i+1<argc)opt_prompt=argv[++i];
        else if(!strcmp(argv[i],"--chat"))opt_chat=1;
        else if(!strcmp(argv[i],"--cache")&&i+1<argc)cache_mb=(uint64_t)atol(argv[++i]);
        else if(!strcmp(argv[i],"-h")){fprintf(stderr,"See --help\n");return 0;}
        else if(!gguf_path)gguf_path=argv[i];
    }
    if(!gguf_path){fprintf(stderr,"ERROR: missing model.gguf\n");return 1;}

    SIDCache cache;sid_cache_init(&cache,cache_mb*1024*1024);
    SIDLoaderCtx loader;
    if(sid_loader_open(&loader,gguf_path,&cache)!=0){fprintf(stderr,"ERROR: open GGUF\n");return 1;}
    SIDStore twidx;memset(&twidx,0,sizeof(twidx));
    if(twidx_path&&sid_read(twidx_path,&twidx)<=0)fprintf(stderr,"WARN: no .twidx\n");
    SidRunnerCtx rc;memset(&rc,0,sizeof(rc));rc.loader=&loader;rc.cache=&cache;rc.twidx=&twidx;

    llama_backend_init();ggml_backend_load_all();
    struct ggml_init_params gp={.mem_size=128*1024*1024,.mem_buffer=NULL};
    struct ggml_context*gc=ggml_init(gp);
    struct gguf_init_params gpar={.no_alloc=true,.ctx=&gc};
    struct gguf_context*gctx=gguf_init_from_file(gguf_path,gpar);
    if(!gctx){fprintf(stderr,"ERROR: gguf_init\n");return 1;}
    struct llama_model_params mp=llama_model_default_params();mp.n_gpu_layers=opt_ngl;
    PoglsTime t0,t1;clock_gettime(CLOCK_MONOTONIC,&t0);
    struct llama_model*model=llama_model_init_from_user(gctx,set_tensor_sid_cb,&rc,mp);
    if(!model){fprintf(stderr,"ERROR: model init\n");return 1;}
    clock_gettime(CLOCK_MONOTONIC,&t1);
    double lms=(t1.tv_sec-t0.tv_sec)*1000.0+(t1.tv_nsec-t0.tv_nsec)/1e6;
    fprintf(stderr,"\n[load] %llu tensors, %.0f ms, file=%llu cache=%llu\n",
        (unsigned long long)rc.tensors_set,lms,(unsigned long long)loader.file_hits,cache.hits);

    struct llama_context_params cp=llama_context_default_params();
    cp.n_ctx=2048;cp.n_threads=4;cp.n_threads_batch=4;
    struct llama_context*lctx=llama_init_from_model(model,cp);
    if(!lctx){fprintf(stderr,"ERROR: context\n");return 1;}
    const struct llama_vocab*v=llama_model_get_vocab(model);
    int nv=llama_vocab_n_tokens(v),eos=llama_vocab_eos(v);
    if(eos==-1)eos=151645;

    if(opt_chat){ /* interactive chat mode */
        char*roles[MAX_CHAT_HISTORY],*content[MAX_CHAT_HISTORY];int cc=0;
        roles[0]="system";content[0]="You are a helpful assistant.";cc=1;
        printf("\n=== SID Chat ===\n/exit /clear\n\n");
        char line[MAX_LINE];
        while(1){
            printf(">>> ");fflush(stdout);
            if(!fgets(line,sizeof(line),stdin))break;
            size_t ll=strlen(line);while(ll>0&&(line[ll-1]=='\n'||line[ll-1]=='\r'))line[--ll]=0;
            if(ll==0)continue;
            if(!strcmp(line,"/exit"))break;
            if(!strcmp(line,"/clear")){cc=0;roles[0]="system";content[0]="You are a helpful assistant.";cc=1;printf("Cleared.\n");continue;}
            roles[cc]="user";content[cc]=_strdup(line);cc++;
            size_t total=0;for(int j=0;j<cc;j++)total+=64+strlen(content[j]);
            char*fmt=(char*)calloc(total+128,1);size_t pos=0;
            for(int j=0;j<cc;j++)pos+=sprintf(fmt+pos,"<|im_start|>%s\n%s<|im_end|>\n",roles[j],content[j]);
            pos+=sprintf(fmt+pos,"<|im_start|>assistant\n");
            int nr=llama_tokenize(v,fmt,strlen(fmt),NULL,0,false,false);
            int nt=nr<0?-nr:nr;if(nt<=0||nt>2048-64){free(fmt);continue;}
            int*toks=(int*)malloc(nt*4);
            llama_tokenize(v,fmt,strlen(fmt),toks,nt,false,false);
            free(fmt);
            struct llama_batch pb=llama_batch_init(nt,0,1);pb.n_tokens=nt;
            for(int j=0;j<nt;j++){pb.token[j]=toks[j];pb.pos[j]=j;pb.n_seq_id[j]=1;pb.seq_id[j][0]=0;pb.logits[j]=j==nt-1?1:0;}
            if(llama_decode(lctx,pb)!=0){llama_batch_free(pb);free(toks);break;}
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
            llama_batch_free(gb);printf("\n");free(toks);
        }
        for(int i=0;i<cc;i++)free(content[i]);
    }

    if(opt_prompt){
        int nr=llama_tokenize(v,opt_prompt,strlen(opt_prompt),NULL,0,true,false);
        int nt=nr<0?-nr:nr;if(nt<=0){fprintf(stderr,"tokenize fail\n");return 1;}
        int*toks=(int*)malloc(nt*4);
        llama_tokenize(v,opt_prompt,strlen(opt_prompt),toks,nt,true,false);
        struct llama_batch pb=llama_batch_init(nt,0,1);pb.n_tokens=nt;
        for(int j=0;j<nt;j++){pb.token[j]=toks[j];pb.pos[j]=j;pb.n_seq_id[j]=1;pb.seq_id[j][0]=0;pb.logits[j]=j==nt-1?1:0;}
        if(llama_decode(lctx,pb)!=0){llama_batch_free(pb);free(toks);return 1;}
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
        llama_batch_free(gb);printf("\n");free(toks);
    }

    llama_free(lctx);llama_model_free(model);gguf_free(gctx);ggml_free(gc);
    fprintf(stderr,"\n[stats] file=%llu cache=%llu evict=%llu pool=%llu/%llu\n",
        (unsigned long long)loader.file_hits,cache.hits,cache.evictions,
        (unsigned long long)cache.pool_used,(unsigned long long)cache.pool_size);
    sid_loader_close(&loader);sid_cache_clear(&cache);free(rc.read_buf);llama_backend_free();
    return 0;
}
