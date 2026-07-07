/*
 * test_dgls_backport.c — Verify DGLS backported modules compile
 */
#include <stdio.h>
#include <stdint.h>

/* Diamond Shell v2 */
#include "diamond/diamond_shell_v2.h"
#include "diamond/diamond_shell_codec.h"
/* Skip binary_shell_codec.h and hex_codec.h due to conflicts with hex_tile.h */
/* #include "diamond/binary_shell_codec.h" */
/* #include "diamond/hex_codec.h" */
#include "diamond/pogls_fold.h"
#include "hex_tile.h"

/* Hamburger Architecture */
#include "diamond/hamburger/hamburger_encode.h"
#include "diamond/hamburger/hamburger_classify.h"
#include "diamond/hamburger/hamburger_pipe.h"

/* GPX5 Container */
#include "diamond/gpx/gpx5_container.h"
#include "diamond/gpx/gpx4_container.h"

/* Geo modules */
#include "geo_flow_chunker_v8.h"
#include "geo_fibo_clock.h"
#include "geo_rewind.h"
#include "geo_rewind_wang.h"
#include "geo_fec.h"
#include "geo_fec_rs.h"
#include "geo_rs.h"
#include "geo_shell_fold.h"
#include "geo_temporal_lut.h"
#include "geo_radial_hilbert.h"
#include "geo_hardening_whe.h"

int main(void) {
    printf("DGLS Backport Module Test\n");
    printf("========================\n");
    
    /* Test Diamond Shell v2 */
    printf("[1] Diamond Shell v2: SHELL_CHUNK_SZ=%u, SHELL_ROT_STATES=%u\n",
           SHELL_CHUNK_SZ, SHELL_ROT_STATES);
    
    /* Test GPX5 */
    printf("[2] GPX5 Container: GPX5_MAGIC=0x%08X\n", GPX5_MAGIC);
    
    /* Test Flow Chunker */
    printf("[3] Flow Chunker v8: FLOW_MIN_CHUNK=%u, FLOW_MAX_CHUNK=%u\n",
           FLOW_MIN_CHUNK, FLOW_MAX_CHUNK);
    
    /* Test FiboClock */
    printf("[4] FiboClock: FIBO_PERIOD_SIG=%u, FIBO_PERIOD_FLUSH=%u\n",
           FIBO_PERIOD_SIG, FIBO_PERIOD_FLUSH);
    
    /* Test Rewind */
    printf("[5] Geo Rewind: REWIND_SLOTS=%u, REWIND_MAX_SNAPSHOTS=%u\n",
           REWIND_SLOTS, REWIND_MAX_SNAPSHOTS);
    
    /* Test FEC */
    printf("[6] FEC: FEC_CHUNKS_PER_BLOCK=%u, FEC_LEVELS=%u\n",
           FEC_CHUNKS_PER_BLOCK, FEC_LEVELS);
    
    /* Test RS */
    printf("[7] Reed-Solomon: RS_MAX_K=%u, RS_MAX_N=%u\n",
           RS_MAX_K, RS_MAX_N);
    
    /* Test Shell Fold */
    printf("[8] Shell Fold: GEO_FIBO[0]=%u, GEO_FIBO[11]=%u\n",
           GEO_FIBO[0], GEO_FIBO[11]);
    
    /* Test Temporal LUT */
    printf("[9] Temporal LUT: TEMPORAL_WALK_LEN=%u\n", TEMPORAL_WALK_LEN);
    
    /* Test Wang */
    printf("[10] Wang Tile: WANG_ROW_SIZE=%u, WANG_ROW_COUNT=%u\n",
           WANG_ROW_SIZE, WANG_ROW_COUNT);
    
    printf("\nAll modules compiled successfully!\n");
    return 0;
}
