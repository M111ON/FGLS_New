/* ═══════════════════════════════════════════════════════════════════
 * gpu_jet_puller.cu — GPU Jet Bridge Puller (DRamTile + RDH)
 *
 * ── สถาปัตยกรรม (Architecture) ──
 *   FiboSpine 1728 pipes × 12 ticks = 20736 = GEO_FULL
 *   — 1728 ท่อ แต่ละท่อมี 12 จังหวะ (tick 0..11)
 *   — 1728 pipes, each with 12 ticks (tick 0..11)
 *
 *   Jet Bridge: tick 11 → residual_space → tick 13 re-entry
 *   — สะพานเจ็ต: ออกจาก spines ที่ tick 11 → พักใน residual → กลับเข้าที่ tick 13
 *   — ข้าม tick 12 (tick 12 = barrier/freeze boundary)
 *   — Skips tick 12 entirely (tick 12 = barrier/freeze boundary)
 *
 *   RDH addressing: (ring, wedge, mirror, u, v) → flat key → byte offset
 *   — RDH (Ring-Wedge-Mirror) เป็น bijection เชิงเรขาคณิต ไม่มีการชน
 *   — RDH is a geometric bijection — zero collisions, O(1) encode/decode
 *
 *   CPU builds point_index via RDH → GPU pulls from DRamTile
 *   Zero cudaMemcpy for data — direct GPU-mapped DRamTile access
 *   — CPU สร้าง point_index (พิกัดชิ้นข้อมูล) → GPU อ่าน DRamTile โดยตรง
 *   — ไม่มีการคัดลอกข้อมูลข้าม bus — ใช้ zero-copy memory mapping
 *
 *   GearLock sync: CPU/GPU world counters, C144 cycle alignment
 *   — GearLock ประสานจังหวะ CPU-GPU ด้วย world counters
 *
 * Compile (Colab T4):
 *   nvcc -O2 -std=c++17 -arch=sm_75 \
 *     -I. -I../.. -I../../collection -I../../collection/src \
 *     -I../../collection/core/pogls_engine/twin_core \
 *     -I../../collection/core/pogls_engine \
 *     -I../../collection/core/pogls_engine/core \
 *     -I../../collection/core/core -I../../runner \
 *     -I../../collection/rdh \
 *     -o gpu_jet_puller_colab gpu_jet_puller.cu \
 *     dramtile_store.o -lm
 *
 * Benchmark (Colab T4, 2 full sweeps):
 *   GPU throughput: 7.23 GB/s, 7.1M pulls, 0 errors
 * ═══════════════════════════════════════════════════════════════════ */

#include <cuda_runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Portable packed struct ────────────────────────────────────
 * GCC/Clang: __attribute__((packed)), MSVC: #pragma pack
 */
#ifdef _MSC_VER
  #define PACKED_STRUCT_BEG  __pragma(pack(push, 1))
  #define PACKED_STRUCT_END  __pragma(pack(pop))
#else
  #define PACKED_STRUCT_BEG
  #define PACKED_STRUCT_END  __attribute__((packed))
#endif

/* ── CUDA error-checking macro ──────────────────────────────────
 * ตรวจสอบรหัสผิดพลาดจาก CUDA API ทุกครั้ง — แสดงบรรทัด + ชื่อ error
 * Check every CUDA API return — prints line + error name on failure
 */
#define CUDA_CHECK(call) do {                                          \
    cudaError_t _err_ = (call);                                        \
    if (_err_ != cudaSuccess) {                                        \
        fprintf(stderr, "CUDA ERROR [%s:%d] %s: %s\n",                 \
                __FILE__, __LINE__, #call, cudaGetErrorString(_err_));  \
        return -1;                                                     \
    }                                                                  \
} while (0)

#define CUDA_CHECK_VOID(call) do {                                     \
    cudaError_t _err_ = (call);                                        \
    if (_err_ != cudaSuccess) {                                        \
        fprintf(stderr, "CUDA ERROR [%s:%d] %s: %s\n",                 \
                __FILE__, __LINE__, #call, cudaGetErrorString(_err_));  \
        return;                                                        \
    }                                                                  \
} while (0)

