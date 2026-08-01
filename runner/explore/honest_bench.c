/* honest_bench.c — contour mask encode/decode with DCE protection
 * Forces real work: accumulate results into a checksum that gets printed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <inttypes.h>

#define UNITS 6000
#define N_SAMPLES 10000000  /* 10M samples */

static int8_t displacement[UNITS];
static int8_t weights[N_SAMPLES];
static volatile uint64_t sink;  /* forces computation to complete */

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

int main(void) {
    srand(42);
    for (int i = 0; i < N_SAMPLES; i++)
        weights[i] = (int8_t)(rand() % 256 - 128);

    /* ── ENCODE: weight -> displacement (modulo placement) ── */
    double t0 = now_ns();
    for (int i = 0; i < N_SAMPLES; i++)
        displacement[i % UNITS] = weights[i];
    double enc = now_ns() - t0;

    /* ── DECODE: XOR 0 read ── */
    uint64_t sum = 0;
    t0 = now_ns();
    for (int i = 0; i < N_SAMPLES; i++)
        sum += (uint64_t)(uint8_t)(displacement[i % UNITS] ^ 0);
    double dec = now_ns() - t0;

    /* ── DECODE: identity read (direct) ── */
    uint64_t sum2 = 0;
    t0 = now_ns();
    for (int i = 0; i < N_SAMPLES; i++)
        sum2 += (uint64_t)(uint8_t)displacement[i % UNITS];
    double dec_id = now_ns() - t0;

    /* ── REFERENCE: memcpy of 6000 bytes ── */
    static int8_t dst[UNITS];
    t0 = now_ns();
    for (int i = 0; i < N_SAMPLES / UNITS + 1; i++)
        memcpy(dst, displacement, UNITS);
    double cp = now_ns() - t0;

    sink = sum + sum2 + (uint64_t)dst[0];

    printf("=== Honest Contour Mask Bench (DCE-protected) ===\n");
    printf("  Encode  (w->disp):  %8.2f ns/sample  = %6.1f M samples/s\n",
           enc / N_SAMPLES, 1e3 * N_SAMPLES / enc);
    printf("  Decode  (XOR 0):    %8.2f ns/sample  = %6.1f M samples/s\n",
           dec / N_SAMPLES, 1e3 * N_SAMPLES / dec);
    printf("  Decode  (identity): %8.2f ns/sample  = %6.1f M samples/s\n",
           dec_id / N_SAMPLES, 1e3 * N_SAMPLES / dec_id);
    printf("  memcpy  (6000B):    %8.2f ns/copy    = %6.1f M copies/s\n",
           cp / (N_SAMPLES / UNITS + 1), 1e3 * (N_SAMPLES / UNITS + 1) / cp);
    printf("  checksum (sanity):  %" PRIu64 "\n", (uint64_t)sink);
    printf("\n  Bytes moved per decode op: 1 (int8)\n");
    printf("  -> throughput = 1 / ns_per_op GB/s per lane\n");
    return 0;
}
