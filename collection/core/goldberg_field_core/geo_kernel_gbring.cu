/*
 * geo_kernel_gbring.cu — Phase 13 GPU: GBTRingPrint → geo_cube_dispatch
 * ═══════════════════════════════════════════════════════════════════════
 * Feeds GBTRingPrint fingerprints into the existing FrustumSlot64 pipeline.
 *
 * Mapping (GBTRingPrint → FrustumSlot64):
 *   core[0] = (uint64)spatial[0] | ((uint64)spatial[1] << 32)
 *   core[1] = (uint64)spatial[2] | ((uint64)spatial[3] << 32)
 *   core[2] = (uint64)tring_enc  | ((uint64)tring_pos  << 32)
 *   core[3] = (uint64)stamp      | ((uint64)scan_id lower 32b << 32)
 *   addr[0] = stamp
 *   addr[1] = tring_pos | (tring_pair_pos << 16)
 *   addr[2] = max_tension
 *   addr[3] = scan_id lower 32b
 *   dir     = active_pair
 *   axis    = active_pole
 *   coset   = blockIdx.x (auto)
 *   frustum = max_tension_gap
 *   checksum= XOR fold core[0..3] lower 32b
 *
 * Launch: <<<12, 32>>> per batch of N prints
 *   Each block handles N/12 prints (round-robin coset assignment)
 *   mode = K_MODE_TRANSFORM (derive next + write back)
 *
 * Bench target: >15,000 M-pkt/s kernel-only (matches K7/K8 regime)
 *
 * Build:
 *   nvcc -O2 -arch=sm_61 -o bench_gbring geo_kernel_gbring.cu
 *   nvcc -O2 -arch=sm_75 -o bench_gbring geo_kernel_gbring.cu  (T4)
 * ═══════════════════════════════════════════════════════════════════════
 */

#define _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH
#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <cuda_runtime.h>

/* ── Constants (self-contained, mirrors geo_kernel_whe.cu) ── */
#define K_COSET_COUNT    12u
#define K_FRUSTUM_MOUNT   6u
#define K_SLOT_BYTES     64u
#define K_COSET_BYTES    (K_FRUSTUM_MOUNT * K_SLOT_BYTES)   /* 384B */
#define K_PAYLOAD_BYTES  (K_COSET_COUNT   * K_COSET_BYTES)  /* 4608B */
#define K_THREADS        32u
#define K_MODE_TRANSFORM  1u

#define FSLOT_CORE0_OFF   0u
#define FSLOT_ADDR0_OFF  32u
#define FSLOT_DIR_OFF    48u
#define FSLOT_AXIS_OFF   49u
#define FSLOT_COSET_OFF  50u
#define FSLOT_FRUST_OFF  51u
#define FSLOT_WORLD_OFF  52u
#define FSLOT_CRC_OFF    56u
#define FSLOT_PAD_OFF    60u

static_assert(K_PAYLOAD_BYTES == 4608u, "payload must be 4608B");

/* ── GBTRingPrint (device-side mirror — no host header dep) ── */
typedef struct {
    uint32_t  spatial[32];
    uint32_t  max_tension;
    uint8_t   max_tension_gap;
    uint8_t   active_pair;
    uint8_t   active_pole;
    uint16_t  tring_pos;
    uint32_t  tring_enc;
    uint16_t  tring_pair_pos;
    uint32_t  stamp;
    uint64_t  scan_id;
} GBTRingPrintD;

/* ── Device helpers ── */
__device__ __forceinline__
void _wr64(uint8_t *p, uint32_t off, uint64_t v) { memcpy(p+off, &v, 8); }
__device__ __forceinline__
void _wr32(uint8_t *p, uint32_t off, uint32_t v) { memcpy(p+off, &v, 4); }
__device__ __forceinline__
uint64_t _rd64(const uint8_t *p, uint32_t off) { uint64_t v; memcpy(&v,p+off,8); return v; }

