/*
 * geo_goldberg_lut.h — AUTO-GENERATED — GP(1,1) Baked Tables
 * DO NOT EDIT — regenerate with bake_goldberg.py
 *
 * GP(1,1): V=60, E=90, F=32 (12 pen + 20 hex)
 * Triangle gaps = 60 = I-symmetry group order
 * Pentagon pairs = 6 = GEO_SPOKES (bipolar circuits)
 */
#ifndef GEO_GOLDBERG_LUT_H
#define GEO_GOLDBERG_LUT_H

#include <stdint.h>

/* ── pentagon opposite pairs [pair_id][0=pos, 1=neg] ── */
/* face 0..11 = pentagons, 12..31 = hexagons */
static const uint8_t GB_PEN_PAIRS[6][2] = {
    { 0,  3},  /* pair 0 */
    { 1,  2},  /* pair 1 */
    { 4,  7},  /* pair 2 */
    { 5, 10},  /* pair 3 */
    { 6,  8},  /* pair 4 */
    { 9, 11},  /* pair 5 */
};

/* ── triangle gap adjacency [gap_id][face_a, face_b, face_c] ── */
/* 60 gaps, each surrounded by 3 faces */
static const uint8_t GB_TRIGAP_ADJ[60][3] = {
    { 0, 12, 13},  /* gap  0 */
    { 1, 14, 15},  /* gap  1 */
    { 2, 16, 17},  /* gap  2 */
    { 3, 16, 18},  /* gap  3 */
    { 4, 12, 19},  /* gap  4 */
    { 4, 19, 20},  /* gap  5 */
    { 5, 21, 22},  /* gap  6 */
    { 4, 17, 20},  /* gap  7 */
    { 6, 15, 23},  /* gap  8 */
    { 5, 18, 21},  /* gap  9 */
    { 3, 21, 23},  /* gap 10 */
    { 0, 12, 19},  /* gap 11 */
    { 5, 22, 24},  /* gap 12 */
    { 7, 21, 22},  /* gap 13 */
    { 7, 21, 23},  /* gap 14 */
    { 8, 12, 25},  /* gap 15 */
    { 9, 22, 24},  /* gap 16 */
    { 1, 26, 27},  /* gap 17 */
    { 7, 22, 27},  /* gap 18 */
    {10, 19, 28},  /* gap 19 */
    {11, 20, 29},  /* gap 20 */
    { 0, 26, 28},  /* gap 21 */
    { 4, 12, 25},  /* gap 22 */
    {10, 20, 29},  /* gap 23 */
    { 2, 25, 30},  /* gap 24 */
    { 7, 15, 27},  /* gap 25 */
    { 3, 18, 21},  /* gap 26 */
    { 3, 23, 31},  /* gap 27 */
    { 6, 23, 31},  /* gap 28 */
    {10, 14, 28},  /* gap 29 */
    { 0, 13, 26},  /* gap 30 */
    { 8, 24, 30},  /* gap 31 */
    { 8, 13, 24},  /* gap 32 */
    { 6, 14, 15},  /* gap 33 */
    { 9, 13, 24},  /* gap 34 */
    {11, 16, 17},  /* gap 35 */
    { 1, 26, 28},  /* gap 36 */
    {10, 14, 29},  /* gap 37 */
    { 2, 18, 30},  /* gap 38 */
    { 1, 14, 28},  /* gap 39 */
    {10, 19, 20},  /* gap 40 */
    { 9, 13, 26},  /* gap 41 */
    { 2, 17, 25},  /* gap 42 */
    { 2, 16, 18},  /* gap 43 */
    {11, 16, 31},  /* gap 44 */
    {11, 17, 20},  /* gap 45 */
    { 8, 25, 30},  /* gap 46 */
    { 9, 22, 27},  /* gap 47 */
    { 1, 15, 27},  /* gap 48 */
    { 3, 16, 31},  /* gap 49 */
    { 6, 14, 29},  /* gap 50 */
    { 6, 29, 31},  /* gap 51 */
    { 5, 18, 30},  /* gap 52 */
    { 8, 12, 13},  /* gap 53 */
    { 7, 15, 23},  /* gap 54 */
    { 0, 19, 28},  /* gap 55 */
    { 9, 26, 27},  /* gap 56 */
    { 5, 24, 30},  /* gap 57 */
    { 4, 17, 25},  /* gap 58 */
    {11, 29, 31},  /* gap 59 */
};

/* ── face type lookup: 0=pentagon, 1=hexagon ── */
static const uint8_t GB_FACE_TYPE[32] = {
    0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
};

/* ── pentagon face_id → pair_id ── */
static const uint8_t GB_PEN_TO_PAIR[12] = {
    0, 1, 1, 0, 2, 3, 4, 2, 4, 5, 3, 5,
};

/* ── pentagon face_id → pole (0=positive, 1=negative) ── */
static const uint8_t GB_PEN_POLE[12] = {
    0, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 1,
};

#endif /* GEO_GOLDBERG_LUT_H */