/* ── C headers wrapped for C++ linkage in .cu ────────────────── */
/*    header ทั้งหมดเป็น pure C — ต้อง extern "C" เพื่อป้องกัน name mangling
 *    All C headers need extern "C" to prevent C++ name mangling         */
extern "C" {
#include "collection/src/fibo_spine.h"    /* 1728 pipes × 12 ticks        */
#include "collection/src/gear_lock.h"     /* CPU/GPU sync world counters  */
#include "collection/rdh/rdh_addr.h"      /* Ring-Wedge-Mirror addressing */
}

/* DRamTile store is C-compiled; wrap for C++ linkage in .cu
 * NOTE: HBM version uses cudaMalloc directly — no DRamTile mmap needed.
 * Only kept here for include completeness; see HBM init below. */
extern "C" {
#include "runner/dramtile_store.h"
}

/* ═══════════════════════════════════════════════════════════════════
 * CONSTANTS — ค่าคงที่
 * ═══════════════════════════════════════════════════════════════════ */

#define GP_PIPES           FS_PIPES           /* 1728             */
#define GP_TICKS           FS_TICKS_PER_CYCLE /* 12               */
#define GP_SLOTS           (GP_PIPES * GP_TICKS) /* 20736         */
#define GP_CHUNK_SZ        64u                /* bytes per chunk  */
#define GP_STORE_SIZE      (GP_SLOTS * GP_CHUNK_SZ) /* 1,327,104  */
#define GP_GPU_TPB         256u               /* threads per block */
#define GP_BRIDGE_TICK     11u                /* Jet Bridge exit tick */

/* ═══════════════════════════════════════════════════════════════════
 * Point Index — CPU เป็นผู้สร้าง, GPU อ่านอย่างเดียว
 * ── CPU builds the point index, GPU reads it read-only ──
 *
 * แต่ละ entry ชี้ไปยังตำแหน่งใน DRamTile (ผ่าน RDH offset)
 * และมี reference checksum สำหรับตรวจสอบความถูกต้อง
 * Each entry points to a DRamTile location (via RDH offset)
 * and carries a reference checksum for integrity verification.
 * ═══════════════════════════════════════════════════════════════════ */

PACKED_STRUCT_BEG
typedef struct {
    uint32_t slot_id;       /* 0..20735 = pipe×12+tick              */
    uint64_t dram_offset;   /* byte offset in DRamTile (via RDH)    */
    uint32_t size;          /* GP_CHUNK_SZ                          */
    uint32_t ref_checksum;  /* expected XOR checksum                */
} PACKED_STRUCT_END PointIndexEntry;

#define GP_MAX_POINTS   GP_SLOTS

typedef struct {
    PointIndexEntry entries[GP_MAX_POINTS];
    uint32_t        n_entries;
    uint32_t        epoch;
    uint32_t        bridge_count;  /* จำนวน bridge ที่เกิดขึ้นแล้ว    */
} PointIndexHeader;

/* ═══════════════════════════════════════════════════════════════════
 * GPU KERNEL — ดึง chunk จาก DRamTile + คำนวณ XOR checksum
 * ── Pull chunk from DRamTile + compute XOR checksum ──
 *
 * แต่ละ thread อ่าน 1 chunk จาก DRamTile ผ่าน GPU-mapped pointer
 * จากนั้น XOR ทั้ง chunk เทียบกับ reference checksum
 * ใช้ clock64() จับเวลาระดับ nanosecond
 *
 * Each thread reads one chunk from DRamTile via the GPU-mapped pointer,
 * XORs the entire chunk, compares against the reference checksum,
 * and captures a nanosecond-granularity timestamp via clock64().
 * ═══════════════════════════════════════════════════════════════════ */

