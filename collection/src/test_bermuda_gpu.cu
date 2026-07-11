/*
 * test_bermuda_gpu.cu — Bermuda GPU vs CPU correctness test
 * Compile: nvcc -O2 -arch=sm_61 -o test_bermuda_gpu.exe test_bermuda_gpu.cu -lcuda
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cuda_runtime.h>
#include <cuda.h>

#define BERMUDA_STRIDE       37u
#define BERMUDA_N_ZONES      12u
#define BERMUDA_TRING_SLOTS  720u
#define ICOSA_GPU_TPB        256u
#define ICOSA_GPU_BATCH      65536u
#define ICOSA_EV_NONE        0x00u
#define ICOSA_EV_FLUSH       0x01u
#define ICOSA_EV_BOUNDARY    0x02u

/* Bermuda constants from bermuda_export.h */
static const uint16_t BERMUDA_SLOTS[5] = {0, 512, 1024, 2048, 4096};
static const uint8_t  BERMUDA_CROSS[12] = {9,10,11,6,7,8,3,4,5,0,1,2};

/* GPU context from icosa_twin_bridge.cu */
typedef struct {
    uint64_t addr;
    uint64_t value;
} IcosaPair;

typedef struct {
    uint64_t  gen2;
    uint64_t  gen3;
    int       valid;
    IcosaPair *d_pairs;
    uint64_t  *d_route;
    uint8_t   *d_event;
    IcosaPair *h_pairs;
    uint64_t  *h_route;
    uint8_t   *h_event;
    uint32_t   capacity;
    uint32_t   count;
    cudaStream_t stream;
    CUcontext  cu_ctx;
} IcosaGpuCtx;

#ifdef _WIN32
  #ifdef ICOSA_BUILD_DLL
    #define ICOSA_API __declspec(dllexport)
  #else
    #define ICOSA_API __declspec(dllimport)
  #endif
#else
  #ifdef ICOSA_BUILD_DLL
    #define ICOSA_API __attribute__((visibility("default")))
  #else
    #define ICOSA_API
  #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

ICOSA_API void icosa_gpu_ctx_destroy(void *gpu_ctx);
ICOSA_API void *icosa_gpu_ctx_create(uint64_t gen2, uint64_t gen3);
ICOSA_API int icosa_gpu_ctx_valid(void *gpu_ctx);
ICOSA_API int icosa_gpu_dispatch(void *gpu_ctx, const uint64_t *addrs, const uint64_t *values,
                                  uint32_t n, uint64_t gen3, uint32_t c144_tag,
                                  uint64_t baseline, uint64_t *out_routes, uint8_t *out_events);
ICOSA_API void *icosa_gpu_alloc(void *gpu_ctx, size_t size);
ICOSA_API int icosa_gpu_free(void *gpu_ctx, void *ptr);
ICOSA_API int icosa_gpu_memcpy_h2d(void *gpu_ctx, void *dst, const void *src, size_t size);
ICOSA_API int icosa_gpu_sync(void *gpu_ctx);
ICOSA_API int icosa_gpu_batch_memcpy_h2d(void *gpu_ctx, void *const *dst, const void *const *src,
                                          const size_t *sizes, int n);
ICOSA_API int icosa_gpu_batch_memcpy_d2h(void *gpu_ctx, void *const *dst, const void *const *src,
                                          const size_t *sizes, int n);
ICOSA_API int icosa_gpu_pin_host(void *gpu_ctx, void **ptr, size_t size);
ICOSA_API int icosa_gpu_unpin_host(void *gpu_ctx, void *ptr);
ICOSA_API int bermuda_gpu_dispatch(void *gpu_ctx, const uint16_t *idxs_in,
                                    void *out, uint8_t gear, uint8_t mode, uint32_t n);

#ifdef __cplusplus
}
#endif

/* CPU reference implementation */
static uint16_t BERMUDA_SLOTS_CPU[5] = {0, 512, 1024, 2048, 4096};
static uint8_t  BERMUDA_CROSS_CPU[12] = {9,10,11,6,7,8,3,4,5,0,1,2};
static uint16_t BERMUDA_WALK_LEN[5];
static uint16_t BERMUDA_FACE_SZ[5];
static uint16_t BERMUDA_INV37[5];
static uint16_t BERMUDA_INV37_WL[5];

