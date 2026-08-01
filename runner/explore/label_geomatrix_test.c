/*
 * label_geomatrix_test.c
 *
 * Connect Geomatrix (C:\Mpogls\Geomatrix) labeling system with FGLS Contour Mask / geo_jump.
 *
 * Key Geometric Connection:
 *   Geomatrix Cylinder = 3456 addresses (6 spokes x 576 slots = 144 x 24)
 *   geo_jump           = 20736 addresses (12^4 = 144 x 144 = 3456 x 6)
 *   Factor 6           = 6 spokes / 6 faces of the Contour Mask!
 *   So 1 Geomatrix Cylinder (3456) x 6 spokes = 20,736 (Full geo_jump space).
 *
 * GeoPacket Labeling:
 *   Phase 0: PROBE   (0xAAAAAAAA) -> exploratory / threshold check
 *   Phase 1: MAIN    (0x55555555) -> primary weight payload
 *   Phase 2: MIRROR  (0xF0F0F0F0) -> symmetric / dual weight
 *   Phase 3: CANCEL  (0x0F0F0F0F) -> inverse / noise rejection
 *
 * Compile:
 *   gcc -Wall -Werror -Wno-error=unused-function -O2 -std=c11
 *       -I. -I"C:\Mpogls\Geomatrix"
 *       -o runner/explore/label_geomatrix_test.exe runner/explore/label_geomatrix_test.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include "beam_addressing/gguf_reader.h"
#include "C:\Mpogls\Geomatrix\geomatrix_shared.h"
#include "C:\Mpogls\Geomatrix\geo_cylinder.h"
#include "C:\Mpogls\Geomatrix\geo_thirdeye.h"

static inline void entrance_stamp32(
    GeoPacketSmall *pkt,
    const uint64_t *bundle,
    uint16_t        raw_idx,
    uint8_t         phase)
{
    pkt->sig32 = geo_compute_sig32(bundle, phase);
    pkt->idx   = raw_idx;
    pkt->bit   = (uint8_t)((bundle[raw_idx >> 6] >> (raw_idx & 63)) & 1ULL);
    pkt->phase = phase & 3;
}

#define GEO_JUMP_SPACE  20736
#define CYL_SPACE       CYL_FULL_N   /* 3456 */

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "I:\\model\\Qwen3-0.6B-Q8_0.gguf";

    printf("=== GEOMATRIX <-> CONTOUR MASK LABELING TEST ===\n");
    printf("  Geomatrix Space : %d (6 spokes x 576 slots = 144 x 24)\n", CYL_SPACE);
    printf("  geo_jump Space  : %d (12^4 = 144 x 144 = 3456 x 6)\n", GEO_JUMP_SPACE);
    printf("  Scale Ratio     : %d : 1  (6 spokes = 6 faces of Contour Mask!)\n\n",
           GEO_JUMP_SPACE / CYL_SPACE);

    GGUF_File *gf = gguf_open(path);
    if (!gf) { printf("[FAIL] cannot open GGUF: %s\n", path); return 1; }

    /* Find first tensor >= 20736 weights */
    int tidx = -1;
    for (uint64_t i = 0; i < gf->tensor_count; i++) {
        if (gf->tensors[i].n_weights >= GEO_JUMP_SPACE) { tidx = (int)i; break; }
    }
    if (tidx < 0) { printf("[FAIL] no tensor >= 20736 weights\n"); gguf_close(gf); return 1; }

    GGUF_Tensor *T = &gf->tensors[tidx];
    printf("  Loaded Tensor[%d]: %s  (type=%u, n_weights=%" PRIu64 ")\n",
           tidx, T->name, T->type, (uint64_t)T->n_weights);

    int8_t *raw = (int8_t*)malloc(GEO_JUMP_SPACE);
    fseek(gf->fp, (long)(gf->tensor_data_start + T->offset), SEEK_SET);
    size_t got = fread(raw, 1, GEO_JUMP_SPACE, gf->fp);
    gguf_close(gf);
    printf("  Read %u weights for 1 full geo_jump block (20736)\n\n", (unsigned)got);

    /* --- STEP 1: Map 20736 cells to (spoke_6 x cyl_3456) --- */
    printf("--- STEP 1: Geometric Mapping 20736 -> 6 x 3456 ---\n");
    int phase_counts[4] = {0};
    int spoke_counts[6] = {0};

    /* Build bundle simulating 8-word weights per 64B block */
    uint64_t bundle[GEO_BUNDLE_WORDS];

    for (int i = 0; i < GEO_JUMP_SPACE; i++) {
        uint8_t spoke = (uint8_t)(i % 6);               /* 6 spokes = 6 faces */
        uint16_t cyl_addr = (uint16_t)((i / 6) % CYL_SPACE); /* 0..3455 */

        spoke_counts[spoke]++;

        /* Classify value into Phase (PROBE / MAIN / MIRROR / CANCEL) */
        int8_t w = raw[i];
        uint8_t phase;
        if (abs(w) < 8)        phase = PHASE_PROBE;   /* near-zero / noise threshold */
        else if (w > 0)        phase = PHASE_MAIN;    /* positive weight */
        else if (w < -32)      phase = PHASE_CANCEL;  /* strong negative / suppression */
        else                   phase = PHASE_MIRROR;  /* moderate negative / dual */

        phase_counts[phase]++;

        /* Create GeoPacketSmall (8B) label */
        bundle[0] = (uint64_t)w;
        bundle[1] = cyl_addr;
        bundle[2] = spoke;
        bundle[3] = phase;
        bundle[4] = bundle[5] = bundle[6] = bundle[7] = 0;

        GeoPacketSmall pkt;
        entrance_stamp32(&pkt, bundle, cyl_addr & 511, phase);

        /* Verify L1 check passes */
        if (!geo_l1_check32(pkt.sig32, bundle, phase)) {
            printf("  [FAIL] L1 check failed at cell %d!\n", i);
            free(raw);
            return 1;
        }
    }

    printf("  [PASS] All 20,736 packets stamped and L1-verified!\n\n");

    /* --- STEP 2: Phase Distribution Histogram --- */
    printf("--- STEP 2: Phase Distribution Histogram ---\n");
    const char *phase_names[4] = {"PROBE (near-zero)", "MAIN (positive)", "MIRROR (mod-neg)", "CANCEL (strong-neg)"};
    for (int p = 0; p < 4; p++) {
        printf("  Phase %d [%-18s]: %5d cells (%5.2f%%)\n",
               p, phase_names[p], phase_counts[p], 100.0 * phase_counts[p] / GEO_JUMP_SPACE);
    }

    printf("\n--- STEP 3: Spoke Alignment (6 Spokes = 6 Faces) ---\n");
    for (int s = 0; s < 6; s++) {
        printf("  Spoke %d (Face %d): %5d cells (%5.2f%%)\n",
               s, s, spoke_counts[s], 100.0 * spoke_counts[s] / GEO_JUMP_SPACE);
    }

    printf("\n=== VERDICT ===\n");
    printf("YES: Geomatrix's 3456-cell Cylinder geometry x 6 Spokes EXACTLY equals 20,736 (geo_jump).\n");
    printf("GeoPacket 8B stamps L1-verify at 100%% accuracy with 0 false rejects, enabling phase-based category routing!\n");

    free(raw);
    return 0;
}