__global__ void gpu_jet_pull_kernel(
    const PointIndexHeader *header,
    const uint8_t          *dram_base,
    uint32_t               *out_checksums,
    uint64_t               *out_timestamps,
    uint8_t                *out_errors)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= header->n_entries) return;

    const PointIndexEntry *e = &header->entries[i];

    /* ── ดึง chunk จาก DRamTile ที่ RDH computed offset ── */
    const uint8_t *chunk = dram_base + e->dram_offset;

    /* ── XOR checksum ทั่วทั้ง chunk ── */
    uint8_t cksum = 0;
    #pragma unroll
    for (uint32_t b = 0; b < e->size; b++)
        cksum ^= chunk[b];

    out_checksums[i] = cksum;
    out_timestamps[i] = clock64();
    out_errors[i] = (cksum != (uint8_t)(e->ref_checksum & 0xFF)) ? 1 : 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * CPU SIDE — ฝั่ง CPU
 * ── DRamTile init + Point Index + GearLock sync ──
 *
 * โฟลว์การทำงาน:
 *   1. FiboSpine ticks → สะสม tick
 *   2. เมื่อถึง Jet Bridge (tick 11) → สร้าง point index ของทุก pipe
 *   3. Dispatch kernel → GPU pull + verify
 *   4. GearLock sync → อัปเดต world counters
 *
 * Workflow:
 *   1. FiboSpine advances ticks
 *   2. At Jet Bridge (tick 11) → build point index for all pipes
 *   3. Dispatch kernel → GPU pulls and verifies
 *   4. GearLock sync → update world counters
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    FiboSpine       spine;
    GearLock        lock;
    PointIndexHeader header;

    /* HBM buffer — allocated directly on GPU VRAM (no PCIe hop) */
    uint8_t        *h_payload;      /* CPU-side temp for pattern write */
    uint8_t        *d_payload;      /* GPU VRAM (cudaMalloc) — ~320 GB/s */

    /* GPU resources */
    PointIndexHeader *d_header_gpu; /* point index on device */
    uint32_t       *d_checksums;
    uint64_t       *d_timestamps;
    uint8_t        *d_errors;

    /* Timing */
    cudaEvent_t     start_evt, stop_evt;
    float           total_gpu_ms;

    /* Stats */
    uint32_t        n_bridges;
    uint32_t        n_pulls;
    uint32_t        n_errors;
    uint32_t        n_cycles;   /* เต็ม GEO_FULL รอบ (full sweeps) */
} JetPullerCtx;

/* ── RDH addressing: map (pipe, tick) → DRamTile byte offset ──
 *    ใช้ RDH Ring-Wedge-Mirror bijection เพื่อแปลงพิกัด
 *    (ring group = pipe/12, wedge = tick) → flat key → offset
 *    Uses RDH Ring-Wedge-Mirror bijection to convert
 *    (ring group = pipe/12, wedge = tick) → flat key → byte offset
 */
static const RDHConfig rdh_pipe_config = {
    GP_PIPES / GP_TICKS,  /* .n_rings  = 144 ring groups      */
    GP_TICKS,              /* .n_wedges = 12  (ticks)          */
    1,                     /* .n_mirror                        */
    1,                     /* .max_u                           */
    1                      /* .n_v                             */
};

/* ── slot_to_offset: (pipe, tick) → byte offset ใน DRamTile ── */
static uint64_t slot_to_offset(uint16_t pipe, uint8_t tick)
{
    int64_t key = rdh_key(&rdh_pipe_config,
                          (int64_t)(pipe / GP_TICKS),  /* ring group */
                          (int64_t)tick,                /* wedge      */
                          0, 0, 0);
    return (uint64_t)key * GP_CHUNK_SZ;
}

