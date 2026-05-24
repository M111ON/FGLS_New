/*
 * geo_field_main.c — GeoField Demo Program
 * ═══════════════════════════════════════════════════════════════════════
 *
 * Demonstrations:
 *   1. COMPLETE LOOP — encode → save → load → decode → verify
 *   2. SCALE — zoom in/out by changing gp_level
 *   3. SHAPE DIMENSION ACCESS — Metatron routing between pentagon faces
 *
 * Build:
 *   See Makefile for include paths and linker flags.
 *
 * Usage:
 *   geo_field_demo
 * ═══════════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>

#include "geo_field_core.h"

/* ═══════════════════════════════════════════════════════════════════════
   DEMO 1 — Complete Loop Roundtrip
   ═══════════════════════════════════════════════════════════════════════
   Encode synthetic data → decode → compare. Verifies bit-exact.
   ═══════════════════════════════════════════════════════════════════════ */

static int demo_complete_loop(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  DEMO 1: COMPLETE LOOP (encode \xe2\x86\x92 decode \xe2\x86\x92 verify)\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    #define DEMO_SZ 8192
    uint8_t *original = (uint8_t *)malloc(DEMO_SZ);
    if (!original) return -1;

    for (int i = 0; i < DEMO_SZ; i++)
        original[i] = (uint8_t)((i * 37 + (i >> 3) * 13) & 0xFF);

    printf("  Original: %d bytes (%d chunks x 64B)\n",
           DEMO_SZ, DEMO_SZ / 64);

    GeoFieldEncodeStats enc_stats;
    GeoFieldDecodeStats dec_stats;
    int ret = geo_field_roundtrip(original, DEMO_SZ, 2,
                                   &enc_stats, &dec_stats);

    printf("  Encode:  %" PRIu64 " chunks, %u blocks, %" PRIu64 " zone resets\n",
           enc_stats.total_chunks,
           (unsigned)enc_stats.total_blocks,
           enc_stats.zone_resets);
    printf("  Decode:  %" PRIu64 " ok, %" PRIu64 " missing, %" PRIu64 " bytes\n",
           dec_stats.chunks_decoded,
           dec_stats.chunks_missing,
           dec_stats.bytes_written);
    printf("  Skeleton: ID=%u FLAT=%u DIFF=%u BREF=%u GEOM=%u RAW=%u\n",
           enc_stats.skel_hits[0], enc_stats.skel_hits[1],
           enc_stats.skel_hits[2], enc_stats.skel_hits[3],
           enc_stats.skel_hits[4], enc_stats.skel_hits[5]);

    if (ret == 0)
        printf("  \xe2\x9c\x85 Roundtrip: PASS (bit-exact)\n\n");
    else if (ret == -3)
        printf("  \xe2\x9d\x8c Roundtrip: FAIL (data mismatch)\n\n");
    else
        printf("  \xe2\x9d\x8c Roundtrip: ERROR (code=%d)\n\n", ret);

    free(original);
    return ret;
}

/* ═══════════════════════════════════════════════════════════════════════
   DEMO 2 — Scale (Zoom In/Out)
   ═══════════════════════════════════════════════════════════════════════
   Show face_count changes across gp_levels + multi-res encode.
   ═══════════════════════════════════════════════════════════════════════ */

