/*
 * test_tw_tensor_capture.c — TW Capture on Real Q8_0 Tensor Data
 *
 * Reads .qdat files from a raw tensor store (built by build_smollm2_store.py),
 * dequantizes Q8_0 blocks, computes 2D signature, runs TW capture.
 *
 * gcc -I. tests/test_tw_tensor_capture.c -o tests/test_tw_tensor_capture -lm
 *   && ./tests/test_tw_tensor_capture <tensors_dir>
 *
 * If no tensors_dir given, defaults to "build/smollm2_tensors_raw/".
 * Pass --info for verbose per-tensor output.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define GEOM_RAW_BRIDGE_IMPLEMENTATION
#include "tw_tensor_capture.h"
#include "geom_raw_bridge.h"

static int verbose = 0;

static void print_tw_capture(const TWTensorCapture *tc, const char *name)
{
    printf("%-50s ", name);
    printf("z=%-2d s=%-3d (%+13lld,%+13lld) %s",
           tc->cap.zone, tc->cap.slot,
           (long long)tc->cap.resid_x, (long long)tc->cap.resid_y,
           tc->cap.drain ? "DRAIN" : "     ");
    if (tc->cap.drain) {
        printf("→z%d/s%d", tc->cap.drain_zone, tc->cap.drain_slot);
    }
    printf("  sig=(%.4f,%.4f) μ=%.4f σ=%.4f n=%u",
           tc->sig_x, tc->sig_y, tc->mean, tc->std, tc->n_dequant);
    printf("\n");
}

int main(int argc, char **argv)
{
    const char *tensors_dir = "build/smollm2_tensors_raw";
    if (argc > 1 && strcmp(argv[1], "--info") == 0) {
        verbose = 1;
        if (argc > 2) tensors_dir = argv[2];
    } else if (argc > 1) {
        tensors_dir = argv[1];
    }

    printf("=== TW Tensor Capture Test ===\n");
    printf("Tensor dir: %s\n\n", tensors_dir);

    RawBridge rb;
    if (rb_load(&rb, tensors_dir) != RB_OK) {
        printf("ERROR: No .qdat files found in '%s'\n", tensors_dir);
        printf("Run: python collection/build_smollm2_store.py --gguf I:/model/SmolLM2-360M-Instruct.Q8_0.gguf --out build/\n");
        return 1;
    }
    printf("Loaded %u tensors from RawBridge\n\n", rb.n_entries);

    /* Zone/slot distribution */
    int zone_counts[TW_N_SECTORS] = {0};
    int slot_counts[TW_N_SLOTS] = {0};
    int n_frozen = 0;
    int n_pass = 0, n_fail = 0;
    int drain_counts[TW_N_SECTORS] = {0};

    for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++) {
        if (!rb.entries[i].occupied) continue;

        TWTensorCapture tc;
        tw_capture_tensor_by_name(&rb, rb.entries[i].name, &tc);

        if (verbose) print_tw_capture(&tc, rb.entries[i].name);

        int z = tc.cap.zone;
        int s = tc.cap.slot;
        if (z >= 0 && z < TW_N_SECTORS) zone_counts[z]++;
        if (s >= 0 && s < TW_N_SLOTS)   slot_counts[s]++;

        if (tc.cap.drain) {
            drain_counts[tc.cap.drain_zone]++;
        }

        /* Verify reconstruction */
        int64_t rx, ry;
        tw_reconstruct_int(&tc.cap, &rx, &ry);
        int64_t vx = (int64_t)(tc.sig_x * TW_SCALE);
        int64_t vy = (int64_t)(tc.sig_y * TW_SCALE);
        if (rx == vx && ry == vy) {
            n_pass++;
        } else {
            n_fail++;
            printf("MISMATCH %s: (%d,%d) ≠ recon (%d,%d)\n",
                   rb.entries[i].name, vx, vy, rx, ry);
        }
    }

    printf("\n=== Results ===\n");
    printf("Tensors: %u  Recon OK: %d  Fail: %d\n",
           rb.n_entries, n_pass, n_fail);

    printf("\nZone Distribution (%d sectors):\n", TW_N_SECTORS);
    for (int z = 0; z < TW_N_SECTORS; z++) {
        printf("  zone %2d: %4d tensors (drain→here: %d)\n",
               z, zone_counts[z], drain_counts[z]);
    }

    printf("\nSlot Distribution (%d slots):\n", TW_N_SLOTS);
    for (int s = 0; s < TW_N_SLOTS; s++) {
        if (slot_counts[s] > 0)
            printf("  slot %2d: %d\n", s, slot_counts[s]);
    }

    /* Compute entropy / dispersion of zone distribution */
    double total = (double)rb.n_entries;
    double entropy = 0;
    for (int z = 0; z < TW_N_SECTORS; z++) {
        if (zone_counts[z] > 0) {
            double p = zone_counts[z] / total;
            entropy -= p * log(p) / log(2.0);
        }
    }
    printf("\nZone entropy: %.4f bits (max %.4f)\n",
           entropy, log(TW_N_SECTORS) / log(2.0));

    rb_free(&rb);
    printf("\n✓ Done\n");
    return n_fail > 0 ? 1 : 0;
}
