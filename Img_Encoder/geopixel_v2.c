#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <zstd.h>

// GEOPIXEL v2 — Delta-pos + zstd
// Stream layout (all zstd-compressed independently):
//   [4B magic] [4B w] [4B h]
//   [4B bmap_sz] [bmap_sz B bitmap]
//   [4B ne_sz]   [ne_sz B  n_entries per tile (u16 each)]
//   [4B dpos_sz] [dpos_sz B delta-pos stream (u8, delta/3 fits 0..255)]
//   [4B cnt_sz]  [cnt_sz B  count stream (u8)]
//
// Delta-pos encoding:
//   pos 0..767, delta between consecutive active positions in tile
//   reset to 0 at tile boundary
//   delta/3 stored as u8 (delta max=767, /3 fits u8 → small loss? no: store full u16 delta then zstd)
//   Actually: delta fits 0..767, store as u16 LE — zstd handles it
//   BUT: split into lo/hi byte separately → entropy of lo byte is very low

#define TILE 16
#define AXIS 768
#define MAGIC 0x47454F32u  // "GEO2"

typedef struct { int w,h; uint8_t *px; } Img;
int bmp_load(const char *path, Img *img){
    FILE *f=fopen(path,"rb"); if(!f) return 0;
    uint8_t hdr[54]; fread(hdr,1,54,f);
    img->w=*(int32_t*)(hdr+18); img->h=*(int32_t*)(hdr+22);
    int rs=(img->w*3+3)&~3;
    img->px=malloc(img->w*img->h*3);
    fseek(f,*(uint32_t*)(hdr+10),SEEK_SET);
    uint8_t *row=malloc(rs);
    for(int y=img->h-1;y>=0;y--){
        fread(row,1,rs,f);
        for(int x=0;x<img->w;x++){
            int d=(y*img->w+x)*3;
            img->px[d+0]=row[x*3+2];
            img->px[d+1]=row[x*3+1];
            img->px[d+2]=row[x*3+0];
        }
    }
    free(row); fclose(f); return 1;
}

typedef struct { uint8_t *buf; int sz, cap; } Buf;
void bp(Buf*b,uint8_t v){ if(b->sz==b->cap){b->cap*=2;b->buf=realloc(b->buf,b->cap);} b->buf[b->sz++]=v; }
void bp16(Buf*b,uint16_t v){ bp(b,v&0xFF); bp(b,v>>8); }
void bp32(Buf*b,uint32_t v){ bp(b,v&0xFF); bp(b,(v>>8)&0xFF); bp(b,(v>>16)&0xFF); bp(b,v>>24); }
Buf newbuf(int cap){ Buf b; b.cap=cap; b.sz=0; b.buf=malloc(cap); return b; }

// zstd compress buf → new malloc'd buf, returns size
uint8_t* zst_compress(const uint8_t*src, int src_sz, int*out_sz, int level){
    size_t cap=ZSTD_compressBound(src_sz);
    uint8_t*dst=malloc(cap);
    size_t r=ZSTD_compress(dst,cap,src,src_sz,level);
    if(ZSTD_isError(r)){free(dst);*out_sz=0;return NULL;}
    *out_sz=(int)r;
    return dst;
}
uint8_t* zst_decompress(const uint8_t*src, int src_sz, int orig_sz){
    uint8_t*dst=malloc(orig_sz);
    size_t r=ZSTD_decompress(dst,orig_sz,src,src_sz);
    if(ZSTD_isError(r)||r!=(size_t)orig_sz){free(dst);return NULL;}
    return dst;
}

double entropy8(const uint8_t*b, int n){
    int f[256]={0}; for(int i=0;i<n;i++) f[b[i]]++;
    double e=0; for(int i=0;i<256;i++){
        if(!f[i]) continue; double p=(double)f[i]/n;
        e-=p*(log(p)/log(2.0));} return e;
}
double psnr(const uint8_t*a,const uint8_t*b,int n){
    long s=0; for(int i=0;i<n;i++){int d=(int)a[i]-(int)b[i];s+=d*d;}
    double mse=(double)s/n; return mse==0?999.0:10.0*log10(65025.0/mse);
}

// write u32 LE into buf at offset
void write32(uint8_t*buf,int off,uint32_t v){
    buf[off]=v&0xFF; buf[off+1]=(v>>8)&0xFF; buf[off+2]=(v>>16)&0xFF; buf[off+3]=v>>24;
}
uint32_t read32(const uint8_t*buf,int off){
    return buf[off]|(buf[off+1]<<8)|(buf[off+2]<<16)|((uint32_t)buf[off+3]<<24);
}

