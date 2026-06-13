/* test_full_pipeline.c — Full TW→POGLS pipeline: load, capture, bridge, wallet
 * Build:
 *   gcc -DGEO_JUMP_INLINE -I. -I../geo_jump_module/include -I../src
 *       -o tests/test_full_pipeline.exe
 *       tests/test_full_pipeline.c -lm
 * Run:   test_full_pipeline.exe <tensors_dir>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>

/* ── Timer ─────────────────────────────────────────────────── */
static double g_freq;
static LARGE_INTEGER g_start;
static void timer_init(void) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = (double)f.QuadPart;
}
static void timer_start(void) {
    QueryPerformanceCounter(&g_start);
}
static double timer_elapsed(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - g_start.QuadPart) / g_freq;
}

/* ── RawBridge + TW Capture + Bridge + Bond ────────────────── */
#define GEOM_RAW_BRIDGE_IMPLEMENTATION
#include "geom_raw_bridge.h"
#include "tw_tensor_capture.h"
#include "tw_bridge.h"
#include "pogls_config.h"
#include "pogls_bond.h"
#define HEX_CODEC_IMPL
#include "hex_codec.h"

/* entropy helper */
static double calc_entropy(int *dist, int n, int total) {
    double e = 0.0;
    for (int i = 0; i < n; i++) {
        if (dist[i] == 0) continue;
        double p = (double)dist[i] / total;
        e -= p * log2(p);
    }
    return e;
}

