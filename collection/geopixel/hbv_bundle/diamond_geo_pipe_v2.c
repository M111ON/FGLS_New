/*
 * diamond_geo_pipe_v2.c
 * ══════════════════════
 * Pipeline:
 *   binary → onion shell 0 header (64B) → diamond flat encode
 *          → data chunks reordered by Hilbert (onion_chunk_at)
 *          → BMP (32-wide, N tiles tall)
 *          → geopixel v21 encode → .gp15
 *
 *   .gp15 → geopixel decode → BMP
 *          → tile 0 = shell 0 header → reconstruct OnionShell
 *          → tile 1+ = chunks in Hilbert order → reorder → binary
 *
 * BMP tile layout:
 *   tile 0  : onion shell 0 header (64B) — diamond encoded, fits pixel 0..21
 *   tile 1+ : data chunk slots
 *             slot = [orig_chunk_idx u16][stored_sz u16][bytes×stored_sz]
 *             packed left-to-right, new tile when full
 *
 * Compile:
 *   gcc -O2 -o diamond_geo_pipe_v2 diamond_geo_pipe_v2.c \
 *       -I/tmp/diamond -I/tmp/geopixel/geopixel \
 *       -lm -lzstd -lpng -lpthread
 *
 * Usage:
 *   ./diamond_geo_pipe_v2 encode    <in.bin>
 *   ./diamond_geo_pipe_v2 decode    <in.bmp>   [out.bin]
 *   ./diamond_geo_pipe_v2 roundtrip <in.bin>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tring.h"
#include "pogls_fold.h"
#include "geo_diamond_field_v4.h"
#include "geo_onion_shell.h"

/* ── constants ───────────────────────────────────────────────────── */
#define CHUNK_SZ    64
#define TILE_PX     32
#define TILE_BYTES  (TILE_PX*TILE_PX*3)   /* 3072 */
#define SLOT_HDR    4                      /* [chunk_idx u16][stored_sz u16] */
#define DATA_BYTES  (TILE_BYTES)           /* tile 0 = header only; tile 1+ = full */

/* tile 0 is reserved for onion shell 0 header (64B encoded) */
/* data tiles start at tile 1 */
#define DATA_TILE_BYTES  TILE_BYTES        /* 3072B per data tile */

/* geopixel binary path and command prefix (with DLL PATH on Windows) */
static char geopixel_bin[1024] = {0};
static char geopixel_prefix[2048] = {0};

#ifdef _WIN32
  #include <windows.h>
  #define NULL_DEVICE "NUL"
#else
  #define NULL_DEVICE "/dev/null"
#endif

/* ── xxh64 ───────────────────────────────────────────────────────── */
#define _H1 0x9e3779b97f4a7c15ULL
#define _H2 0x6c62272e07bb0142ULL
static inline uint64_t _rot(uint64_t x,int r){return(x<<r)|(x>>(64-r));}
static inline uint64_t _hu(uint64_t a,uint64_t w){
    a^=(w*_H1);a=_rot(a,27);a=a*_H2+0x94d049bb133111ebULL;return a;}
static uint64_t xxh64(const uint8_t*d,size_t n){
    uint64_t a=_H1^n; size_t i=0;
    for(;i+8<=n;i+=8){uint64_t w;memcpy(&w,d+i,8);a=_hu(a,w);}
    if(i<n){uint64_t t=0;memcpy(&t,d+i,n-i);a=_hu(a,t);}
    a^=(a>>33);a*=_H1;a^=(a>>29);a*=_H2;a^=(a>>32);return a;}

/* ── BMP ─────────────────────────────────────────────────────────── */
static int bmp_write(const char *path,const uint8_t *rgb,int w,int h){
    int rs=(w*3+3)&~3,psz=rs*h,fsz=54+psz;
    FILE*f=fopen(path,"wb");if(!f){perror(path);return-1;}
    uint8_t bfh[14]={'B','M',
        (uint8_t)fsz,(uint8_t)(fsz>>8),(uint8_t)(fsz>>16),(uint8_t)(fsz>>24),
        0,0,0,0,54,0,0,0};
    fwrite(bfh,1,14,f);
    uint8_t dib[40]={0};
    dib[0]=40;*(int32_t*)(dib+4)=w;*(int32_t*)(dib+8)=-h;
    dib[12]=1;dib[14]=24;
    fwrite(dib,1,40,f);
    uint8_t *row=calloc(rs,1);
    for(int y=0;y<h;y++){
        for(int x=0;x<w;x++){
            const uint8_t*p=rgb+(y*w+x)*3;
            row[x*3]=p[2];row[x*3+1]=p[1];row[x*3+2]=p[0];
        }
        fwrite(row,1,rs,f);
    }
    free(row);fclose(f);return 0;
}