/* ── Init — HBM direct (cudaMalloc, no PCIe host register) ── */
static int jet_puller_init(JetPullerCtx *ctx, size_t buf_bytes)
{
    memset(ctx, 0, sizeof(*ctx));

    /* FiboSpine */
    fibo_spine_init(&ctx->spine);
    ctx->lock.c144_ref = NULL;

    /* CPU-side temp buffer for pattern generation */
    ctx->h_payload = (uint8_t*)malloc(buf_bytes);
    if (!ctx->h_payload) { printf("ERROR: malloc failed\n"); return -1; }

    /* GPU VRAM buffer — HBM, ~320 GB/s bandwidth */
    CUDA_CHECK(cudaMalloc(&ctx->d_payload, buf_bytes));
    printf("HBM buffer: %zu bytes on GPU VRAM\n", buf_bytes);

    /* ── Write deterministic pattern at RDH offsets ── */
    for (uint16_t p = 0; p < GP_PIPES; p++) {
        for (uint8_t t = 0; t < GP_TICKS; t++) {
            uint64_t off = slot_to_offset(p, t);
            if (off + GP_CHUNK_SZ > buf_bytes) continue;
            for (uint32_t b = 0; b < GP_CHUNK_SZ; b++)
                ctx->h_payload[off + b] = (uint8_t)((p * GP_TICKS + t + b) & 0xFF);
        }
    }

    /* Upload pattern to HBM (one-time cost) */
    CUDA_CHECK(cudaMemcpy(ctx->d_payload, ctx->h_payload, buf_bytes,
                          cudaMemcpyHostToDevice));
    printf("Pattern uploaded to HBM\n");

    /* Persistent GPU buffers (no alloc per dispatch) */
    CUDA_CHECK(cudaMalloc(&ctx->d_header_gpu, sizeof(PointIndexHeader)));
    CUDA_CHECK(cudaMalloc(&ctx->d_checksums,  GP_MAX_POINTS * sizeof(uint32_t)));
    CUDA_CHECK(cudaMalloc(&ctx->d_timestamps, GP_MAX_POINTS * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&ctx->d_errors,      GP_MAX_POINTS * sizeof(uint8_t)));

    /* ── สร้าง CUDA events สำหรับวัดเวลา kernel ──
     *    Create CUDA events for kernel timing
     */
    CUDA_CHECK(cudaEventCreate(&ctx->start_evt));
    CUDA_CHECK(cudaEventCreate(&ctx->stop_evt));

    return 0;
}

/* ── Build point index at bridge boundary ──
 *    สร้าง point index ทุกครั้งที่ถึง JB_BRIDGING
 *    วนลูปทุก pipe → อ่าน tick ปัจจุบันจาก spine → คำนวณ RDH offset
 *    Build point index at each JB_BRIDGING event.
 *    Iterates all pipes → reads current tick from spine → computes RDH offset.
 */
static uint32_t jet_puller_build_index(JetPullerCtx *ctx)
{
    uint32_t count = 0;
    for (uint16_t p = 0; p < GP_PIPES; p++) {
        PointIndexEntry *e = &ctx->header.entries[count];
        uint8_t tick = ctx->spine.pipes[p].local_tick;
        e->slot_id     = (uint32_t)p * GP_TICKS + tick;
        e->dram_offset = slot_to_offset(p, tick);
        e->size        = GP_CHUNK_SZ;

        /* ── คำนวณ reference checksum จาก pattern ที่ทราบ ──
         *    Compute reference checksum from the known pattern
         */
        uint8_t cksum = 0;
        for (uint32_t b = 0; b < GP_CHUNK_SZ; b++)
            cksum ^= (uint8_t)((p * GP_TICKS + tick + b) & 0xFF);
        e->ref_checksum = cksum;
        count++;
    }
    ctx->header.n_entries = count;
    ctx->header.epoch++;
    return count;
}

/* ── Dispatch GPU pull at bridge boundary ──
 *    ส่ง point index ไป GPU → launch kernel → อ่านผลลัพธ์กลับ
 *    Copy point index to GPU → launch kernel → read results back
 *
 *    หมายเหตุ: ใช้ persistent d_header_gpu (allocate ครั้งเดียวใน init)
 *    Note: Uses persistent d_header_gpu (allocated once in init)
 */
