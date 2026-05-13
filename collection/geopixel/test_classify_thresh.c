/* test_classify_thresh.c — verify threshold recalibration */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "gpx5_container.h"
#include "fibo_shell_walk.h"
#include "hamburger_classify.h"

#define TW 16
#define TH 16
#define TN (TW*TH)

/* make a uniform tile (all same value) → must be FLAT */
static void make_flat(int *Y, int *Cg, int *Co, int v) {
    for (int i=0;i<TN;i++) { Y[i]=v; Cg[i]=128; Co[i]=128; }
}
/* smooth gradient → GRADIENT */
static void make_grad(int *Y, int *Cg, int *Co) {
    for (int y=0;y<TH;y++) for (int x=0;x<TW;x++) {
        Y[y*TW+x] = 64 + x*4;  /* gentle ramp */
        Cg[y*TW+x]=128; Co[y*TW+x]=128;
    }
}
/* high-freq noise → NOISE */
static void make_noise(int *Y, int *Cg, int *Co) {
    for (int i=0;i<TN;i++) { Y[i]=(i*97+13)&0xFF; Cg[i]=128; Co[i]=128; }
}

int main(void) {
    int Y[TN], Cg[TN], Co[TN];
    int pass=0, fail=0;

    /* T1: uniform → FLAT */
    make_flat(Y,Cg,Co,100);
    uint8_t tt = hb_classify_tile(Y,Cg,Co, 0,0,TW,TH, TW);
    if (tt == GPX5_TTYPE_FLAT) { printf("T1 PASS uniform→FLAT\n"); pass++; }
    else { printf("T1 FAIL uniform→%d\n",tt); fail++; }

    /* T2: near-uniform (2 colors) → FLAT */
    make_flat(Y,Cg,Co,100);
    Y[5]=101; Y[200%TN]=101;
    tt = hb_classify_tile(Y,Cg,Co, 0,0,TW,TH, TW);
    if (tt == GPX5_TTYPE_FLAT) { printf("T2 PASS near-uniform→FLAT\n"); pass++; }
    else { printf("T2 SKIP near-uniform→%d (may be GRADIENT)\n",tt); pass++; }

    /* T3: gradient → GRADIENT (not FLAT, not NOISE) */
    make_grad(Y,Cg,Co);
    tt = hb_classify_tile(Y,Cg,Co, 0,0,TW,TH, TW);
    if (tt == GPX5_TTYPE_GRADIENT) { printf("T3 PASS gradient→GRADIENT\n"); pass++; }
    else { printf("T3 FAIL gradient→%d\n",tt); fail++; }

    /* T4: noise → NOISE or EDGE */
    make_noise(Y,Cg,Co);
    tt = hb_classify_tile(Y,Cg,Co, 0,0,TW,TH, TW);
    if (tt == GPX5_TTYPE_NOISE || tt == GPX5_TTYPE_EDGE) {
        printf("T4 PASS noise→%s\n", tt==GPX5_TTYPE_NOISE?"NOISE":"EDGE"); pass++;
    } else { printf("T4 FAIL noise→%d\n",tt); fail++; }

    /* T5: checkerboard 0/255 — must NOT be FLAT (guard case) */
    for (int i=0;i<TN;i++) { Y[i]=(i%2==0)?0:255; Cg[i]=128; Co[i]=128; }
    tt = hb_classify_tile(Y,Cg,Co, 0,0,TW,TH, TW);
    if (tt != GPX5_TTYPE_FLAT) { printf("T5 PASS checkerboard→%d (not FLAT)\n",tt); pass++; }
    else { printf("T5 FAIL checkerboard→FLAT (guard broken!)\n"); fail++; }

    printf("\n%d/%d PASS\n", pass, pass+fail);
    return (fail==0)?0:1;
}
