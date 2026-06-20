/*
 * test_stream_window.c — CPU fread + GPU consume loop harness
 *
 * Simulates: PWC/GGUF tensor stream → window slots → GPU consume
 * Measures:  throughput (MB/s), slot utilization, flush timing
 *
 * Build: gcc -O2 -o test_sw test_stream_window.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "stream_window.h"

/* ── Config ─────────────────────────────────────────────────────── */
#define VRAM_BUDGET_MB   256u          /* simulate 256MB VRAM window */
#define N_TENSORS        64u           /* tensors to stream          */
#define TENSOR_MIN_KB    512u
#define TENSOR_MAX_KB    4096u
#define C144_INTERVAL    144u          /* flush every 144 ops        */

/* ── Fake file (in-memory, simulates fread) ─────────────────────── */
#define FAKE_FILE_MB     512u
static uint8_t fake_file[FAKE_FILE_MB * 1024 * 1024];

static void fake_file_init(void) {
    /* fill with pattern so GPU "work" is verifiable */
    for (size_t i = 0; i < sizeof(fake_file); i++)
        fake_file[i] = (uint8_t)(i ^ (i >> 8));
}

/* ── Fake tensor table ──────────────────────────────────────────── */
typedef struct {
    char     name[64];
    uint64_t offset;
    uint64_t size;
    uint32_t geo_addr;
    uint8_t  fmt;
} FakeTensor;

static FakeTensor tensor_table[N_TENSORS];

static void tensor_table_init(void) {
    uint64_t off = 0;
    for (uint32_t i = 0; i < N_TENSORS; i++) {
        snprintf(tensor_table[i].name, 64, "blk.%u.attn_weight", i);
        uint64_t sz = ((TENSOR_MIN_KB + (uint64_t)(rand() % (TENSOR_MAX_KB - TENSOR_MIN_KB)))
                       * 1024);
        /* clamp to fake_file size */
        if (off + sz > sizeof(fake_file)) sz = sizeof(fake_file) - off;
        tensor_table[i].offset   = off;
        tensor_table[i].size     = sz;
        tensor_table[i].geo_addr = 0xD0000000u | (i * 60u); /* dodeca geo */
        tensor_table[i].fmt      = (i % 2 == 0) ? SW_FMT_PWC : SW_FMT_GGUF;
        off += sz;
        if (off >= sizeof(fake_file)) off = 0;
    }
}

/* ── Timer ──────────────────────────────────────────────────────── */
static inline double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ── Simulate GPU work (checksum over buffer) ───────────────────── */
static uint64_t gpu_consume_work(const uint8_t *buf, uint64_t sz) {
    uint64_t acc = 0;
    for (uint64_t i = 0; i < sz; i += 64) acc ^= buf[i];
    return acc;
}

/* ── Main loop ──────────────────────────────────────────────────── */
int main(void) {
    printf("=== stream_window test harness ===\n");
    printf("  VRAM budget : %u MB\n", VRAM_BUDGET_MB);
    printf("  Tensors     : %u\n", N_TENSORS);
    printf("  Fake file   : %u MB\n", FAKE_FILE_MB);
    printf("\n");

    fake_file_init();
    tensor_table_init();

    /* Allocate VRAM window (caller owns buffer) */
    uint64_t win_bytes = (uint64_t)VRAM_BUDGET_MB * 1024 * 1024;
    uint8_t *win_buf = (uint8_t *)malloc(win_bytes);
    if (!win_buf) { fprintf(stderr, "alloc failed\n"); return 1; }

    GearLock gear;
    memset(&gear, 0, sizeof(gear));

    StreamWindow sw;
    sw_init(&sw, win_buf, win_bytes, &gear);

    /* Wire c144 ref to gear cpu_ops (simulate fibo clock) */
    static uint8_t c144_counter = 0;
    gear.c144_ref = &c144_counter;

    /* ── Stream loop ─────────────────────────────────────────────── */
    double t_start = now_sec();
    uint64_t total_bytes  = 0;
    uint64_t total_ops    = 0;
    uint64_t checksum     = 0;
    uint32_t flush_count  = 0;
    uint32_t skip_count   = 0;

    for (uint32_t i = 0; i < N_TENSORS; i++) {
        FakeTensor *ft = &tensor_table[i];

        /* CPU: request slot */
        int slot = sw_request(&sw,
                              ft->name,
                              ft->offset,
                              ft->size,
                              ft->geo_addr,
                              ft->fmt);

        if (slot < 0) {
            /* Window full — flush then retry */
            sw_flush(&sw);
            flush_count++;
            c144_counter = (uint8_t)((c144_counter + 1) % 144);

            slot = sw_request(&sw, ft->name, ft->offset, ft->size,
                              ft->geo_addr, ft->fmt);
            if (slot < 0) {
                fprintf(stderr, "  [SKIP] tensor %u too large for any slot\n", i);
                skip_count++;
                continue;
            }
        }

        /* CPU: "fread" — memcpy from fake_file into slot buffer */
        uint64_t src_off = ft->offset % (sizeof(fake_file) - ft->size);
        uint64_t copy_sz = ft->size;
        if (copy_sz > sw.slots[slot].capacity)
            copy_sz = sw.slots[slot].capacity;

        memcpy(sw.slots[slot].buf, fake_file + src_off, copy_sz);
        sw_mark_ready(&sw, slot, copy_sz);

        /* GPU: consume */
        uint64_t gpu_sz = 0;
        uint8_t *gpu_ptr = sw_consume(&sw, slot, &gpu_sz);
        if (gpu_ptr) {
            checksum ^= gpu_consume_work(gpu_ptr, gpu_sz);
            total_bytes += gpu_sz;
            total_ops++;
        }

        /* GPU: release */
        sw_release(&sw, slot);

        /* c144 boundary flush */
        if (total_ops % C144_INTERVAL == 0) {
            sw_flush(&sw);
            flush_count++;
            c144_counter = (uint8_t)((c144_counter + 1) % 144);
        }
    }

    double elapsed = now_sec() - t_start;

    /* ── Results ─────────────────────────────────────────────────── */
    printf("=== Results ===\n");
    printf("  elapsed     : %.4f s\n", elapsed);
    printf("  total ops   : %llu\n",   (unsigned long long)total_ops);
    printf("  total bytes : %.2f MB\n",(double)total_bytes / (1024*1024));
    printf("  throughput  : %.2f MB/s\n",
           elapsed > 0 ? (double)total_bytes / (1024*1024) / elapsed : 0.0);
    printf("  flushes     : %u\n",     flush_count);
    printf("  skipped     : %u\n",     skip_count);
    printf("  checksum    : 0x%016llx\n", (unsigned long long)checksum);
    printf("\n");
    sw_print_stats(&sw, stdout);

    /* Gear stats */
    printf("\n=== GearLock ===\n");
    printf("  cpu_ops=%u gpu_ops=%u worlds: cpu=%u gpu=%u\n",
           gear.cpu_ops, gear.gpu_ops,
           gear.cpu_worlds, gear.gpu_worlds);

    free(win_buf);
    return 0;
}