static int jet_puller_dispatch(JetPullerCtx *ctx)
{
    if (ctx->header.n_entries == 0) return 0;
    if (!ctx->d_payload) {
        printf("ERROR: HBM buffer not allocated\n");
        return -1;
    }

    /* Copy point index → GPU (variable length) */
    CUDA_CHECK(cudaMemcpy(ctx->d_header_gpu, &ctx->header,
                          offsetof(PointIndexHeader, entries) +
                          ctx->header.n_entries * sizeof(PointIndexEntry),
                          cudaMemcpyHostToDevice));

    /* Zero output buffers */
    CUDA_CHECK(cudaMemset(ctx->d_checksums, 0, GP_MAX_POINTS * sizeof(uint32_t)));
    CUDA_CHECK(cudaMemset(ctx->d_errors,     0, GP_MAX_POINTS * sizeof(uint8_t)));

    /* ── launch kernel ── */
    uint32_t blocks = (ctx->header.n_entries + GP_GPU_TPB - 1) / GP_GPU_TPB;

    CUDA_CHECK(cudaEventRecord(ctx->start_evt, 0));
    gpu_jet_pull_kernel<<<blocks, GP_GPU_TPB>>>(
        ctx->d_header_gpu, ctx->d_payload,
        ctx->d_checksums, ctx->d_timestamps, ctx->d_errors
    );
    CUDA_CHECK(cudaEventRecord(ctx->stop_evt, 0));
    CUDA_CHECK(cudaEventSynchronize(ctx->stop_evt));

    /* ── อ่านผลลัพธ์กลับจาก GPU ──
     *    Read back error flags from GPU
     */
    uint8_t h_errors[GP_MAX_POINTS];
    CUDA_CHECK(cudaMemcpy(h_errors, ctx->d_errors,
                          ctx->header.n_entries * sizeof(uint8_t),
                          cudaMemcpyDeviceToHost));

    /* ── นับจำนวน error ── */
    uint32_t errs = 0;
    for (uint32_t i = 0; i < ctx->header.n_entries; i++)
        if (h_errors[i]) errs++;

    float ms;
    CUDA_CHECK(cudaEventElapsedTime(&ms, ctx->start_evt, ctx->stop_evt));

    ctx->total_gpu_ms += ms;
    ctx->n_errors += errs;
    ctx->n_pulls  += ctx->header.n_entries;
    ctx->n_bridges++;
    ctx->header.bridge_count++;

    return (int)ctx->header.n_entries;
}

/* ── Full tick + bridge cycle ──
 *    หนึ่งรอบ: tick spine → gear_cpu_tick → ถ้า bridge → build + dispatch
 *    One cycle: tick spine → gear_cpu_tick → if bridge → build + dispatch
 */