static uint8_t *bmp_read(const char *path,int *W,int *H){
    FILE*f=fopen(path,"rb");if(!f){perror(path);return NULL;}
    uint8_t bfh[14];if(fread(bfh,1,14,f)!=14){fclose(f);return NULL;}
    if(bfh[0]!='B'||bfh[1]!='M'){fclose(f);return NULL;}
    uint8_t dib[40];if(fread(dib,1,40,f)!=40){fclose(f);return NULL;}
    int32_t w,hr;memcpy(&w,dib+4,4);memcpy(&hr,dib+8,4);
    int h=hr<0?-hr:hr,td=hr<0;
    *W=w;*H=h;
    int rs=(w*3+3)&~3;
    uint32_t po;memcpy(&po,bfh+10,4);
    fseek(f,po,SEEK_SET);
    uint8_t *rgb=malloc((size_t)w*h*3);
    uint8_t *row=malloc(rs);
    for(int y=0;y<h;y++){
        if(fread(row,1,rs,f)!=(size_t)rs)break;
        int dy=td?y:(h-1-y);
        for(int x=0;x<w;x++){
            uint8_t*p=rgb+(dy*w+x)*3;
            p[0]=row[x*3+2];p[1]=row[x*3+1];p[2]=row[x*3];
        }
    }
    free(row);fclose(f);return rgb;
}

/* ── geopixel subprocess (with DLL PATH prefix) ────────────────── */
static void geo_encode(const char *bmp, char *out, size_t n){
    char cmd[2048];
    snprintf(cmd,sizeof(cmd),"%s%s \"%s\" %s 2>%s",
             geopixel_prefix,geopixel_bin,bmp,NULL_DEVICE,NULL_DEVICE);
    system(cmd);
    snprintf(out,n,"%s.gp15",bmp);
}
static void geo_decode(const char *gp15, char *out_bmp, size_t n){
    char cmd[2048];
    snprintf(cmd,sizeof(cmd),"%s%s \"%s\" %s 2>%s",
             geopixel_prefix,geopixel_bin,gp15,NULL_DEVICE,NULL_DEVICE);
    system(cmd);
    snprintf(out_bmp,n,"%s.bmp",gp15);
}

/* ══════════════════════════════════════════════════════════════════
 * ENCODE
 * binary → diamond(all chunks) → onion shell ordering → BMP
 * tile 0 : shell 0 header (64B, diamond-encoded)
 * tile 1+: data slots in Hilbert order
 * ══════════════════════════════════════════════════════════════════ */