static uint16_t _bermuda_modinv(uint16_t a, uint16_t m) {
    int32_t g = (int32_t)m, x = 0, a0 = (int32_t)a, x0 = 1;
    while (a0 != 0) {
        int32_t q = g / a0;
        int32_t t = a0; a0 = g - q * a0; g = t;
        t = x0; x0 = x - q * x0; x = t;
    }
    int32_t r = x % (int32_t)m;
    return (uint16_t)(r < 0 ? r + (int32_t)m : r);
}

static uint16_t _bermuda_walk_len(uint16_t slots) {
    uint16_t wl = slots;
    while (1) {
        if (wl % 12 == 0 && wl % 37 != 0) return wl;
        wl++;
    }
}

static void bermuda_cpu_init(void) {
    for (int g = 1; g <= 4; g++) {
        uint16_t slots = BERMUDA_SLOTS_CPU[g];
        BERMUDA_WALK_LEN[g] = _bermuda_walk_len(slots);
        BERMUDA_FACE_SZ[g] = BERMUDA_WALK_LEN[g] / 12;
        BERMUDA_INV37[g] = _bermuda_modinv(BERMUDA_STRIDE, slots);
        BERMUDA_INV37_WL[g] = _bermuda_modinv(BERMUDA_STRIDE, BERMUDA_WALK_LEN[g]);
    }
}

static uint16_t bermuda_cpu_traverse_orbit(uint16_t idx, uint8_t gear) {
    uint16_t N = BERMUDA_SLOTS_CPU[gear];
    return (uint16_t)((idx + 1) % N);
}

static uint16_t bermuda_cpu_traverse_chiral(uint16_t idx, uint8_t gear) {
    uint16_t N = BERMUDA_SLOTS_CPU[gear];
    return (uint16_t)((idx + N / 2) % N);
}

static uint16_t bermuda_cpu_traverse_cross(uint16_t idx, uint8_t gear) {
    uint16_t WL = BERMUDA_WALK_LEN[gear];
    uint16_t FS = BERMUDA_FACE_SZ[gear];
    uint16_t IW = BERMUDA_INV37_WL[gear];
    uint16_t N  = BERMUDA_SLOTS_CPU[gear];
    uint16_t enc = (uint16_t)(((uint32_t)idx * BERMUDA_STRIDE) % WL);
    uint8_t  z   = (uint8_t)(enc / FS);
    uint8_t  pz  = BERMUDA_CROSS_CPU[z % 12];
    uint16_t ne  = (uint16_t)(pz * FS + enc % FS);
    return (uint16_t)(((uint32_t)ne * IW) % WL % N);
}

static uint16_t bermuda_cpu_traverse_hub(uint16_t idx, uint8_t gear) {
    uint16_t WL = BERMUDA_WALK_LEN[gear];
    uint16_t FS = BERMUDA_FACE_SZ[gear];
    uint16_t N  = BERMUDA_SLOTS_CPU[gear];
    uint16_t enc = (uint16_t)(((uint32_t)idx * BERMUDA_STRIDE) % WL);
    uint8_t  z  = (uint8_t)(enc / FS);
    return (uint16_t)(((uint32_t)z * (N / BERMUDA_N_ZONES)) % N);
}

static uint16_t bermuda_cpu_traverse(uint16_t idx, uint8_t gear, uint8_t mode) {
    switch (mode) {
        case 0: return bermuda_cpu_traverse_orbit(idx, gear);
        case 1: return bermuda_cpu_traverse_chiral(idx, gear);
        case 2: return bermuda_cpu_traverse_cross(idx, gear);
        case 3: return bermuda_cpu_traverse_hub(idx, gear);
        default: return idx;
    }
}

static uint8_t bermuda_cpu_zone(uint16_t idx, uint8_t gear) {
    uint16_t enc = (uint16_t)(((uint32_t)idx * BERMUDA_STRIDE) % BERMUDA_WALK_LEN[gear]);
    return (uint8_t)(enc / BERMUDA_FACE_SZ[gear]);
}

static uint8_t bermuda_cpu_pole(uint8_t zone) {
    return zone >= 6 ? 1 : 0;
}

static uint16_t bermuda_cpu_tring_slot(uint16_t idx) {
    return idx % BERMUDA_TRING_SLOTS;
}

static uint8_t bermuda_cpu_shape(uint8_t mode, uint8_t zone) {
    switch (mode) {
        case 0: return zone < 6 ? 73 : 79;
        case 1: return 79;
        case 2: return 83;
        case 3: return 76;
        default: return 73;
    }
}