int main(int argc, char **argv) {
    const char *tensors_dir = "build/smollm2_tensors_raw";
    if (argc > 1) tensors_dir = argv[1];

    timer_init();

    printf("═══════════════════════════════════════════════════════════\n");
    printf("  FULL PIPELINE TEST — TW Capture → Bridge → Wallet\n");
    printf("  Model: SmolLM2-360M-Instruct Q8_0  (290 tensors)\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    /* ── Step 1: Load via RawBridge ─────────────────────────── */
    printf("─── [1/5] Load Q8_0 store ───\n");
    timer_start();
    RawBridge rb;
    memset(&rb, 0, sizeof(rb));
    rb_load(&rb, tensors_dir);
    double load_time = timer_elapsed();
    printf("  %u tensors  %.3f s\n", rb.n_entries, load_time);
    if (rb.n_entries == 0) { printf("  ERROR: empty\n"); return 1; }

    /* build occupied index */
    uint32_t occ[RB_MAX_ENTRIES], nocc = 0;
    uint64_t total_qdat = 0;
    for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++) {
        if (rb.entries[i].occupied) {
            occ[nocc++] = i;
            total_qdat += rb.entries[i].size;
        }
    }

    /* ── Step 2: TW Capture ─────────────────────────────────── */
    printf("\n─── [2/5] TW Capture ───\n");
    TWTensorCapture *tc = (TWTensorCapture *)malloc(nocc * sizeof(TWTensorCapture));
    if (!tc) { printf("  OOM\n"); return 1; }

    uint32_t *occ_idx = (uint32_t *)malloc(nocc * sizeof(uint32_t));
    memcpy(occ_idx, occ, nocc * sizeof(uint32_t));

    timer_start();
    int capture_ok = 0;
    for (uint32_t j = 0; j < nocc; j++)
        if (tw_capture_tensor_by_name(&rb, rb.entries[occ_idx[j]].name, &tc[j]) == RB_OK)
            capture_ok++;
    double capture_time = timer_elapsed();

    /* stats */
    int drain_count = 0;
    int zone_dist[10] = {0}, slot_dist[60] = {0};
    for (uint32_t j = 0; j < nocc; j++) {
        if (tc[j].cap.drain) drain_count++;
        if (tc[j].cap.zone < 10) zone_dist[tc[j].cap.zone]++;
        if (tc[j].cap.slot < 60) slot_dist[tc[j].cap.slot]++;
    }
    int az = 0, as = 0;
    for (int z = 0; z < 10; z++) if (zone_dist[z]) az++;
    for (int s = 0; s < 60; s++) if (slot_dist[s]) as++;

    printf("  Capture OK: %d/%d  drain=%d  zone_dist=[", capture_ok, nocc, drain_count);
    for (int z = 0; z < 10; z++) printf("%s%d", z ? "," : "", zone_dist[z]);
    printf("]  slots=%d/60\n", as);

    /* ── Step 3: Reconstruction ─────────────────────────────── */
    printf("\n─── [3/5] Lossless verify ───\n");
    timer_start();
    int recon_ok = 0;
    for (uint32_t j = 0; j < nocc; j++) {
        int64_t rx, ry;
        tw_reconstruct_int(&tc[j].cap, &rx, &ry);
        int64_t vx = (int64_t)(tc[j].sig_x * TW_SCALE);
        int64_t vy = (int64_t)(tc[j].sig_y * TW_SCALE);
        if (rx == vx && ry == vy) recon_ok++;
    }
    double recon_time = timer_elapsed();
    printf("  %d/%d  %.1f%%  %.4fs\n", recon_ok, nocc,
           100.0 * recon_ok / nocc, recon_time);

    /* ── Step 4: Shell 1+2+3 Bridge ─────────────────────────── */
    printf("\n─── [4/5] Shell 1+2+3 (tw_bridge) ───\n");
    uint32_t tick = 12;  /* P5H barrier */
    uint8_t density = 1, is_temporal = 1, layer = 0;
    uint32_t seed_node = 0x42;

    timer_start();
    int frozen = 0;
    uint64_t wallet_total = 0;
    for (uint32_t j = 0; j < nocc; j++) {
        TWBridgeResult br = tw_bridge(
            &tc[j].cap, density, is_temporal, layer, tick, seed_node);
        if (br.frozen) {
            frozen++;
            /* wallet entry via pogls_bond */
            uint64_t origin = POGLS_GEO_MAGIC
                ^ (uint64_t)tc[j].cap.zone
                ^ ((uint64_t)tc[j].cap.slot << 8)
                ^ ((uint64_t)br.freeze_addr << 16);
            uint8_t axis = (tc[j].cap.zone % 7) + 1;
            PoglsPiece p = pogls_make_piece(origin, axis);
            wallet_total += p.geo_key;
        }
    }
    double bridge_time = timer_elapsed();
    printf("  %d frozen / %d drain  (tick=%u)  %.4fs\n",
           frozen, drain_count, tick, bridge_time);

    /* ── Step 5: Concatenated store ─────────────────────────── */
    printf("\n─── [5/5] Build unified .gsten store ───\n");
    const char *gsten_path = "..\\build\\smollm2.gsten";
    FILE *fout = fopen(gsten_path, "wb");
    if (!fout) { printf("  ERROR: cannot write %s\n", gsten_path); return 1; }

    /* Header */
    uint32_t magic = 0x4747454F; /* 'GEOM' */
    fwrite(&magic, 4, 1, fout);
    uint32_t n = nocc;
    fwrite(&n, 4, 1, fout);
    uint8_t reserved[8] = {0};
    fwrite(reserved, 8, 1, fout);

    /* Index: offset(4B) + size(4B) + dtype(1B) per entry */
    uint64_t data_offset = 16 + nocc * 9; /* header + index */
    struct __attribute__((packed)) { uint32_t offset; uint32_t size; uint8_t dtype; } idx;

    timer_start();
    for (uint32_t j = 0; j < nocc; j++) {
        uint32_t ei = occ_idx[j];
        idx.offset = (uint32_t)data_offset;
        idx.size   = (uint32_t)rb.entries[ei].size;
        idx.dtype  = (uint8_t)rb.entries[ei].dtype;
        fwrite(&idx, sizeof(idx), 1, fout);
        data_offset += idx.size;
    }

    /* Data blocks */
    for (uint32_t j = 0; j < nocc; j++) {
        uint32_t ei = occ_idx[j];
        fwrite(rb.entries[ei].data, 1, rb.entries[ei].size, fout);
    }

    fclose(fout);
    double store_time = timer_elapsed();

    /* verify */
    FILE *fchk = fopen(gsten_path, "rb");
    fseek(fchk, 0, SEEK_END);
    long gsten_size = ftell(fchk);
    fclose(fchk);

    printf("  %s  %ld bytes  (%.4fs)\n", gsten_path, gsten_size, store_time);
    printf("  Combined: %.2f MB → %.2f MB\n",
           total_qdat / (1024.0*1024.0), gsten_size / (1024.0*1024.0));
    printf("  Overhead: +%.1f%% (index)\n",
           100.0 * (gsten_size - (long)total_qdat) / (double)total_qdat);

    /* ── Bonus: hex_tile compression test on first few tensors ─── */
    printf("\n─── [BONUS] hex_tile compression test ───\n");
    int hflat = 0, hsmooth = 0, hgrad = 0, hedge = 0;
    uint64_t raw_bytes = 0, enc_bytes = 0;
    int htest_cnt = 0;
    for (uint32_t j = 0; j < nocc && j < 20; j++) {
        uint32_t ei = occ_idx[j];
        uint8_t *buf = (uint8_t *)rb.entries[ei].data;
        size_t sz = rb.entries[ei].size;
        for (size_t off = 0; off + 7 <= sz; off += 7) {
            HexTile tile;
            memcpy(tile.c, buf + off, 7);
            uint8_t dst[16];
            int nw = hex_tile_encode(&tile, dst);
            raw_bytes += 7;
            enc_bytes += (uint32_t)nw;
            uint8_t cls = hex_tile_classify(&tile);
            if (cls == HENC_FLAT) hflat++;
            else if (cls == HENC_SMOOTH) hsmooth++;
            else if (cls == HENC_GRADIENT) hgrad++;
            else hedge++;
        }
        htest_cnt++;
    }
    printf("  Tested %d tensors (%llu tiles)\n", htest_cnt,
           (unsigned long long)(raw_bytes / 7));
    printf("  Classification: FLAT=%d SMOOTH=%d GRADIENT=%d EDGE=%d\n",
           hflat, hsmooth, hgrad, hedge);
    double ratio = (double)enc_bytes / raw_bytes;
    printf("  Compression: %llu raw → %llu enc  (ratio=%.4f, %s)\n",
           (unsigned long long)raw_bytes, (unsigned long long)enc_bytes,
           ratio, ratio < 1.0 ? "SHRINKS" : "EXPANDS");

    /* ── Summary ────────────────────────────────────────────── */
    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  RESULTS\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    printf("  Sizes:\n");
    printf("    Raw store (290 .qdat):  %12llu bytes  (%.2f MB)\n",
           (unsigned long long)total_qdat,
           total_qdat / (1024.0*1024.0));
    printf("    Per tensor avg:          %12.0f bytes\n",
           (double)total_qdat / nocc);
    printf("    Routing (.twidx):        ~74,000 bytes  (~74 KB)\n");
    printf("    Unified (.gsten):        %12ld bytes  (%.2f MB)\n",
           gsten_size, gsten_size / (1024.0*1024.0));
    printf("    Wallet entries:          %d (frozen at tick=%u)\n",
           frozen, tick);

    printf("\n  Speed (total pipeline — %u tensors):\n", nocc);
    printf("    ┌──────────────────────┬──────────┬──────────────┐\n");
    printf("    │ Step                 │   Time   │     Rate     │\n");
    printf("    ├──────────────────────┼──────────┼──────────────┤\n");
    printf("    │ Load %-23s│ %6.4fs │ %10.1f MB/s │\n", "",
           load_time, (total_qdat / (1024.0*1024.0)) / load_time);
    printf("    │ TW Capture %-19s│ %6.4fs │ %11.1f t/s │\n", "",
           capture_time, nocc / capture_time);
    printf("    │ Reconstruction %-16s│ %6.4fs │ %11.1f t/s │\n", "",
           recon_time, nocc / recon_time);
    printf("    │ Shell 1-3 route %-14s│ %6.4fs │ %11.1f t/s │\n", "",
           bridge_time, nocc / bridge_time);
    printf("    │ Store build %-18s│ %6.4fs │ %7.1f MB/s │\n", "",
           store_time, (total_qdat / (1024.0*1024.0)) / store_time);
    printf("    ├──────────────────────┼──────────┼──────────────┤\n");
    double total = load_time + capture_time + recon_time + bridge_time + store_time;
    printf("    │ TOTAL %-25s│ %6.4fs │              │\n", "", total);
    printf("    └──────────────────────┴──────────┴──────────────┘\n");
    printf("    Per tensor (capture):  %7.1f us\n",
           capture_time / nocc * 1e6);

    printf("\n  Quality metrics:\n");
    printf("    Lossless reconstruction:  %s (%d/%d)\n",
           recon_ok == nocc ? "YES" : "NO", recon_ok, nocc);
    printf("    Zone coverage:            %d/10  (%.1f%%)\n", az, 100.0*az/10);
    printf("    Slot coverage:            %d/60  (%.1f%%)\n", as, 100.0*as/60);
    printf("    Zone entropy:             %.4f bits (%.1f%% of max)\n",
           calc_entropy(zone_dist, 10, nocc),
           100.0 * calc_entropy(zone_dist, 10, nocc) / 3.3219);
    printf("    Drain rate:               %.1f%%\n",
           100.0 * drain_count / nocc);
    printf("    Frozen (Shell 3):         %d  (%.1f%% of drain)\n",
           frozen, 100.0 * frozen / (drain_count ? drain_count : 1));
    printf("    Wallet bond:              %llu (signature)\n",
           (unsigned long long)wallet_total);

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  ALL DONE\n");

    free(tc);
    free(occ_idx);
    rb_free(&rb);
    return 0;
}
