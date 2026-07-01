#ifndef SID_LOADER_H
#define SID_LOADER_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gguf_index.h"
#include "sid_cache.h"

#define SID_LOADER_NAME_MAX  256

typedef struct {
    char     name[SID_LOADER_NAME_MAX];
    uint32_t dtype;
    uint64_t offset;
    uint64_t size;
} SIDLoaderTensor;

typedef struct {
    const char       *gguf_path;
    FILE             *gguf_file;
    GGUFTensorIndex   idx;
    SIDCache         *cache;
    uint64_t          n_tensors;
    uint64_t          bytes_read;
    uint64_t          file_hits;
    uint64_t          cache_hits;
} SIDLoaderCtx;

static int sid_loader_open(SIDLoaderCtx *ctx, const char *gguf_path, SIDCache *cache) {
    memset(ctx,0,sizeof(*ctx)); ctx->gguf_path=gguf_path; ctx->cache=cache;
    ctx->gguf_file=fopen(gguf_path,"rb");
    if(!ctx->gguf_file) return -1;
    if(gguf_idx_open(gguf_path,&ctx->idx)!=0){fclose(ctx->gguf_file);return -1;}
    ctx->n_tensors=ctx->idx.n_tensors; return 0;
}

static void sid_loader_close(SIDLoaderCtx *ctx) {
    if(ctx->gguf_file){fclose(ctx->gguf_file);ctx->gguf_file=NULL;}
    gguf_idx_close(&ctx->idx);
}

static int64_t sid_loader_find(SIDLoaderCtx *ctx, const char *name) {
    for(uint64_t i=0;i<ctx->idx.n_tensors;i++)
        if(strcmp(ctx->idx.names[i],name)==0) return (int64_t)i;
    return -1;
}

static int sid_loader_read(SIDLoaderCtx *ctx, uint64_t ti, uint8_t *buf) {
    if(ti>=ctx->idx.n_tensors) return -1;
    uint64_t off=gguf_idx_tensor_abs_offset(&ctx->idx, ti), sz=ctx->idx.sizes[ti];
    if (fseeko64(ctx->gguf_file, (__int64)off, SEEK_SET) != 0) return -1;
    if(fread(buf,1,sz,ctx->gguf_file)!=sz) return -1;
    ctx->bytes_read+=sz; ctx->file_hits++; return 0;
}

static int sid_loader_read_by_name(SIDLoaderCtx *ctx, const char *name, uint8_t *buf) {
    int64_t ti=sid_loader_find(ctx,name); if(ti<0) return -1;
    return sid_loader_read(ctx,(uint64_t)ti,buf);
}

static int sid_loader_info(SIDLoaderCtx *ctx, const char *name, SIDLoaderTensor *info) {
    int64_t ti=sid_loader_find(ctx,name); if(ti<0) return -1;
    strncpy(info->name,ctx->idx.names[ti],SID_LOADER_NAME_MAX-1);
    info->dtype=ctx->idx.dtypes[ti]; info->offset=ctx->idx.offsets[ti]; info->size=ctx->idx.sizes[ti];
    return 0;
}

static int sid_loader_is_norm(const char *name) {
    return strstr(name,"norm")!=NULL||strstr(name,"_norm")!=NULL||strstr(name,"bias")!=NULL;
}

static int sid_loader_load(SIDLoaderCtx *ctx, const char *name, uint8_t *read_buf, uint8_t **data, size_t *size) {
    uint8_t *c; size_t cs;
    if(sid_cache_get(ctx->cache,name,&c,&cs)==0){*data=c;*size=cs;ctx->cache_hits++;return 0;}
    int64_t ti=sid_loader_find(ctx,name); if(ti<0) return -1;
    uint64_t sz=ctx->idx.sizes[ti], off=gguf_idx_tensor_abs_offset(&ctx->idx, (uint64_t)ti);
    if (fseeko64(ctx->gguf_file, (__int64)off, SEEK_SET) != 0) return -1; if(fread(read_buf,1,sz,ctx->gguf_file)!=sz) return -1;
    ctx->bytes_read+=sz; ctx->file_hits++; *data=read_buf; *size=(size_t)sz;
    if(!sid_loader_is_norm(name)) sid_cache_put_compressed(ctx->cache,name,0,read_buf,(size_t)sz);
    return 0;
}

#endif