__device__ __forceinline__
uint64_t _mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31; return x;
}
__device__ __forceinline__
uint64_t _gb_rotl64(uint64_t x, int r) { return (x << r) | (x >> (64-r)); }

/* ── Pack GBTRingPrint → FrustumSlot64 (device) ── */
__device__ __forceinline__
void gbring_pack_slot(const GBTRingPrintD *fp, uint8_t *slot, uint8_t coset_id)
{
    uint64_t core0 = (uint64_t)fp->spatial[0] | ((uint64_t)fp->spatial[1] << 32);
    uint64_t core1 = (uint64_t)fp->spatial[2] | ((uint64_t)fp->spatial[3] << 32);
    uint64_t core2 = (uint64_t)fp->tring_enc  | ((uint64_t)fp->tring_pos  << 32);
    uint64_t core3 = (uint64_t)fp->stamp      | ((uint64_t)(fp->scan_id & 0xFFFFFFFFu) << 32);

    _wr64(slot, FSLOT_CORE0_OFF +  0, core0);
    _wr64(slot, FSLOT_CORE0_OFF +  8, core1);
    _wr64(slot, FSLOT_CORE0_OFF + 16, core2);
    _wr64(slot, FSLOT_CORE0_OFF + 24, core3);

    _wr32(slot, FSLOT_ADDR0_OFF +  0, fp->stamp);
    _wr32(slot, FSLOT_ADDR0_OFF +  4,
          (uint32_t)fp->tring_pos | ((uint32_t)fp->tring_pair_pos << 16));
    _wr32(slot, FSLOT_ADDR0_OFF +  8, fp->max_tension);
    _wr32(slot, FSLOT_ADDR0_OFF + 12, (uint32_t)(fp->scan_id & 0xFFFFFFFFu));

    slot[FSLOT_DIR_OFF]   = fp->active_pair;
    slot[FSLOT_AXIS_OFF]  = fp->active_pole;
    slot[FSLOT_COSET_OFF] = coset_id;
    slot[FSLOT_FRUST_OFF] = fp->max_tension_gap;

    /* world[0..3] = spatial fold bytes */
    uint32_t sfold = fp->spatial[0] ^ fp->spatial[1] ^ fp->spatial[2] ^ fp->spatial[3];
    slot[FSLOT_WORLD_OFF+0] = (uint8_t)(sfold);
    slot[FSLOT_WORLD_OFF+1] = (uint8_t)(sfold >> 8);
    slot[FSLOT_WORLD_OFF+2] = (uint8_t)(sfold >> 16);
    slot[FSLOT_WORLD_OFF+3] = (uint8_t)(sfold >> 24);

    /* checksum = XOR fold lower 32b of core[0..3] */
    uint32_t crc = (uint32_t)(core0) ^ (uint32_t)(core1)
                 ^ (uint32_t)(core2) ^ (uint32_t)(core3);
    _wr32(slot, FSLOT_CRC_OFF, crc);
    _wr32(slot, FSLOT_PAD_OFF, 0u);
}

/* ── Derive step (mirrors geo_frustum_dispatch derive rule) ── */
__device__ __forceinline__
void gbring_derive_slot(uint8_t *slot, uint8_t face_id)
{
    uint64_t c0 = _rd64(slot, FSLOT_CORE0_OFF +  0);
    uint64_t c3 = _rd64(slot, FSLOT_CORE0_OFF + 24);
    uint64_t next = _mix64(c0 ^ _gb_rotl64(c3, 18) ^ ((uint64_t)face_id << 32));
    _wr64(slot, FSLOT_CORE0_OFF, next);
    /* update checksum */
    uint64_t c1 = _rd64(slot, FSLOT_CORE0_OFF +  8);
    uint64_t c2 = _rd64(slot, FSLOT_CORE0_OFF + 16);
    uint32_t crc = (uint32_t)next ^ (uint32_t)c1 ^ (uint32_t)c2 ^ (uint32_t)c3;
    _wr32(slot, FSLOT_CRC_OFF, crc);
}