static int do_encode(const uint8_t *data, size_t orig_size,
                     const char *bmp_path){
    int n_chunks=(int)((orig_size+CHUNK_SZ-1)/CHUNK_SZ);
    uint64_t digest=xxh64(data,orig_size);

    /* ── 1. diamond encode all chunks ───────────────────────── */
    DiamondField df;
    dfield_init(&df,(uint32_t)(n_chunks+64));

    uint8_t  **enc  = malloc(n_chunks*sizeof(uint8_t*));
    uint32_t  *esz  = malloc(n_chunks*sizeof(uint32_t));

    for(int i=0;i<n_chunks;i++){
        uint8_t chunk[CHUNK_SZ]={0};
        size_t src=(size_t)i*CHUNK_SZ;
        size_t cp=orig_size-src; if(cp>CHUNK_SZ)cp=CHUNK_SZ;
        memcpy(chunk,data+src,cp);
        uint32_t tick=dfield_encode_flat(&df,chunk);
        uint32_t sz; const uint8_t*p=tring_read(&df.tring,tick,&sz);
        enc[i]=malloc(sz); memcpy(enc[i],p,sz); esz[i]=sz;
    }

    /* ── 2. init OnionShell — seed from digest ──────────────── */
    uint8_t shell_count=4;   /* shells 1..4 virtual */
    OnionShell os;
    onion_init(&os,(uint32_t)n_chunks,digest,shell_count);

    /* ── 3. build Hilbert-ordered write sequence ────────────── */
    /* order[k] = which original chunk_idx to write at position k */
    /* We walk shell 1..shell_count in ring order via onion_chunk_at */
    /* Chunks not covered by shells (wrap) are still written — all n_chunks */
    uint8_t *written=calloc(n_chunks,1);
    int     *order  =malloc(n_chunks*sizeof(int));
    int      pos    =0;

    for(uint32_t sh=1;sh<=(uint32_t)shell_count && pos<n_chunks;sh++){
        uint32_t cells=6*sh;
        for(uint32_t c=0;c<cells && pos<n_chunks;c++){
            uint32_t ci=onion_chunk_at(&os,sh,c);
            if(ci<(uint32_t)n_chunks && !written[ci]){
                order[pos++]=(int)ci;
                written[ci]=1;
            }
        }
    }
    /* append any remaining chunks not covered by shells */
    for(int i=0;i<n_chunks && pos<n_chunks;i++)
        if(!written[i]){order[pos++]=i; written[i]=1;}
    free(written);

    /* ── 4. pack slots into data tiles (tile 1+) ────────────── */
    /* compute tile assignment for each ordered slot */
    int *tile_of=malloc(n_chunks*sizeof(int));
    int *off_of =malloc(n_chunks*sizeof(int));
    int n_data_tiles=0;
    {
        int ct=0,co=0;
        for(int k=0;k<n_chunks;k++){
            int ci=order[k];
            int slot=SLOT_HDR+(int)esz[ci];
            if(co+slot>DATA_TILE_BYTES){ct++;co=0;}
            tile_of[k]=ct; off_of[k]=co; co+=slot;
        }
        n_data_tiles=ct+1;
    }

    /* ── 5. build BMP: tile 0 = shell header, tile 1+ = data ── */
    int total_tiles=1+n_data_tiles;
    int img_w=TILE_PX, img_h=total_tiles*TILE_PX;
    uint8_t *rgb=calloc((size_t)img_w*img_h*3,1);

    /* tile 0: onion shell 0 header (64B) written directly as bytes */
    /* we store it raw in the first 64 bytes of tile 0 pixel data   */
    /* (no diamond encode for header itself — it's already 64B)      */
    uint8_t hdr_buf[64];
    onion_header_write(&os,hdr_buf);
    /* also embed orig_size and digest in hdr_buf spare bytes [36..63] */
    memcpy(hdr_buf+36,&orig_size,4);           /* orig_size u32 */
    memcpy(hdr_buf+40,&digest,8);              /* xxh64 u64     */
    memcpy(hdr_buf+48,&n_chunks,4);            /* n_chunks u32  */
    /* write 64B into tile 0 as RGB triplets (ceil(64/3)=22 pixels) */
    for(int b=0;b<64;b++){
        size_t px_off=((size_t)(b/3))*3 + (b%3);
        rgb[px_off]=hdr_buf[b];
    }

    /* tile 1+ : data slots in Hilbert order */
    for(int k=0;k<n_chunks;k++){
        int ci=order[k];
        size_t tile_base=(size_t)(1+tile_of[k])*TILE_PX*TILE_PX*3;
        uint8_t *dp=rgb+tile_base+off_of[k];
        dp[0]=(uint8_t)(ci&0xFF); dp[1]=(uint8_t)((ci>>8)&0xFF);
        dp[2]=(uint8_t)(esz[ci]&0xFF); dp[3]=(uint8_t)((esz[ci]>>8)&0xFF);
        memcpy(dp+4,enc[ci],esz[ci]);
    }

    int r=bmp_write(bmp_path,rgb,img_w,img_h);

    uint32_t total_enc=0;
    for(int i=0;i<n_chunks;i++) total_enc+=esz[i];
    printf("diamond  orig=%zuB chunks=%d enc=%uB ratio=%.2fx\n",
           orig_size,n_chunks,total_enc,(double)(n_chunks*CHUNK_SZ)/total_enc);
    printf("onion    seed=%016llX shells=%d\n",
           (unsigned long long)digest,shell_count);
    printf("bmp      tiles=1+%d=%d img=%dx%d\n",
           n_data_tiles,total_tiles,img_w,img_h);

    for(int i=0;i<n_chunks;i++) free(enc[i]);
    free(enc); free(esz); free(order);
    free(tile_of); free(off_of);
    free(rgb); dfield_free(&df); onion_free(&os);
    return r;
}

/* ══════════════════════════════════════════════════════════════════
 * DECODE
 * BMP → read tile 0 (shell header) → reconstruct OnionShell
 *     → read data tiles → diamond decode → reorder → binary
 * ══════════════════════════════════════════════════════════════════ */