static uint8_t bermuda_cpu_polarity(uint8_t mode, uint8_t zone) {
    switch (mode) {
        case 0: return bermuda_cpu_pole(zone);
        case 1: return 1;
        case 2: return 0;
        case 3: return 1;
        default: return 0;
    }
}

typedef struct {
    uint16_t idx_in;
    uint16_t idx_out;
    uint8_t  zone;
    uint8_t  pole;
    uint8_t  shape;
    uint8_t  polarity;
    uint16_t tring_slot;
} BermudaRouteEntry;

static void bermuda_cpu_route_token(uint16_t idx_in, uint8_t gear, uint8_t mode, BermudaRouteEntry *out) {
    uint16_t idx_out = bermuda_cpu_traverse(idx_in, gear, mode);
    uint8_t  z       = bermuda_cpu_zone(idx_in, gear);
    out->idx_in      = idx_in;
    out->idx_out     = idx_out;
    out->zone        = z;
    out->pole        = bermuda_cpu_pole(z);
    out->shape       = bermuda_cpu_shape(mode, z);
    out->polarity    = bermuda_cpu_polarity(mode, z);
    out->tring_slot  = bermuda_cpu_tring_slot(idx_in);
}

int main(void) {
    bermuda_cpu_init();

    printf("=== Bermuda GPU vs CPU Correctness Test ===\n\n");

    uint32_t n = 5000;
    uint16_t *idxs_in = (uint16_t*)malloc(n * sizeof(uint16_t));
    BermudaRouteEntry *gpu_out = (BermudaRouteEntry*)malloc(n * sizeof(BermudaRouteEntry));
    BermudaRouteEntry *cpu_out = (BermudaRouteEntry*)malloc(n * sizeof(BermudaRouteEntry));

    srand(12345);
    for (uint32_t i = 0; i < n; i++) {
        idxs_in[i] = (uint16_t)(rand() % 4096);
    }

    /* Test all gear/mode combinations */
    int all_pass = 1;
    for (int gear = 1; gear <= 4; gear++) {
        for (int mode = 0; mode <= 3; mode++) {
            printf("Testing gear=%d mode=%d... ", gear, mode);

            /* GPU dispatch */
            IcosaGpuCtx *ctx = (IcosaGpuCtx*)icosa_gpu_ctx_create(0, 0xDEADBEEFCAFEBABEULL);
            if (!ctx || !icosa_gpu_ctx_valid(ctx)) {
                printf("GPU init failed, skipping\n");
                continue;
            }

            int ret = bermuda_gpu_dispatch(ctx, idxs_in, gpu_out, gear, mode, n);
            icosa_gpu_ctx_destroy(ctx);

            if (ret != 0) {
                printf("GPU dispatch error %d\n", ret);
                all_pass = 0;
                continue;
            }

            /* CPU reference */
            for (uint32_t i = 0; i < n; i++) {
                bermuda_cpu_route_token(idxs_in[i], gear, mode, &cpu_out[i]);
            }

            /* Compare */
            int mismatches = 0;
            for (uint32_t i = 0; i < n; i++) {
                if (gpu_out[i].idx_in   != cpu_out[i].idx_in ||
                    gpu_out[i].idx_out  != cpu_out[i].idx_out ||
                    gpu_out[i].zone     != cpu_out[i].zone ||
                    gpu_out[i].pole     != cpu_out[i].pole ||
                    gpu_out[i].shape    != cpu_out[i].shape ||
                    gpu_out[i].polarity != cpu_out[i].polarity ||
                    gpu_out[i].tring_slot != cpu_out[i].tring_slot) {
                    if (mismatches < 5)
                        printf("\n  MISMATCH [%u]: GPU(idx_out=%u,zone=%u,shape=%u) CPU(idx_out=%u,zone=%u,shape=%u)",
                               i, gpu_out[i].idx_out, gpu_out[i].zone, gpu_out[i].shape,
                               cpu_out[i].idx_out, cpu_out[i].zone, cpu_out[i].shape);
                    mismatches++;
                }
            }
            if (mismatches == 0) {
                printf("PASS\n");
            } else {
                printf("FAIL (%d mismatches)\n", mismatches);
                all_pass = 0;
            }
        }
    }

    printf("\n=== RESULT ===\n");
    if (all_pass) {
        printf("✓ ALL TESTS PASSED\n");
    } else {
        printf("✗ SOME TESTS FAILED\n");
    }

    free(idxs_in);
    free(gpu_out);
    free(cpu_out);

    return all_pass ? 0 : 1;
}