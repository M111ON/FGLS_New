/*
 * gen_pent_o22.c — O22: GPX4_LAYER_GEO integration
 * ═══════════════════════════════════════════════════
 * Adds "GEOA" layer (GPX4_LAYER_GEO = 0x06) to gpx4_container.h
 * Stores O21 TileAddr.packed for all tiles as independent GEO layer
 *
 * Test:
 *   1. Synthesise TileAddr[256] with O21-equivalent geometry
 *   2. Write GPX4 file with GEOA layer (+ dummy O4 layer)
 *   3. Read back, verify every packed value round-trips correctly
 *   4. Verify GEO layer is NOT in tile_table (untiled)
 *   5. Print pent distribution from recovered addrs
 *
 * Build: gcc -O2 -o gen_pent_o22 gen_pent_o22.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* ── embed the updated container header inline ── */
#include "gpx4_container_o22.h"

#define W    512
#define H    512
#define TILE  32
#define TX   (W/TILE)   /* 16 */
#define TY   (H/TILE)   /* 16 */
#define NT   (TX*TY)    /* 256 */

#define PI  3.14159265358979323846
#define PHI 1.61803398874989484820

/* ── minimal icosa pentagon assignment (mirrors O21 logic) ─── */
typedef struct{double x,y,z;}V3;
static V3 v3n(V3 v){double r=sqrt(v.x*v.x+v.y*v.y+v.z*v.z);if(r<1e-12)return v;return(V3){v.x/r,v.y/r,v.z/r};}
static double v3d(V3 a,V3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}

static V3 ico_v[12];
static void build_ico(void){
    double t=1.0/PHI;
    V3 raw[12]={{-1,t,0},{1,t,0},{-1,-t,0},{1,-t,0},
                {0,-1,t},{0,1,t},{0,-1,-t},{0,1,-t},
                {t,0,-1},{t,0,1},{-t,0,-1},{-t,0,1}};
    for(int i=0;i<12;i++) ico_v[i]=v3n(raw[i]);
}

static int nearest_pent(V3 s){
    int best=0; double bd=-2;
    for(int i=0;i<12;i++){double d=v3d(s,ico_v[i]);if(d>bd){bd=d;best=i;}}
    return best;
}

static uint32_t hilbert_xy2d(uint32_t n,uint32_t x,uint32_t y){
    uint32_t d=0;
    for(uint32_t s=n/2;s>0;s>>=1){
        uint32_t rx=(x&s)?1:0,ry=(y&s)?1:0;
        d+=(uint32_t)s*s*((3u*rx)^ry);
        /* rotate */
        if(ry==0){if(rx==1){x=(uint32_t)(s-1)-x;y=(uint32_t)(s-1)-y;}uint32_t tmp=x;x=y;y=tmp;}
    }
    return d;
}
#define HILBERT_N 16

/* synthesise TileAddr packed values */
static void build_geo_addrs(Gpx4GeoAddr *addrs){
    build_ico();
    int idx=0;
    for(int ty=0;ty<TY;ty++){
        for(int tx=0;tx<TX;tx++){
            double cx=((tx+0.5)*TILE-W*0.5)/(W*0.5);
            double cy=((ty+0.5)*TILE-H*0.5)/(H*0.5);
            double r2=cx*cx+cy*cy;
            V3 s;
            if(r2>=1.0){ s=(V3){0,0,1}; }
            else { double z=sqrt(1.0-r2); s=v3n((V3){cx,cy,z}); }
            int pent=nearest_pent(s);
            uint32_t hd=hilbert_xy2d(HILBERT_N,(uint32_t)tx,(uint32_t)ty);
            uint32_t packed=((uint32_t)pent<<28)|((hd&0x3FFF)<<14);
            addrs[idx++].packed=packed;
        }
    }
}