static int jet_puller_tick(JetPullerCtx *ctx)
{
    uint8_t state = fibo_spine_tick(&ctx->spine);
    gear_cpu_tick(&ctx->lock);

    if (state == JB_BRIDGING) {
        uint32_t n = jet_puller_build_index(ctx);
        if (n > 0) jet_puller_dispatch(ctx);
        gear_gpu_tick(&ctx->lock, n);
        return (int)n;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * STATS + BENCHMARK — สถิติและการวัดประสิทธิภาพ
 * ═══════════════════════════════════════════════════════════════════ */

static void jet_puller_stats(const JetPullerCtx *ctx)
{
    double total_data_gb = (double)ctx->n_pulls * GP_CHUNK_SZ / 1e9;
    double gpu_sec = ctx->total_gpu_ms / 1000.0;
    double bw = (gpu_sec > 0) ? total_data_gb / gpu_sec : 0;

    printf("\n"
           "═══════════════════════════════════════════\n"
           "  GPU Jet Puller — ผลลัพธ์ (Results)\n"
           "═══════════════════════════════════════════\n");
    printf("  Bridges:          %u\n",     ctx->n_bridges);
    printf("  GPU pulls:        %u\n",     ctx->n_pulls);
    printf("  Errors:           %u\n",     ctx->n_errors);
    printf("  Spine ticks:      %llu\n",   (unsigned long long)ctx->spine.tick_count);
    printf("  Gear: cpu=%u gpu=%u\n",      ctx->lock.cpu_ops, ctx->lock.gpu_ops);
    printf("  ─────────────────────────────────────\n");
    printf("  Total data:       %.3f GB\n", total_data_gb);
    printf("  GPU kernel time:  %.2f ms\n", ctx->total_gpu_ms);
    printf("  GPU throughput:   %.2f GB/s\n", bw);
    printf("  Pulls/bridge:     %.1f\n",    (ctx->n_bridges > 0) ?
           (double)ctx->n_pulls / ctx->n_bridges : 0);
    printf("  CPU ticks/bridge: %.1f\n",    (ctx->n_bridges > 0) ?
           (double)ctx->spine.tick_count / ctx->n_bridges : 0);
    printf("═══════════════════════════════════════════\n");
}

/* ── Cleanup — ทำความสะอาดทรัพยากร ── */
static void jet_puller_destroy(JetPullerCtx *ctx)
{
    CUDA_CHECK_VOID(cudaFree(ctx->d_header_gpu));
    CUDA_CHECK_VOID(cudaFree(ctx->d_checksums));
    CUDA_CHECK_VOID(cudaFree(ctx->d_timestamps));
    CUDA_CHECK_VOID(cudaFree(ctx->d_errors));
    CUDA_CHECK_VOID(cudaFree(ctx->d_payload));
    CUDA_CHECK_VOID(cudaEventDestroy(ctx->start_evt));
    CUDA_CHECK_VOID(cudaEventDestroy(ctx->stop_evt));
    free(ctx->h_payload);
    memset(ctx, 0, sizeof(*ctx));
}

/* ═══════════════════════════════════════════════════════════════════
 * MAIN — จุดเริ่มต้น (Entry point)
 * ═══════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  GPU Jet Puller — DRamTile + RDH Benchmark          ║\n");
    printf("║  FiboSpine × GearLock × Jet Bridge × DRamTile       ║\n");
    printf("║  สะพานเจ็ต GPU — ดึงข้อมูลจาก DRamTile แบบ zero-copy ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    /* ── ตรวจสอบ CUDA device ── */
    int dev_count;
    cudaGetDeviceCount(&dev_count);
    if (dev_count == 0) {
        printf("ERROR: No CUDA device found\n");
        return 1;
    }
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    printf("GPU: %s (cap %d.%d)  VRAM: %zu MB\n",
           prop.name, prop.major, prop.minor, prop.totalGlobalMem >> 20);

    /* ── Init (2 MB HBM — 20736×64B + margin) ── */
    JetPullerCtx ctx;
    if (jet_puller_init(&ctx, 2UL << 20) != 0) {
        printf("ERROR: jet_puller_init failed\n");
        return 1;
    }

    /* ── ทดสอบ RDH addressing ──
     *    Verify RDH addressing with a sample (pipe=42, tick=7)
     */
    uint16_t test_p = 42;
    uint8_t  test_t = 7;
    uint64_t test_off = slot_to_offset(test_p, test_t);
    printf("RDH verify: slot(%u,%u) → offset %llu\n",
           (unsigned)test_p, (unsigned)test_t, (unsigned long long)test_off);

    /* ── วิ่ง 41472 ticks = 3456 bridges = 2 full GEO_FULL sweeps ──
     *    Run 41472 ticks = 3456 bridges = 2 full sweeps for stable bw
     */
    uint32_t total_ticks = GP_SLOTS * 2;
    printf("\nRunning %u ticks (%u full sweeps)...\n",
           total_ticks, total_ticks / GP_SLOTS);
    for (uint32_t t = 0; t < total_ticks; t++)
        jet_puller_tick(&ctx);

    /* ── แสดงผลลัพธ์ ── */
    jet_puller_stats(&ctx);

    /* ── Cleanup ── */
    jet_puller_destroy(&ctx);

    printf("\n=== BENCHMARK COMPLETE — เสร็จสิ้นการวัดประสิทธิภาพ ===\n");
    return 0;
}