/* ══════════════════════════════════════════════════════════════════
 * geo_gbring_dispatch — main kernel
 *
 * Input:  d_prints  — array of N GBTRingPrintD (device)
 *         d_payload — 4608B workspace (device, zeroed by caller)
 *         d_out     — 4608B output   (device)
 *         n_prints  — total fingerprints in batch
 *
 * Grid: <<<12, 32>>>
 *   blockIdx.x  = coset (0..11)
 *   threadIdx.x = thread within warp (0..31)
 *
 * Each thread processes prints where (print_idx % 12 == coset)
 * and face = thread_id % K_FRUSTUM_MOUNT
 * ══════════════════════════════════════════════════════════════════ */
__global__
void geo_gbring_dispatch(const GBTRingPrintD * __restrict__ d_prints,
                               uint8_t        * __restrict__ d_payload,
                               uint8_t        * __restrict__ d_out,
                               uint32_t                      n_prints)
{
    uint32_t coset     = blockIdx.x;
    uint32_t thread_id = threadIdx.x;
    uint8_t  face_id   = (uint8_t)(thread_id % K_FRUSTUM_MOUNT);

    uint32_t coset_base = coset * K_COSET_BYTES;
    uint32_t face_off   = face_id * K_SLOT_BYTES;
    uint32_t slot_off   = coset_base + face_off;

    /* iterate over prints assigned to this coset */
    for (uint32_t i = coset; i < n_prints; i += K_COSET_COUNT) {
        /* pack fingerprint into payload slot */
        gbring_pack_slot(&d_prints[i], d_payload + slot_off, (uint8_t)coset);
        __syncwarp();

        /* derive step */
        gbring_derive_slot(d_payload + slot_off, face_id);
        __syncwarp();

        /* write to output */
        memcpy(d_out + slot_off, d_payload + slot_off, K_SLOT_BYTES);
    }
}

/* ── Host launch ── */
cudaError_t gbring_kernel_launch(const GBTRingPrintD *d_prints,
                                        uint8_t       *d_payload,
                                        uint8_t       *d_out,
                                        uint32_t       n_prints)
{
    dim3 grid(K_COSET_COUNT, 1, 1);
    dim3 block(K_THREADS,    1, 1);
    geo_gbring_dispatch<<<grid, block>>>(d_prints, d_payload, d_out, n_prints);
    cudaDeviceSynchronize(); // Ensure kernel completion before returning
    return cudaGetLastError(); // Return any error encountered
}

/* ══════════════════════════════════════════════════════════════════
 * Benchmark — host driver
 * ══════════════════════════════════════════════════════════════════ */
#define BENCH_N       (1 << 20)   /* 1M fingerprints */
#define BENCH_REPS    10
#define WARMUP_REPS    3

static GBTRingPrintD h_prints[BENCH_N];

static void fill_prints(void) {
    uint64_t x = 0xdeadbeefcafe0001ULL;
    for (int i = 0; i < BENCH_N; i++) {
        x ^= x>>17; x ^= x<<31; x ^= x>>8;
        for (int j = 0; j < 4; j++) h_prints[i].spatial[j] = (uint32_t)(x >> (j*8));
        h_prints[i].stamp           = (uint32_t)x;
        h_prints[i].tring_enc       = (uint32_t)(x >> 13);
        h_prints[i].tring_pos       = (uint16_t)(x % 720);
        h_prints[i].tring_pair_pos  = (uint16_t)((x>>3) % 720);
        h_prints[i].max_tension     = (uint32_t)(x >> 7);
        h_prints[i].max_tension_gap = (uint8_t)(x % 60);
        h_prints[i].active_pair     = (uint8_t)(x % 6);
        h_prints[i].active_pole     = (uint8_t)(x & 1);
        h_prints[i].scan_id         = x ^ (x<<17);
    }
}