int main(void){
    printf("O22: GPX4_LAYER_GEO round-trip test\n");
    printf("────────────────────────────────────\n");

    /* 1. Build synthetic TileAddr */
    Gpx4GeoAddr addrs[NT];
    build_geo_addrs(addrs);
    printf("[1] Built %d TileAddr entries\n", NT);

    /* 2. Serialise GEO layer data */
    uint8_t geo_buf[NT * GPX4_GEO_ADDR_SZ];
    gpx4_geo_write_layer_data(addrs, NT, geo_buf);

    /* 3. Dummy O4 layer data (just zeros, 1 byte) */
    uint8_t dummy_o4[1]={0};
    Gpx4TileEntry tiles[NT];
    memset(tiles, 0, sizeof(tiles));

    /* 4. Build layer list */
    Gpx4LayerDef layers[2];
    /* O4 layer */
    layers[0].type=GPX4_LAYER_O4; layers[0].lflags=GPX4_LFLAG_KEYFRAME;
    memcpy(layers[0].name,"F000",4);
    layers[0].data=dummy_o4; layers[0].size=1;
    layers[0].tiles=tiles;
    /* GEO layer — untiled (tiles=NULL) */
    layers[1].type=GPX4_LAYER_GEO; layers[1].lflags=0;
    memcpy(layers[1].name,GPX4_GEO_NAME,4);
    layers[1].data=geo_buf; layers[1].size=NT*GPX4_GEO_ADDR_SZ;
    layers[1].tiles=NULL;

    const char *path="/tmp/test_o22.gpx4";
    int rc=gpx4_write(path,(uint16_t)TX,(uint16_t)TY,layers,2);
    if(rc){ fprintf(stderr,"FAIL: gpx4_write returned %d\n",rc); return 1; }
    printf("[2] Wrote %s (%d bytes GEO layer)\n", path, NT*GPX4_GEO_ADDR_SZ);

    /* 5. Read back */
    Gpx4File gf;
    if(gpx4_open(path,&gf)){ fprintf(stderr,"FAIL: gpx4_open\n"); return 1; }
    printf("[3] Opened: n_layers=%d tw=%d th=%d n_tiles=%d\n",
           gf.n_layers, gf.tw, gf.th, gf.n_tiles);

    /* 6. Verify GEO layer not in tile_table */
    int geo_idx=gpx4_layer_index(&gf,GPX4_GEO_NAME);
    if(geo_idx<0){ fprintf(stderr,"FAIL: GEOA layer not found\n"); return 1; }
    if(gf.tile_tables[geo_idx]!=NULL){
        fprintf(stderr,"FAIL: GEO layer should have NULL tile_table\n"); return 1;
    }
    printf("[4] GEOA layer idx=%d tile_table=NULL  PASS\n", geo_idx);

    /* 7. Round-trip check */
    Gpx4GeoAddr *recovered = gpx4_geo_open(&gf);
    if(!recovered){ fprintf(stderr,"FAIL: gpx4_geo_open returned NULL\n"); return 1; }

    int errors=0;
    for(int i=0;i<NT;i++){
        if(recovered[i].packed != addrs[i].packed){
            fprintf(stderr,"  MISMATCH tile %d: wrote %08X got %08X\n",
                    i, addrs[i].packed, recovered[i].packed);
            errors++;
        }
    }
    if(errors) fprintf(stderr,"FAIL: %d mismatches\n", errors);
    else printf("[5] All %d packed values round-trip  PASS\n", NT);

    /* 8. Pentagon distribution from recovered */
    int pent_cnt[12]={0};
    for(int i=0;i<NT;i++) pent_cnt[GPX4_GEO_PENT(recovered[i].packed)]++;
    printf("[6] Pentagon distribution:\n");
    for(int p=0;p<12;p++)
        printf("    pent %2d : %3d tiles\n", p, pent_cnt[p]);

    free(recovered);
    gpx4_close(&gf);

    printf("────────────────────────────────────\n");
    printf("O22 %s\n", errors==0?"PASS":"FAIL");
    return errors?1:0;
}
