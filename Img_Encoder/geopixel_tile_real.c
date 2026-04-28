#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// GEOPIXEL TILE ENCODER — real encode/decode
// 1D axis 0..767: R=0..255, G=256..511, B=512..767 (implicit)
// 16x16 tile → per tile: bitmap(1b) + axis histogram + adaptive offset
//
// Tile stream format:
//   [tile_bitmap: ceil(nt/8) B]
//   per active tile:
//     [n_entries: 1B]  number of unique axis positions in tile
//     per entry:
//       [pos_hi: 2b | count: 6b] packed in 1B  → pos 0..767 needs 10 bits
//       so: [pos: 2B uint16] [count: 1B]        → 3B per entry
//       + adaptive offset: if pixel not in any entry → [offset: 1B int8]

#define TILE 16
#define AXIS 768

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

double ent_u16(const uint16_t*b,int n){
    // entropy over 0..767
    int f[AXIS]={0};
    for(int i=0;i<n;i++) if(b[i]<AXIS) f[b[i]]++;
    double e=0;
    for(int i=0;i<AXIS;i++){
        if(!f[i]) continue;
        double p=(double)f[i]/n;
        e-=p*(log(p)/log(2.0));
    }
    return e;
}
double ent8(const uint8_t*b,int n){
    int f[256]={0}; for(int i=0;i<n;i++) f[b[i]]++;
    double e=0; for(int i=0;i<256;i++){
        if(!f[i]) continue; double p=(double)f[i]/n;
        e-=p*(log(p)/log(2.0));}
    return e;
}
double psnr(const uint8_t*a,const uint8_t*b,int n){
    long s=0; for(int i=0;i<n;i++){int d=(int)a[i]-(int)b[i];s+=d*d;}
    double mse=(double)s/n;
    return mse==0?999.0:10.0*log10(255.0*255.0/mse);
}

// output buffer helper
typedef struct { uint8_t *buf; int sz, cap; } Buf;
void buf_push(Buf*b, uint8_t v){
    if(b->sz==b->cap){b->cap*=2;b->buf=realloc(b->buf,b->cap);}
    b->buf[b->sz++]=v;
}
void buf_push16(Buf*b, uint16_t v){
    buf_push(b,(uint8_t)(v&0xFF));
    buf_push(b,(uint8_t)(v>>8));
}