static int do_decode(const char *bmp_path, const char *out_path){
    int W,H;
    uint8_t *rgb=bmp_read(bmp_path,&W,&H);
    if(!rgb) return 1;
    if(W!=TILE_PX){
        fprintf(stderr,"bad width %d\n",W); free(rgb); return 1;}

    /* ── 1. read shell 0 header from tile 0 ─────────────────── */
    uint8_t hdr_buf[64]={0};
    for(int b=0;b<64;b++){
        size_t px_off=((size_t)(b/3))*3+(b%3);
        hdr_buf[b]=rgb[px_off];
    }

    OnionShell os; memset(&os,0,sizeof(os));
    if(onion_header_read(&os,hdr_buf)!=0){
        fprintf(stderr,"shell header corrupt\n"); free(rgb); return 1;}

    size_t orig_size=0; memcpy(&orig_size,hdr_buf+36,4);
    uint64_t stored_dig=0; memcpy(&stored_dig,hdr_buf+40,8);
    int n_chunks=0; memcpy(&n_chunks,hdr_buf+48,4);

    /* rebuild chunk_seq for OnionShell (identity) */
    os.chunk_seq=malloc((size_t)n_chunks*sizeof(uint32_t));
    os.n_chunks=(uint32_t)n_chunks;
    for(int i=0;i<n_chunks;i++) os.chunk_seq[i]=(uint32_t)i;

    /* ── 2. rebuild Hilbert write order (same as encode) ──────── */
    uint8_t *written=calloc(n_chunks,1);
    int     *order  =malloc(n_chunks*sizeof(int));
    int      pos    =0;
    uint8_t  sc     =os.hdr.fpt.shell_count;

    for(uint32_t sh=1;sh<=(uint32_t)sc && pos<n_chunks;sh++){
        uint32_t cells=6*sh;
        for(uint32_t c=0;c<cells && pos<n_chunks;c++){
            uint32_t ci=onion_chunk_at(&os,sh,c);
            if(ci<(uint32_t)n_chunks && !written[ci]){
                order[pos++]=(int)ci; written[ci]=1;}
        }
    }
    for(int i=0;i<n_chunks && pos<n_chunks;i++)
        if(!written[i]){order[pos++]=i; written[i]=1;}
    free(written);

    /* ── 3. init diamond field and decode slots ──────────────── */
    DiamondField df;
    dfield_init(&df,(uint32_t)(n_chunks+64));

    uint8_t *out=calloc(1,(size_t)n_chunks*CHUNK_SZ);

    int n_data_tiles=(H/TILE_PX)-1;
    int ok=0;

    /* walk data tiles (tile 1+) and decode slots in order */
    int k=0;  /* position in Hilbert order */
    for(int dt=0;dt<n_data_tiles && k<n_chunks;dt++){
        size_t tile_base=(size_t)(1+dt)*TILE_PX*TILE_PX*3;
        int co=0;
        while(co+SLOT_HDR<=DATA_TILE_BYTES && k<n_chunks){
            uint8_t *dp=rgb+tile_base+co;
            uint16_t cidx=(uint16_t)(dp[0]|(dp[1]<<8));
            uint16_t esz =(uint16_t)(dp[2]|(dp[3]<<8));
            if(esz==0||esz>CHUNK_SZ+4) break;
            if(co+SLOT_HDR+(int)esz>DATA_TILE_BYTES) break;

            uint32_t tick=tring_push(&df.tring,dp+4,esz);
            uint8_t decoded[CHUNK_SZ];
            if(dfield_decode_flat(&df,tick,decoded)==0){
                memcpy(out+(size_t)cidx*CHUNK_SZ,decoded,CHUNK_SZ);
                ok++;
            }
            co+=SLOT_HDR+(int)esz;
            k++;
        }
    }

    /* ── 4. trim and verify ──────────────────────────────────── */
    uint64_t got=xxh64(out,orig_size);
    if(got!=stored_dig){
        fprintf(stderr,"FAIL xxh64 got=%016llX stored=%016llX\n",
                (unsigned long long)got,(unsigned long long)stored_dig);
        free(rgb);free(out);free(order);dfield_free(&df);onion_free(&os);
        return 2;
    }

    FILE*f=fopen(out_path,"wb");
    if(!f){perror(out_path);free(rgb);free(out);free(order);
           dfield_free(&df);onion_free(&os);return 1;}
    fwrite(out,1,orig_size,f); fclose(f);

    printf("decode   ok=%d/%d orig=%zuB xxh64 PASS %016llX\n",
           ok,n_chunks,orig_size,(unsigned long long)got);

    free(rgb);free(out);free(order);dfield_free(&df);onion_free(&os);
    return 0;
}