int main(int argc, char**argv){
    const char *path=argc>1?argv[1]:"test02.bmp";
    Img img; if(!bmp_load(path,&img)){fprintf(stderr,"load failed\n");return 1;}
    int w=img.w,h=img.h,n=w*h,raw=n*3;
    int tw=(w+TILE-1)/TILE, th=(h+TILE-1)/TILE, nt=tw*th;
    int bmap_bytes=(nt+7)/8;

    // ── ENCODE ──────────────────────────────────────────────
    Buf bmap = newbuf(bmap_bytes+1); bmap.sz=bmap_bytes; memset(bmap.buf,0,bmap_bytes);
    Buf s_ne   = newbuf(nt*2+8);
    Buf s_dpos = newbuf(1<<21);
    Buf s_cnt  = newbuf(1<<20);

    long total_entries=0;
    int tile_id=0;

    for(int ty=0;ty<th;ty++){
        for(int tx=0;tx<tw;tx++,tile_id++){
            int freq[AXIS]={0}, tile_n=0;
            for(int dy=0;dy<TILE;dy++){
                int py=ty*TILE+dy; if(py>=h) continue;
                for(int dx=0;dx<TILE;dx++){
                    int px2=tx*TILE+dx; if(px2>=w) continue;
                    int i=py*w+px2;
                    freq[img.px[i*3+0]]++;
                    freq[256+img.px[i*3+1]]++;
                    freq[512+img.px[i*3+2]]++;
                    tile_n++;
                }
            }
            if(!tile_n) continue;
            bmap.buf[tile_id>>3] |= (1<<(tile_id&7));

            int ne=0; for(int p=0;p<AXIS;p++) if(freq[p]) ne++;
            total_entries+=ne;
            bp16(&s_ne,(uint16_t)ne);

            // delta-pos: reset per tile, store u16 deltas
            int prev=0;
            for(int p=0;p<AXIS;p++){
                if(!freq[p]) continue;
                int delta=p-prev; prev=p;
                bp16(&s_dpos,(uint16_t)delta);
                bp16(&s_cnt,(uint16_t)freq[p]);
            }
        }
    }

    // compress each stream with zstd level 19
    int zne_sz, zdpos_sz, zcnt_sz, zbmap_sz;
    uint8_t *zne   = zst_compress(s_ne.buf,   s_ne.sz,   &zne_sz,   19);
    uint8_t *zdpos = zst_compress(s_dpos.buf, s_dpos.sz, &zdpos_sz, 19);
    uint8_t *zcnt  = zst_compress(s_cnt.buf,  s_cnt.sz,  &zcnt_sz,  19);
    uint8_t *zbmap = zst_compress(bmap.buf,   bmap.sz,   &zbmap_sz, 19);

    // pack into final container
    // header: magic(4) w(4) h(4) | bmap_orig(4) bmap_zst(4) | ne_orig(4) ne_zst(4) |
    //         dpos_orig(4) dpos_zst(4) | cnt_orig(4) cnt_zst(4)
    int header_sz = 4+4+4 + (4+4)*4;  // 40B
    int enc_total = header_sz + zbmap_sz + zne_sz + zdpos_sz + zcnt_sz;

    uint8_t *container = malloc(enc_total);
    int off=0;
    write32(container,off,MAGIC);          off+=4;
    write32(container,off,(uint32_t)w);    off+=4;
    write32(container,off,(uint32_t)h);    off+=4;
    // bmap
    write32(container,off,(uint32_t)bmap.sz);  off+=4;
    write32(container,off,(uint32_t)zbmap_sz); off+=4;
    // ne
    write32(container,off,(uint32_t)s_ne.sz);  off+=4;
    write32(container,off,(uint32_t)zne_sz);   off+=4;
    // dpos
    write32(container,off,(uint32_t)s_dpos.sz); off+=4;
    write32(container,off,(uint32_t)zdpos_sz);  off+=4;
    // cnt
    write32(container,off,(uint32_t)s_cnt.sz); off+=4;
    write32(container,off,(uint32_t)zcnt_sz);  off+=4;
    // data
    memcpy(container+off, zbmap, zbmap_sz);  off+=zbmap_sz;
    memcpy(container+off, zne,   zne_sz);    off+=zne_sz;
    memcpy(container+off, zdpos, zdpos_sz);  off+=zdpos_sz;
    memcpy(container+off, zcnt,  zcnt_sz);   off+=zcnt_sz;

    // ── DECODE ──────────────────────────────────────────────
    off=0;
    uint32_t magic=read32(container,off); off+=4;
    int dw=read32(container,off); off+=4;
    int dh=read32(container,off); off+=4;
    if(magic!=MAGIC||dw!=w||dh!=h){fprintf(stderr,"header mismatch\n");return 1;}

    int rbmap_orig=read32(container,off); off+=4; int rbmap_zst=read32(container,off); off+=4;
    int rne_orig  =read32(container,off); off+=4; int rne_zst  =read32(container,off); off+=4;
    int rdpos_orig=read32(container,off); off+=4; int rdpos_zst=read32(container,off); off+=4;
    int rcnt_orig =read32(container,off); off+=4; int rcnt_zst =read32(container,off); off+=4;

    uint8_t *d_bmap = zst_decompress(container+off, rbmap_zst, rbmap_orig); off+=rbmap_zst;
    uint8_t *d_ne   = zst_decompress(container+off, rne_zst,   rne_orig);   off+=rne_zst;
    uint8_t *d_dpos = zst_decompress(container+off, rdpos_zst, rdpos_orig); off+=rdpos_zst;
    uint8_t *d_cnt  = zst_decompress(container+off, rcnt_zst,  rcnt_orig);  off+=rcnt_zst;

    uint8_t *recon=calloc(n*3,1);
    int ne_rp=0, dpos_rp=0, cnt_rp=0;
    tile_id=0;

    for(int ty=0;ty<th;ty++){
        for(int tx=0;tx<tw;tx++,tile_id++){
            if(!(d_bmap[tile_id>>3]>>(tile_id&7)&1)) continue;

            uint16_t ne=(uint16_t)(d_ne[ne_rp]|(d_ne[ne_rp+1]<<8)); ne_rp+=2;

            // rebuild freq from delta-pos + count
            int freq[AXIS]={0};
            int cur_pos=0;
            for(int e=0;e<ne;e++){
                uint16_t delta=(uint16_t)(d_dpos[dpos_rp]|(d_dpos[dpos_rp+1]<<8)); dpos_rp+=2;
                cur_pos+=delta;
                freq[cur_pos]=(d_cnt[cnt_rp]|(d_cnt[cnt_rp+1]<<8)); cnt_rp+=2;
            }

            // per-channel value queue (sorted, since pos is monotone)
            uint8_t cval[3][256]; int ccnt[3]={0,0,0};
            for(int p=0;p<AXIS;p++){
                if(!freq[p]) continue;
                int ch=p/256, val=p%256;
                for(int k=0;k<freq[p]&&ccnt[ch]<256;k++)
                    cval[ch][ccnt[ch]++]=(uint8_t)val;
            }

            int ci[3]={0,0,0};
            for(int dy=0;dy<TILE;dy++){
                int py=ty*TILE+dy; if(py>=h) continue;
                for(int dx=0;dx<TILE;dx++){
                    int px2=tx*TILE+dx; if(px2>=w) continue;
                    int i=py*w+px2;
                    for(int c=0;c<3;c++)
                        recon[i*3+c]= ci[c]<ccnt[c] ? cval[c][ci[c]++] : 0;
                }
            }
        }
    }

    // verify histogram
    int hist_ok=1;
    {
        int fO[3][256]={}, fR[3][256]={};
        for(int i=0;i<n;i++) for(int c=0;c<3;c++){
            fO[c][img.px[i*3+c]]++;
            fR[c][recon[i*3+c]]++;
        }
        for(int c=0;c<3;c++) for(int v=0;v<256;v++)
            if(fO[c][v]!=fR[c][v]){hist_ok=0;}
    }

    printf("Image : %s  (%dx%d)  raw=%d B\n\n", path,w,h,raw);
    printf("=== Stream sizes (before/after zstd-19) ===\n");
    printf("  bitmap  : %6d → %6d B\n", bmap.sz,  zbmap_sz);
    printf("  n_entries:%6d → %6d B  ent=%.3f\n", s_ne.sz,   zne_sz,   entropy8(s_ne.buf,s_ne.sz));
    printf("  delta-pos:%6d → %6d B  ent=%.3f\n", s_dpos.sz, zdpos_sz, entropy8(s_dpos.buf,s_dpos.sz));
    printf("  count    :%6d → %6d B  ent=%.3f\n", s_cnt.sz,  zcnt_sz,  entropy8(s_cnt.buf,s_cnt.sz));
    printf("\n=== Results ===\n");
    printf("  Raw              : %7d B  (1.00x)\n", raw);
    printf("  Encoded (GEO2)   : %7d B  (%.2fx)\n", enc_total, (float)raw/enc_total);
    printf("  Histogram verify : %s\n", hist_ok?"PASS":"FAIL");
    printf("  PSNR             : %.2f dB\n", psnr(img.px,recon,n*3));

    // save .geo2 file
    char out_path[256]; snprintf(out_path,sizeof(out_path),"%s.geo2",path);
    FILE *fo=fopen(out_path,"wb"); fwrite(container,1,enc_total,fo); fclose(fo);
    printf("  Saved            : %s\n", out_path);

    free(container); free(d_bmap); free(d_ne); free(d_dpos); free(d_cnt);
    free(zne); free(zdpos); free(zcnt); free(zbmap);
    free(recon); free(img.px);
    return 0;
}
