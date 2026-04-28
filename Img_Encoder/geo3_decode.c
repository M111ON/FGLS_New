#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#define TILE 16
#define AXIS 768
#define MAGIC 0x47454F33u

uint8_t* zst_decomp(const uint8_t*src,int sz,int orig){
    uint8_t*dst=malloc(orig);
    if((int)ZSTD_decompress(dst,orig,src,sz)!=orig){free(dst);return NULL;}
    return dst;
}
uint32_t r32(const uint8_t*b,int o){ return b[o]|(b[o+1]<<8)|(b[o+2]<<16)|((uint32_t)b[o+3]<<24); }

int bmp_save(const char*path,const uint8_t*px,int w,int h){
    int rs=(w*3+3)&~3;
    uint8_t hdr[54]={0};
    hdr[0]='B';hdr[1]='M';
    *(uint32_t*)(hdr+2)=54+rs*h; *(uint32_t*)(hdr+10)=54;
    *(uint32_t*)(hdr+14)=40; *(int32_t*)(hdr+18)=w; *(int32_t*)(hdr+22)=h;
    hdr[26]=1; hdr[28]=24;
    FILE*f=fopen(path,"wb"); if(!f) return 0;
    fwrite(hdr,1,54,f);
    uint8_t*row=calloc(rs,1);
    for(int y=h-1;y>=0;y--){
        for(int x=0;x<w;x++){
            int i=(y*w+x)*3;
            row[x*3+0]=px[i+2];
            row[x*3+1]=px[i+1];
            row[x*3+2]=px[i+0];
        }
        fwrite(row,1,rs,f);
    }
    free(row); fclose(f); return 1;
}

int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage: geo3_decode <in.geo3> [out.bmp]\n");return 1;}
    FILE*f=fopen(argv[1],"rb"); if(!f){perror(argv[1]);return 1;}
    fseek(f,0,SEEK_END); long fsz=ftell(f); rewind(f);
    uint8_t*data=malloc(fsz); fread(data,1,fsz,f); fclose(f);

    int off=0;
    if(r32(data,off)!=MAGIC){fprintf(stderr,"not GEO3\n");return 1;} off+=4;
    int w=r32(data,off); off+=4;
    int h=r32(data,off); off+=4;
    int tw=(w+TILE-1)/TILE, th=(h+TILE-1)/TILE;

    int bmap_o=r32(data,off),bmap_z=r32(data,off+4); off+=8;
    int ne_o  =r32(data,off),ne_z  =r32(data,off+4); off+=8;
    int dpos_o=r32(data,off),dpos_z=r32(data,off+4); off+=8;
    int cnt_o =r32(data,off),cnt_z =r32(data,off+4); off+=8;

    uint8_t*bmap =zst_decomp(data+off,bmap_z,bmap_o); off+=bmap_z;
    uint8_t*s_ne =zst_decomp(data+off,ne_z,  ne_o);   off+=ne_z;
    uint8_t*s_dp =zst_decomp(data+off,dpos_z,dpos_o); off+=dpos_z;
    uint8_t*s_cnt=zst_decomp(data+off,cnt_z, cnt_o);
    if(!bmap||!s_ne||!s_dp||!s_cnt){fprintf(stderr,"decompress error\n");return 1;}

    uint8_t*recon=calloc(w*h*3,1);
    int ne_rp=0,dp_rp=0,cnt_rp=0,tile_id=0;

    for(int ty=0;ty<th;ty++){
        for(int tx=0;tx<tw;tx++,tile_id++){
            if(!(bmap[tile_id>>3]>>(tile_id&7)&1)) continue;
            int ne=(s_ne[ne_rp]|(s_ne[ne_rp+1]<<8)); ne_rp+=2;

            int freq[AXIS]={0},pos=0;
            for(int e=0;e<ne;e++){
                uint8_t tag=s_dp[dp_rp++];
                if(tag==0x00){ pos+=s_dp[dp_rp]|(s_dp[dp_rp+1]<<8); dp_rp+=2; }
                else { pos+=tag; }
                uint8_t cb=s_cnt[cnt_rp++];
                int cnt=(cb&0x80)?(((cb&0x7F)<<8)|s_cnt[cnt_rp++]):cb;
                freq[pos]=cnt;
            }

            uint8_t cv[3][256]; int cc[3]={0,0,0};
            for(int p=0;p<AXIS;p++){
                if(!freq[p]) continue;
                int ch=p/256,val=p%256;
                for(int k=0;k<freq[p]&&cc[ch]<256;k++)
                    cv[ch][cc[ch]++]=(uint8_t)val;
            }
            int ci[3]={0,0,0};
            for(int dy=0;dy<TILE;dy++){
                int py=ty*TILE+dy; if(py>=h) continue;
                for(int dx=0;dx<TILE;dx++){
                    int px2=tx*TILE+dx; if(px2>=w) continue;
                    int i=(py*w+px2)*3;
                    for(int c=0;c<3;c++)
                        recon[i+c]= ci[c]<cc[c]?cv[c][ci[c]++]:0;
                }
            }
        }
    }

    char out[512];
    if(argc>2) snprintf(out,sizeof(out),"%s",argv[2]);
    else {
        snprintf(out,sizeof(out),"%s",argv[1]);
        char*e=strstr(out,".geo3"); if(e) strcpy(e,"_dec.bmp"); else strcat(out,"_dec.bmp");
    }
    if(!bmp_save(out,recon,w,h)){fprintf(stderr,"save failed\n");return 1;}
    printf("OK  %s → %s  (%dx%d)\n",argv[1],out,w,h);
    free(data); free(bmap); free(s_ne); free(s_dp); free(s_cnt); free(recon);
    return 0;
}