int main(void) {
    fill_prints();

    /* device alloc */
    GBTRingPrintD *d_prints  = NULL;
    uint8_t       *d_payload = NULL;
    uint8_t       *d_out     = NULL;
    size_t prints_bytes = (size_t)BENCH_N * sizeof(GBTRingPrintD);
    cudaError_t status;

    // Initialize variables at the very beginning of main
    float best_ms = 1e9f;
    bool error_occurred = false;
    cudaEvent_t ev0 = 0, ev1 = 0; // Initialize to 0 for safety

    // Setup phase errors: set flag and goto cleanup
    status = cudaMalloc(&d_prints,  prints_bytes); if (status != cudaSuccess) { error_occurred = true; goto cleanup; }
    status = cudaMalloc(&d_payload, K_PAYLOAD_BYTES); if (status != cudaSuccess) { error_occurred = true; goto cleanup; }
    status = cudaMalloc(&d_out,     K_PAYLOAD_BYTES); if (status != cudaSuccess) { error_occurred = true; goto cleanup; }
    
    status = cudaMemcpy(d_prints, h_prints, prints_bytes, cudaMemcpyHostToDevice);
    if (status != cudaSuccess) { error_occurred = true; goto cleanup; }

    /* warmup */
    for (int r = 0; r < WARMUP_REPS; r++) {
        status = cudaMemset(d_payload, 0, K_PAYLOAD_BYTES); if (status != cudaSuccess) { error_occurred = true; goto cleanup; }
        status = gbring_kernel_launch(d_prints, d_payload, d_out, BENCH_N); if (status != cudaSuccess) { error_occurred = true; goto cleanup; }
    }

    /* bench */
    status = cudaEventCreate(&ev0); if (status != cudaSuccess) { error_occurred = true; goto cleanup; }
    status = cudaEventCreate(&ev1); if (status != cudaSuccess) { error_occurred = true; goto cleanup; }

    for (int r = 0; r < BENCH_REPS; r++) {
        // Benchmark loop errors set flag and break
        status = cudaMemset(d_payload, 0, K_PAYLOAD_BYTES);
        if (status != cudaSuccess) { error_occurred = true; break; }

        status = cudaEventRecord(ev0);
        if (status != cudaSuccess) { error_occurred = true; break; }

        status = gbring_kernel_launch(d_prints, d_payload, d_out, BENCH_N);
        if (status != cudaSuccess) { error_occurred = true; break; }

        status = cudaEventRecord(ev1);
        if (status != cudaSuccess) { error_occurred = true; break; }

        status = cudaEventSynchronize(ev1);
        if (status != cudaSuccess) { error_occurred = true; break; }

        float ms;
        status = cudaEventElapsedTime(&ms, ev0, ev1);
        if (status != cudaSuccess) { error_occurred = true; break; }

        if (ms < best_ms) best_ms = ms;
    }

    if (!error_occurred) {
        double mpkt_s = (double)BENCH_N / (best_ms * 1e-3) / 1e6;
        printf("=== geo_gbring_dispatch bench (N=%dM, best/%d) ===\n",
               BENCH_N>>20, BENCH_REPS);
        printf("  best:   %.3f ms\n", best_ms);
        printf("  M-pkt/s: %.1f\n",  mpkt_s);
        printf("  target:  15,000 M-pkt/s\n");
        printf("  ratio:   %.3fx\n", mpkt_s / 15000.0);
    } else {
        printf("Benchmark failed with CUDA error: %s\n", cudaGetErrorString(status));
    }

cleanup: // Label for goto statements (used for cleanup)
    // Free device memory
    cudaFree(d_prints);
    cudaFree(d_payload);
    cudaFree(d_out);
    // Destroy CUDA events only if they were successfully created
    if (ev0) cudaEventDestroy(ev0);
    if (ev1) cudaEventDestroy(ev1);
    
    // Return non-zero exit code if any error occurred
    return error_occurred ? 1 : 0;
}