int main(int argc, char **argv){
    const char *path=argc>1?argv[1]:"test02.bmp";
    Img img; if(!bmp_load(path,&img)){fprintf(stderr,"load failed\n");return 1;}
    int w=img.w,h=img.h,n=w*h,raw=n*3;
    int tw=(w+TILE-1)/TILE, th=(h+TILE-1)/TILE, nt=tw*th;

    // === ENCODE ===
    int bitmap_bytes=(nt+7)/8;
    Buf out; out.cap=1<<20; out.sz=0; out.buf=malloc(out.cap);

    // reserve space for tile bitmap
    int bmap_off=out.sz;
    for(int i=0;i<bitmap_bytes;i++) buf_push(&out,0);

    // per-tile encode
    long total_entries=0, total_offsets=0, total_offset_bytes=0;
    int tile_id=0;
    for(int ty=0;ty<th;ty++){
        for(int tx=0;tx<tw;tx++,tile_id++){
            // collect all axis positions in this tile
            int freq[AXIS]={0};
            int tile_n=0;
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
            out.buf[bmap_off+(tile_id>>3)] |= (1<<(tile_id&7));

            // count unique positions
            int n_entries=0;
            for(int p=0;p<AXIS;p++) if(freq[p]) n_entries++;
            total_entries+=n_entries;

            // write n_entries (uint16 to be safe)
            buf_push16(&out,(uint16_t)n_entries);

            // write entries: pos(uint16) + count(uint8)
            for(int p=0;p<AXIS;p++){
                if(!freq[p]) continue;
                buf_push16(&out,(uint16_t)p);
                buf_push(&out,(uint8_t)(freq[p]>255?255:freq[p]));
            }

            // adaptive offset: pixels whose exact axis pos isn't perfectly
            // reconstructible from count alone need a residual
            // For now: count stream encodes frequency, not order
            // offset stream = per-pixel delta from tile median axis pos
            // find dominant axis pos per channel in this tile
            int dom[3]={0,0,0};
            for(int c=0;c<3;c++){
                int best=-1,bfreq=0;
                for(int v=0;v<256;v++){
                    if(freq[c*256+v]>bfreq){bfreq=freq[c*256+v];best=v;}
                }
                dom[c]=best;
            }
            // per-pixel offset from dominant
            for(int dy=0;dy<TILE;dy++){
                int py=ty*TILE+dy; if(py>=h) continue;
                for(int dx=0;dx<TILE;dx++){
                    int px2=tx*TILE+dx; if(px2>=w) continue;
                    int i=py*w+px2;
                    for(int c=0;c<3;c++){
                        int8_t off=(int8_t)((int)img.px[i*3+c]-(int)dom[c]);
                        buf_push(&out,(uint8_t)off);
                        if(off!=0) total_offsets++;
                        total_offset_bytes++;
                    }
                }
            }
        }
    }

    // === DECODE ===
    uint8_t *recon=calloc(n*3,1);
    tile_id=0;
    int rp=bitmap_bytes; // read pointer past bitmap
    for(int ty=0;ty<th;ty++){
        for(int tx=0;tx<tw;tx++,tile_id++){
            if(!(out.buf[bmap_off+(tile_id>>3)]>>(tile_id&7)&1)) continue;

            // read n_entries
            uint16_t n_ent=out.buf[rp]|(out.buf[rp+1]<<8); rp+=2;

            // read entries into freq table
            int freq[AXIS]={0};
            for(int e=0;e<n_ent;e++){
                uint16_t pos=out.buf[rp]|(out.buf[rp+1]<<8); rp+=2;
                freq[pos]=out.buf[rp++];
            }

            // find dominant per channel
            int dom[3]={0,0,0};
            for(int c=0;c<3;c++){
                int best=-1,bfreq=0;
                for(int v=0;v<256;v++){
                    if(freq[c*256+v]>bfreq){bfreq=freq[c*256+v];best=v;}
                }
                dom[c]=best<0?0:best;
            }

            // read per-pixel offsets
            for(int dy=0;dy<TILE;dy++){
                int py=ty*TILE+dy; if(py>=h) continue;
                for(int dx=0;dx<TILE;dx++){
                    int px2=tx*TILE+dx; if(px2>=w) continue;
                    int i=py*w+px2;
                    for(int c=0;c<3;c++){
                        int8_t off=(int8_t)out.buf[rp++];
                        recon[i*3+c]=(uint8_t)(dom[c]+off);
                    }
                }
            }
        }
    }

    // verify
    int ok=1;
    for(int i=0;i<n*3;i++) if(recon[i]!=img.px[i]){ok=0;break;}

    // entropy of offset stream (last section of output)
    // estimate: offset portion starts after entries
    double off_ent = ent8(out.buf+out.sz-total_offset_bytes, total_offset_bytes);
    long theo_offs = (long)(total_offset_bytes * off_ent / 8.0);
    long entry_bytes = out.sz - bitmap_bytes - total_offset_bytes;
    long theo_total = bitmap_bytes + entry_bytes + theo_offs;

    printf("Image: %s (%dx%d)\n\n",path,w,h);
    printf("=== Tile Encode ===\n");
    printf("  Tiles           : %d  (%dx%d)\n", nt, tw, th);
    printf("  Bitmap overhead : %d B\n", bitmap_bytes);
    printf("  Entry stream    : %ld B  (pos+count per unique axis val)\n", entry_bytes);
    printf("  Offset stream   : %d B raw  →  %.3fb entropy  →  %ld B theo\n",
           total_offset_bytes, off_ent, theo_offs);
    printf("  Non-zero offsets: %.1f%%\n", 100.0*total_offsets/total_offset_bytes);

    printf("\n=== Results ===\n");
    printf("  Raw             : %7d B  (1.00x)\n", raw);
    printf("  Encoded (raw)   : %7d B  (%.2fx)\n", out.sz, (float)raw/out.sz);
    printf("  Theoretical     : %7ld B  (%.2fx)\n", theo_total, (float)raw/theo_total);
    printf("  Verify          : %s\n", ok?"PASS (lossless)":"FAIL");
    printf("  PSNR            : %.2f dB\n", psnr(img.px,recon,n*3));

    free(out.buf); free(recon); free(img.px);
    return 0;
}
