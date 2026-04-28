#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// GEOPIXEL TILE ENCODER
// 1D axis 0..767: all 3 channels flattened as one line
//   value 0-255 = R, 256-511 = G, 512-767 = B  (implicit, no header)
// 16x16 tile grid → sparse bitmap (1 bit per tile)
// Each active tile stores: [axis_pos(2B)] pairs only
// Pair = same value appears in 2+ channels → store once, reference by channel offset (×256)

#define TILE  16
#define AXIS  768   // 256 × 3 channels

typedef struct { int w,h; uint8_t *px; } Img;
int bmp_load(const char *path, Img *img) {
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

double ent(const uint8_t*b,int n){
    int f[256]={0}; for(int i=0;i<n;i++) f[b[i]]++;
    double e=0; for(int i=0;i<256;i++){
        if(!f[i]) continue; double p=(double)f[i]/n;
        e-=p*(log(p)/log(2.0));}
    return e;
}
double psnr(const uint8_t*a,const uint8_t*b,int n){
    long s=0; for(int i=0;i<n;i++){int d=a[i]-b[i];s+=d*d;}
    double mse=(double)s/n;
    return mse==0?999.0:10.0*log10(255.0*255.0/mse);
}

// Per-tile: histogram of axis positions (0..767)
// axis_pos = ch*256 + value
// pair detection: same base value across channels → count multiplicity
typedef struct {
    uint16_t pos;   // axis position 0..767
    uint8_t  count; // how many pixels at this pos in tile (1..TILE*TILE)
    int8_t   offset;// adaptive residual (for non-pair pixels, delta from nearest pair)
} TileEntry;

int main(int argc, char **argv){
    const char *path = argc>1?argv[1]:"test02.bmp";
    Img img; if(!bmp_load(path,&img)){fprintf(stderr,"load failed\n");return 1;}
    int w=img.w, h=img.h, n=w*h, raw=n*3;
    int tw=(w+TILE-1)/TILE, th=(h+TILE-1)/TILE, nt=tw*th;
    printf("Image: %s (%dx%d)  tiles=%dx%d (%d total)\n\n",path,w,h,tw,th,nt);

    // flatten image to 1D axis stream: axis[pixel*3+ch] = ch*256 + value
    // but we work per-tile, so process tile by tile
    int bitmap_bytes = (nt+7)/8;
    uint8_t *bitmap = calloc(bitmap_bytes, 1);

    // per-tile analysis
    long total_axis_vals = 0;   // unique axis positions across all tiles
    long total_pairs     = 0;   // positions shared by 2+ channels
    long total_triples   = 0;   // positions shared by all 3 channels
    long total_singles   = 0;   // unique per channel only
    long total_pixels    = 0;

    // histogram across all tiles on the 768-axis
    int axis_hist[AXIS] = {0};

    int tile_id = 0;
    for(int ty=0;ty<th;ty++){
        for(int tx=0;tx<tw;tx++, tile_id++){
            // collect axis positions for this tile
            int freq[AXIS] = {0};  // freq[pos] = count of pixels at that axis pos
            int tile_n = 0;
            for(int dy=0;dy<TILE;dy++){
                int py=ty*TILE+dy; if(py>=h) continue;
                for(int dx=0;dx<TILE;dx++){
                    int px2=tx*TILE+dx; if(px2>=w) continue;
                    int i=py*w+px2;
                    int r=img.px[i*3+0], g=img.px[i*3+1], b=img.px[i*3+2];
                    freq[r]++;           // R zone: 0..255
                    freq[256+g]++;       // G zone: 256..511
                    freq[512+b]++;       // B zone: 512..767
                    axis_hist[r]++;
                    axis_hist[256+g]++;
                    axis_hist[512+b]++;
                    tile_n++;
                }
            }
            total_pixels += tile_n;

            // count unique positions and pairs
            int tile_active = 0;
            for(int p=0;p<256;p++){
                int cr=freq[p], cg=freq[256+p], cb=freq[512+p];
                int channels_with_val = (cr>0)+(cg>0)+(cb>0);
                if(channels_with_val>=3) total_triples++;
                else if(channels_with_val==2) total_pairs++;
                else if(channels_with_val==1) total_singles++;
                if(cr||cg||cb) tile_active++;
            }
            total_axis_vals += tile_active;

            // mark tile active in bitmap
            if(tile_n>0) bitmap[tile_id>>3] |= (1<<(tile_id&7));
        }
    }

    // axis histogram entropy
    int axis_total = n*3;
    double axis_ent = 0;
    for(int p=0;p<AXIS;p++){
        if(!axis_hist[p]) continue;
        double pr=(double)axis_hist[p]/axis_total;
        axis_ent -= pr*(log(pr)/log(2.0));
    }

    // theoretical size:
    // bitmap: nt bits
    // per active tile: ~tile_active positions × log2(AXIS)/8 bytes each
    // pairs save: instead of 3 entries store 1 + 2-bit channel mask
    long bitmap_bits = nt;
    // avg unique axis positions per tile
    double avg_unique = (double)total_axis_vals / nt;
    // bits per axis position: log2(768) ≈ 9.58 bits
    double bits_per_pos = log(AXIS)/log(2.0);
    // pair encoding: pair=1pos+2bit_mask vs 2 separate = saves ~7.5 bits per pair
    double pair_save = total_pairs * (bits_per_pos - 2.0);
    double triple_save = total_triples * (bits_per_pos*2 - 2.0);

    long theo_naive  = (long)(bitmap_bits/8) + (long)(total_axis_vals * bits_per_pos/8);
    long theo_paired = (long)(bitmap_bits/8) + (long)((total_axis_vals * bits_per_pos - pair_save - triple_save)/8);

    printf("=== Axis Analysis ===\n");
    printf("  Axis entropy      : %.3f bits  (max=%.3f)\n", axis_ent, log(AXIS)/log(2.0));
    printf("  Avg unique pos/tile: %.1f  (of 256 possible)\n", avg_unique);
    printf("  Singles (1 ch)    : %ld\n", total_singles);
    printf("  Pairs   (2 ch)    : %ld\n", total_pairs);
    printf("  Triples (3 ch)    : %ld  ← encode once, reference ×3\n", total_triples);

    printf("\n=== Size Estimate ===\n");
    printf("  Bitmap            : %d B  (%d tiles)\n", bitmap_bytes, nt);
    printf("  Naive axis stream : %7ld B  (%.2fx)\n", theo_naive,  (float)raw/theo_naive);
    printf("  Pair-encoded      : %7ld B  (%.2fx)\n", theo_paired, (float)raw/theo_paired);
    printf("  Raw               : %7d B  (1.00x)\n", raw);

    // verify bitmap coverage
    int active_tiles=0;
    for(int i=0;i<nt;i++) if(bitmap[i>>3]>>(i&7)&1) active_tiles++;
    printf("\n  Active tiles      : %d / %d (%.1f%%)\n",
           active_tiles, nt, 100.0*active_tiles/nt);

    free(bitmap); free(img.px);
    return 0;
}