static void demo_scale(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  DEMO 2: SCALE (zoom in/out via gp_level)\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    printf("  gp_level \xe2\x86\x92 face_count = 10n\xc2\xb2 + 2  (tiles/dim)\n\n");
    for (uint8_t lv = 1; lv <= GP_MAX_LEVEL; lv++) {
        uint32_t faces = gp_face_count(lv);
        uint32_t hex = faces - GP_PENT_COUNT;
        printf("    level %d  \xe2\x86\x92  %4u tiles  (%u penta + %u hexa)",
               lv, faces, GP_PENT_COUNT, hex);
        if (lv < GP_MAX_LEVEL) {
            uint8_t zin;
            uint32_t zf = geo_field_zoom_in(lv, &zin);
            printf("  [zoom in \xe2\x86\x92 L%d = %u tiles]", zin, zf);
        }
        printf("\n");
    }

    printf("\n  Multi-res encode (L2\xe2\x86\x92L4, same 4096B):\n");
    #define MR_SZ 4096
    uint8_t mr_data[MR_SZ];
    for (int i = 0; i < MR_SZ; i++)
        mr_data[i] = (uint8_t)((i * 7 + 11) & 0xFF);

    GeoField fields[3];
    GeoFieldEncodeStats mrstats[3];
    int nlv = geo_field_encode_multires(mr_data, MR_SZ, 2, 4,
                                         fields, mrstats);
    if (nlv > 0) {
        for (int i = 0; i < nlv; i++) {
            uint8_t lv = (uint8_t)(2 + i);
            printf("    level %d: %" PRIu64 " chunks, %u blocks, %u tiles/layer\n",
                   lv,
                   mrstats[i].total_chunks,
                   (unsigned)mrstats[i].total_blocks,
                   gp_face_count(lv));
        }

        for (int i = 0; i < nlv; i++) {
            uint8_t *mr_out = (uint8_t *)malloc(MR_SZ);
            GeoFieldDecodeStats mr_ds;
            int64_t w = geo_field_decode(&fields[i], mr_out, MR_SZ, &mr_ds);
            int ok = (w == MR_SZ && memcmp(mr_data, mr_out, MR_SZ) == 0);
            printf("    decode level %d: %" PRId64 " bytes, %s\n",
                   2 + i, w, ok ? "OK" : "FAIL");
            free(mr_out);
        }

        for (int i = 0; i < nlv; i++)
            geo_field_free(&fields[i]);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════
   DEMO 3 — Shape Dimension Access (Metatron Routing)
   ═══════════════════════════════════════════════════════════════════════
   Navigate faces via ORBITAL / CHIRAL / CROSS / HUB routes.
   ═══════════════════════════════════════════════════════════════════════ */

static void demo_shape_access(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  DEMO 3: SHAPE DIMENSION ACCESS (Metatron Routing)\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    printf("  Route hierarchy (all O(1), no malloc):\n");
    printf("    ORBITAL \xe2\x80\x94 same face, circular slot\n");
    printf("    CHIRAL  \xe2\x80\x94 opposite face (f \xe2\x86\x94 f+6)\n");
    printf("    CROSS   \xe2\x80\x94 inter-ring 3-step (self-inverse)\n");
    printf("    HUB     \xe2\x80\x94 any face via center node\n\n");

    uint8_t sd[2048];
    for (int i = 0; i < 2048; i++) sd[i] = (uint8_t)(i & 0xFF);

    GeoField gf;
    geo_field_init(&gf, 2, 2);
    GeoFieldEncodeStats es;
    geo_field_encode(&gf, sd, sizeof(sd), &es);

    printf("  Routes from pentagon face 0 (tile_id=0):\n\n");

    const char *route_names[] = { "ORBITAL", "CHIRAL", "CROSS", "HUB" };

    for (uint8_t dst = 0; dst < 12; dst++) {
        GeoFieldShapeAccess sa = geo_field_shape_route(0, dst, 2);
        printf("    \xe2\x86\x92 face %2u  [slot %2u]  via %-7s  (enc %3u \xe2\x86\x92 %3u)\n",
               sa.dst_face, sa.dst_slot,
               route_names[sa.route_type],
               sa.src_enc, sa.dst_enc);
    }

    printf("\n  Reading data via shape route:\n");
    uint8_t routed[64];
    int rr = geo_field_shape_read(&gf, 0, META_ROUTE_CHIRAL, 6, routed);
    if (rr == 0) {
        printf("    CHIRAL (face 0 \xe2\x86\x92 face 6): first 16 bytes: ");
        for (int i = 0; i < 16; i++) printf("%02x ", routed[i]);
        printf("\n");
    }

    printf("\n  Circuit switch (tri[5] FLOW condition):\n");
    uint16_t enc0 = geo_field_tile_to_enc(0, 2);
    uint16_t enc6 = geo_field_tile_to_enc(6, 2);
    uint8_t cond0 = metatron_cond(enc0);
    uint8_t cond6 = metatron_cond(enc6);
    uint8_t key6  = metatron_key(enc6);
    int trip = metatron_trip(cond0, enc6);

    printf("    cond(face0)  = %2u  (face*3 + enc%%3)\n", cond0);
    printf("    cond(face6)  = %2u\n", cond6);
    printf("    key(face6)   = %2u  (=(face+6)%%12*3 + enc%%3)\n", key6);
    printf("    cond+18 mod36 = %2u  (half-ring shift)\n", (cond0 + 18) % 36);
    printf("    trip = %s  (cond+18 == key?)\n",
           trip ? "FIRES" : "blocked");
    printf("\n");

    geo_field_free(&gf);
}

/* ═══════════════════════════════════════════════════════════════════════
   DEMO 4 — File-based roundtrip
   ═══════════════════════════════════════════════════════════════════════
   Encode data \xe2\x86\x92 save to .geofield file \xe2\x86\x92 load \xe2\x86\x92 decode \xe2\x86\x92 verify.
   ═══════════════════════════════════════════════════════════════════════ */

static int demo_file_roundtrip(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  DEMO 4: FILE-BASED ROUNDTRIP\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    uint8_t fdata[3456];
    fdata[0] = 0; fdata[1] = 1;
    for (size_t i = 2; i < sizeof(fdata); i++)
        fdata[i] = (uint8_t)((fdata[i-1] + fdata[i-2]) & 0xFF);

    printf("  Data: %u bytes (%u chunks, gp_level=2)\n",
           (unsigned)sizeof(fdata), (unsigned)(sizeof(fdata)/64));

    /* Encode and save — compute needed blocks */
    uint32_t tiles_per_layer = gp_face_count(2);
    uint32_t blocks_per_layer = (tiles_per_layer + GF_CHUNKS_PER_BLOCK - 1)
                                / GF_CHUNKS_PER_BLOCK;
    uint32_t nb = (uint32_t)GP_MAX_DIM * blocks_per_layer;
    GeoField gf_enc;
    geo_field_init(&gf_enc, 2, nb);
    GeoFieldEncodeStats es2;
    geo_field_encode(&gf_enc, fdata, sizeof(fdata), &es2);

    const char *fpath = "demo_roundtrip.geofield";
    if (geo_field_save(&gf_enc, fdata, sizeof(fdata), fpath) != 0) {
        printf("  Save failed\n");
        geo_field_free(&gf_enc);
        return -1;
    }
    printf("  Saved: %s (%u blocks x %uB = %uB + header)\n",
           fpath, nb, (unsigned)FGLS_TOTAL_BYTES,
           (unsigned)(nb * FGLS_TOTAL_BYTES + 32));

    geo_field_free(&gf_enc);

    GeoField gf_dec;
    int64_t orig_sz = geo_field_load(&gf_dec, fpath);
    if (orig_sz < 0) {
        printf("  Load failed\n");
        return -1;
    }
    printf("  Loaded: gp_level=%u, blocks=%u, orig_size=%" PRId64 "\n",
           gf_dec.gp_level, gf_dec.n_blocks, orig_sz);

    uint8_t *fout = (uint8_t *)malloc((size_t)orig_sz);
    GeoFieldDecodeStats ds2;
    int64_t w2 = geo_field_decode(&gf_dec, fout, (size_t)orig_sz, &ds2);
    int ok = (w2 == orig_sz && memcmp(fdata, fout, (size_t)orig_sz) == 0);

    printf("  Decoded: %" PRId64 " bytes, chunks: %" PRIu64 "/%" PRIu64 ", %s\n\n",
           w2, ds2.chunks_decoded,
           (ds2.chunks_decoded + ds2.chunks_missing),
           ok ? "OK" : "FAIL");

    free(fout);
    geo_field_free(&gf_dec);
    remove(fpath);
    return ok ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("\n");
    printf("GeoField -- Unified Encode/Decode Field\n");
    printf("GpSphere x FrustumBlock x Metatron x Trit x Flow\n\n");

    demo_complete_loop();
    demo_scale();
    demo_shape_access();
    demo_file_roundtrip();

    printf("All demos complete.\n\n");
    return 0;
}