/* discover geopixel binary path + DLL PATH prefix */
static void init_geopixel_path(const char *argv0){
#ifdef _WIN32
    char modpath[1024]; DWORD len=GetModuleFileNameA(NULL,modpath,sizeof(modpath)-1);
    if(len>0){modpath[len]=0;char*p=strrchr(modpath,'\\');
        if(p)*p=0;snprintf(geopixel_bin,sizeof(geopixel_bin),"%s\\geopixel_v21_o25",modpath);
        snprintf(geopixel_prefix,sizeof(geopixel_prefix),
                 "set PATH=C:\\msys64\\mingw64\\bin;%%PATH%% && ",modpath);}
    else{strcpy(geopixel_bin,".\\geopixel_v21_o25");geopixel_prefix[0]=0;}
#else
    const char *slash=strrchr(argv0,'/');
    if(slash){size_t d=(size_t)(slash-argv0);
        snprintf(geopixel_bin,sizeof(geopixel_bin),"%.*s/geopixel_v21_o25",(int)d,argv0);
        snprintf(geopixel_prefix,sizeof(geopixel_prefix),
                 "export PATH=\"%.*s\":$PATH && ",(int)d,argv0);}
    else{strcpy(geopixel_bin,"./geopixel_v21_o25");geopixel_prefix[0]=0;}
#endif
}

/* ── main ────────────────────────────────────────────────────────── */
int main(int argc,char **argv){
    init_geopixel_path(argv[0]);
    if(argc<3){
        fprintf(stderr,
            "Usage:\n"
            "  %s encode    <in.bin>  [out.bmp]\n"
            "  %s decode    <in.bmp>  [out.bin]\n"
            "  %s roundtrip <in.bin>\n",
            argv[0],argv[0],argv[0]);
        return 1;
    }
    const char *cmd=argv[1],*src=argv[2];

    if(strcmp(cmd,"encode")==0){
        char def[512]; snprintf(def,sizeof(def),"%s.bmp",src);
        const char *dst=(argc>=4)?argv[3]:def;
        FILE*f=fopen(src,"rb");if(!f){perror(src);return 1;}
        fseek(f,0,SEEK_END);size_t sz=ftell(f);rewind(f);
        uint8_t*data=malloc(sz);
        if(fread(data,1,sz,f)!=sz){free(data);fclose(f);return 1;}
        fclose(f);
        int r=do_encode(data,sz,dst);
        if(r==0){
            char gp15[512]; geo_encode(dst,gp15,sizeof(gp15));
            printf("geopixel → %s\n",gp15);
        }
        free(data);return r;
    }

    if(strcmp(cmd,"decode")==0){
        char def[512]; snprintf(def,sizeof(def),"%s.out",src);
        const char *dst=(argc>=4)?argv[3]:def;
        return do_decode(src,dst);
    }

    if(strcmp(cmd,"roundtrip")==0){
        char bmp[512],gp15[512],bmp2[512],out[512];
        snprintf(bmp,sizeof(bmp),"%s.bmp",src);
        snprintf(out,sizeof(out),"%s.rt.bin",src);

        printf("=== Roundtrip v2 (onion shell): %s ===\n",src);

        FILE*f=fopen(src,"rb");if(!f){perror(src);return 1;}
        fseek(f,0,SEEK_END);size_t sz=ftell(f);rewind(f);
        uint8_t*data=malloc(sz);
        if(fread(data,1,sz,f)!=sz){free(data);fclose(f);return 1;}
        fclose(f);

        printf("[1] binary → diamond + onion → BMP\n");
        if(do_encode(data,sz,bmp)){free(data);return 1;}

        printf("[2] BMP → geopixel encode\n");
        geo_encode(bmp,gp15,sizeof(gp15));
        printf("    → %s (%.0fB)\n",gp15,(double)({
            FILE*g=fopen(gp15,"rb");long s=0;
            if(g){fseek(g,0,SEEK_END);s=ftell(g);fclose(g);}s;}));

        printf("[3] geopixel decode → BMP\n");
        geo_decode(gp15,bmp2,sizeof(bmp2));
        printf("    → %s\n",bmp2);

        printf("[4] BMP → onion + diamond → binary\n");
        int r=do_decode(bmp2,out);

        if(r==0){
            FILE*g=fopen(out,"rb");
            fseek(g,0,SEEK_END);size_t sz2=ftell(g);rewind(g);
            uint8_t*d2=malloc(sz2);
            if(fread(d2,1,sz2,g)!=sz2){free(d2);fclose(g);r=3;}
            else{
                fclose(g);
                int match=(sz==sz2&&memcmp(data,d2,sz)==0);
                printf("\n%s  %zuB in → %zuB out\n",
                    match?"✓ ROUNDTRIP PASS":"✗ ROUNDTRIP FAIL",sz,sz2);
                free(d2);
            }
        }
        free(data);return r;
    }

    fprintf(stderr,"unknown cmd: %s\n",cmd);return 1;
}
