// hex_tile_enc.c — encode always 8 bytes (uniform), decode roundtrip test
// uniform 8-byte layout: [type(1), res0..res6(7)] where res6 encodes pred for non-FLAT
// FLAT stays 2 bytes as special case

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#define HEX_CELLS  7
#define HENC_FLAT         0x00
#define HENC_TRIPLET_FLAT 0x01
#define HENC_GRADIENT     0x02
#define HENC_EDGE         0x03

static const uint8_t TRIPLETS[6][3] = {
    {0,1,6},{1,2,6},{2,3,6},{3,4,6},{4,5,6},{5,0,6}
};

typedef struct { uint8_t c[HEX_CELLS]; } HexTile;

static int triplet_flat(const HexTile *t, int i) {
    return t->c[TRIPLETS[i][0]]==t->c[TRIPLETS[i][1]] &&
           t->c[TRIPLETS[i][1]]==t->c[TRIPLETS[i][2]];
}

static uint8_t predict_center(const HexTile *t) {
    for (int i=0;i<6;i++)
        if (triplet_flat(t,i)) return t->c[TRIPLETS[i][0]];
    uint8_t r[6]; for(int i=0;i<6;i++) r[i]=t->c[i];
    for(int i=0;i<5;i++) for(int j=i+1;j<6;j++)
        if(r[j]<r[i]){uint8_t x=r[i];r[i]=r[j];r[j]=x;}
    return (r[2]+r[3])>>1;
}

static uint8_t classify(const HexTile *t) {
    int same=1;
    for(int i=1;i<HEX_CELLS;i++) if(t->c[i]!=t->c[0]){same=0;break;}
    if(same) return HENC_FLAT;
    for(int i=0;i<6;i++) if(triplet_flat(t,i)) return HENC_TRIPLET_FLAT;
    uint8_t mn=255,mx=0;
    for(int i=0;i<HEX_CELLS;i++){if(t->c[i]<mn)mn=t->c[i];if(t->c[i]>mx)mx=t->c[i];}
    return (mx-mn>32)?HENC_EDGE:HENC_GRADIENT;
}

// encode → dst[8], returns 2 (FLAT) or 8
int hex_encode(const HexTile *t, uint8_t *dst) {
    uint8_t type = classify(t);
    dst[0] = type;
    if (type==HENC_FLAT) { dst[1]=t->c[0]; return 2; }
    uint8_t pred = predict_center(t);
    for(int i=0;i<HEX_CELLS;i++) dst[1+i]=(uint8_t)((t->c[i]-pred+128)&0xFF);
    // store pred in last byte position (overwrite res[6] with raw pred)
    // decoder recovers: c[i] = dst[1+i] - 128 + pred; center = pred itself
    // so res[6] = (pred - pred + 128) = 128 always → use byte 7 as pred store
    // layout: [type, res0..res5, pred] — 8 bytes total
    dst[7] = pred;
    return 8;
}

// decode → fills t, returns bytes consumed
int hex_decode(const uint8_t *src, HexTile *t) {
    if(src[0]==HENC_FLAT){
        for(int i=0;i<HEX_CELLS;i++) t->c[i]=src[1];
        return 2;
    }
    uint8_t pred=src[7];
    for(int i=0;i<6;i++) t->c[i]=(uint8_t)((src[1+i]-128+pred)&0xFF);
    t->c[6] = (src[0]==HENC_TRIPLET_FLAT) ? pred
            : (uint8_t)((src[7]-128+pred)&0xFF); // for GRAD/EDGE center is res[6]
    return 8;
}

// ── fix decode center for GRAD/EDGE ──
// re-encode with correct layout: [type, res0..res6] res6=(center-pred+128), then pred appended
// total 9 bytes for GRAD/EDGE — or keep 8 by dropping res6 and using pred as center approx
// Decision: 8 bytes, center decoded = pred (acceptable for predictor use)
// For exact roundtrip: use 9 bytes. Let's do exact.

int hex_encode_exact(const HexTile *t, uint8_t *dst) {
    uint8_t type=classify(t);
    dst[0]=type;
    if(type==HENC_FLAT){dst[1]=t->c[0];return 2;}
    uint8_t pred=predict_center(t);
    dst[1]=pred; // store pred at byte 1
    for(int i=0;i<HEX_CELLS;i++) dst[2+i]=(uint8_t)((t->c[i]-pred+128)&0xFF);
    return 9; // [type,pred,res0..res6]
}

int hex_decode_exact(const uint8_t *src, HexTile *t) {
    if(src[0]==HENC_FLAT){
        for(int i=0;i<HEX_CELLS;i++) t->c[i]=src[1];
        return 2;
    }
    uint8_t pred=src[1];
    for(int i=0;i<HEX_CELLS;i++) t->c[i]=(uint8_t)((src[2+i]-128+pred)&0xFF);
    return 9;
}

// ── test ──────────────────────────────────────────────────────
static void test(const char *name, HexTile *t) {
    uint8_t buf[16];
    HexTile out;
    int n = hex_encode_exact(t, buf);
    hex_decode_exact(buf, &out);
    int ok = memcmp(t->c, out.c, HEX_CELLS)==0;
    printf("[%s] type=%d bytes=%d %s | ", name, buf[0], n, ok?"OK":"FAIL");
    printf("in:["); for(int i=0;i<7;i++) printf("%3d",t->c[i]); printf("] ");
    printf("out:["); for(int i=0;i<7;i++) printf("%3d",out.c[i]); printf("]\n");
}

int main(void) {
    HexTile flat     = {{100,100,100,100,100,100,100}};
    HexTile tflat    = {{200,200, 50, 50, 50, 50,200}}; // triplet 0,1,6 = 200
    HexTile gradient = {{ 10, 20, 30, 40, 50, 60, 35}};
    HexTile edge     = {{  5, 10,  8,200,210,205,100}};
    HexTile random_t = {{ 42,187, 63,220, 11,155, 88}};

    test("flat    ", &flat);
    test("tri_flat", &tflat);
    test("gradient", &gradient);
    test("edge    ", &edge);
    test("random  ", &random_t);
    return 0;
}
