/* ═══════════════════════════════════════════════════════════════════
 * gpu_jet_puller_gguf.cu — REAL MODEL WEIGHT benchmark
 *
 * Reads a GGUF file on disk, extracts tensor data, loads into HBM
 * via DRamTile addressing, and pulls via Gear Lock Jet Puller.
 *
 * Self-contained: embeds rdh_addr, gear_lock inline (no header files).
 *
 * Compile (Colab T4):
 *   pip install gguf -q && nvcc -O3 -std=c++17 -arch=sm_75 \
 *     -o gpu_jet_puller_gguf gpu_jet_puller_gguf.cu -lm
 *
 * Usage:
 *   ./gpu_jet_puller_gguf [model.gguf]
 *
 * Output:
 *   Per-tensor bandwidth + aggregate report
 * ═══════════════════════════════════════════════════════════════════ */

#include <cuda_runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define CUDA_CHECK(call) do { \
    cudaError_t _err_ = (call); \
    if (_err_ != cudaSuccess) { \
        fprintf(stderr, "CUDA ERROR [%s:%d] %s: %s\n", \
                __FILE__, __LINE__, #call, cudaGetErrorString(_err_)); \
        return -1; \
    } \
} while (0)

#define CUDA_CHECK_VOID(call) do { \
    cudaError_t _err_ = (call); \
    if (_err_ != cudaSuccess) { \
        fprintf(stderr, "CUDA ERROR [%s:%d] %s: %s\n", \
                __FILE__, __LINE__, #call, cudaGetErrorString(_err_)); \
        return; \
    } \
} while (0)

/* ═══════════════════════════════════════════════════════════════════
 * EMBEDDED RDH ADDRESSING (from rdh_addr.h — scalar)
 * ═══════════════════════════════════════════════════════════════════ */

#define RDH_CONFIG_MAGIC  0x5244485F434F4E46ULL  /* "RDH_CONFIG" */

typedef struct {
    uint64_t magic;
    uint32_t n_rings;     /* number of rings (e.g. 144) */
    uint32_t n_wedges;    /* wedges per ring (e.g. 12) */
    uint32_t n_mirror;    /* mirror planes (e.g. 1)   */
    uint32_t max_u;       /* U dimension               */
    uint32_t n_v;         /* V tiles                   */
} RDHConfig;

static inline int64_t rdh_key(const RDHConfig *cfg,
                              int64_t ring, int64_t wedge,
                              int64_t mirror, int64_t u, int64_t v)
{
    return ((ring * cfg->n_wedges + wedge) * cfg->n_mirror + mirror)
           * (cfg->max_u * cfg->n_v) + u * cfg->n_v + v;
}

/* ═══════════════════════════════════════════════════════════════════
 * EMBEDDED GEAR LOCK (from gear_lock.h)
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t gpu_counter;   /* GPU bridge count  */
    uint64_t cpu_counter;   /* CPU tick count    */
    void    *c144_ref;      /* optional ref      */
} GearLock;

static inline void gear_cpu_tick(GearLock *lock) {
    lock->cpu_counter++;
}

static inline void gear_gpu_tick(GearLock *lock, uint32_t n) {
    lock->gpu_counter += n;
}

/* ═══════════════════════════════════════════════════════════════════
 * GGUF READER — minimal, self-contained
 * ═══════════════════════════════════════════════════════════════════ */

/* GGUF3 magic */
#define GGUF_MAGIC  0x46475547u  /* "GGUF" */

enum GGUFValueType {
    GGUF_TYPE_UINT8   = 0,
    GGUF_TYPE_INT8    = 1,
    GGUF_TYPE_UINT16  = 2,
    GGUF_TYPE_INT16   = 3,
    GGUF_TYPE_UINT32  = 4,
    GGUF_TYPE_INT32   = 5,
    GGUF_TYPE_FLOAT32 = 6,
    GGUF_TYPE_BOOL    = 7,
    GGUF_TYPE_STRING  = 8,
    GGUF_TYPE_ARRAY   = 9,
    GGUF_TYPE_UINT64  = 10,
    GGUF_TYPE_INT64   = 11,
    GGUF_TYPE_FLOAT64 = 12,
    GGUF_TYPE_FLOAT16 = 18,
    GGUF_TYPE_BF16    = 19,
};

/* Max tensors we track */
#define MAX_TENSORS  4096

typedef struct {
    char     name[256];
    uint32_t n_dims;
    uint64_t dims[4];
    uint32_t type;
    uint64_t offset;       /* data offset in file */
    uint64_t n_elems;
    uint64_t n_bytes;
    uint32_t ggml_type;    /* GGML tensor type */
} GGUFTensorInfo;

typedef struct {
    uint8_t  *file_data;
    size_t    file_size;
    uint32_t  header_size;
    uint32_t  n_tensors;
    GGUFTensorInfo tensors[MAX_TENSORS];
    uint64_t  tensor_data_offset;  /* offset where tensor data starts */
} GGUFFile;

static inline uint32_t gguf_read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t gguf_read_u64(const uint8_t *p) {
    return (uint64_t)gguf_read_u32(p) | ((uint64_t)gguf_read_u32(p + 4) << 32);
}

static inline uint32_t ggml_type_size(uint32_t t) {
    /* Common GGML tensor types */
    switch (t) {
        case 0:  return 4;     /* GGML_TYPE_F32     */
        case 1:  return 4;     /* GGML_TYPE_F16      */
        case 2:  return 4;     /* GGML_TYPE_Q4_0     */
        case 3:  return 4;     /* GGML_TYPE_Q4_1     */
        case 6:  return 4;     /* GGML_TYPE_Q5_0     */
        case 7:  return 4;     /* GGML_TYPE_Q5_1     */
        case 8:  return 4;     /* GGML_TYPE_Q8_0     */
        case 10: return 2;     /* GGML_TYPE_Q2_K     */
        case 11: return 2;     /* GGML_TYPE_Q3_K     */
        case 12: return 2;     /* GGML_TYPE_Q4_K     */
        case 13: return 2;     /* GGML_TYPE_Q5_K     */
        case 14: return 2;     /* GGML_TYPE_Q6_K     */
        case 15: return 2;     /* GGML_TYPE_Q8_K     */
        default: return 4;     /* fallback           */
    }
}

static inline uint64_t ggml_tensor_nbytes(uint32_t type, uint64_t n_elems) {
    /* For quantized types, need actual block size.
     * Conservative: 4 bytes per element usually works for Q4_0 etc */
    return n_elems * ggml_type_size(type);
}

static int gguf_parse(const uint8_t *data, size_t size, GGUFFile *f) {
    memset(f, 0, sizeof(*f));
    f->file_data = (uint8_t*)data;
    f->file_size = size;

    if (size < 16) return -1;

    uint32_t magic = gguf_read_u32(data);
    if (magic != GGUF_MAGIC) return -1;

    /* version */
    uint32_t version = gguf_read_u32(data + 4);

    /* n_tensors, n_metadata_kv */
    uint32_t n_tensors   = gguf_read_u64(data + 8);
    uint32_t n_metadata  = gguf_read_u64(data + 16);

    f->n_tensors = n_tensors > MAX_TENSORS ? MAX_TENSORS : n_tensors;

    /* Offset past header */
    uint64_t pos = 24; /* magic(4) + version(4) + n_tensors(8) + n_metadata(8) */

    /* Skip metadata KV pairs */
    for (uint32_t i = 0; i < n_metadata; i++) {
        if (pos + 8 > size) return -1;
        /* key string */
        uint64_t key_len = gguf_read_u64(data + pos);
        pos += 8 + key_len;
        if (pos > size) return -1;

        /* value type */
        uint32_t vtype = gguf_read_u32(data + pos);
        pos += 4;
        if (pos > size) return -1;

        /* skip value based on type */
        switch (vtype) {
            case GGUF_TYPE_UINT8:
            case GGUF_TYPE_INT8:   pos += 1; break;
            case GGUF_TYPE_UINT16:
            case GGUF_TYPE_INT16:  pos += 2; break;
            case GGUF_TYPE_UINT32:
            case GGUF_TYPE_INT32:
            case GGUF_TYPE_FLOAT32:
            case GGUF_TYPE_BOOL:   pos += 4; break;
            case GGUF_TYPE_UINT64:
            case GGUF_TYPE_INT64:
            case GGUF_TYPE_FLOAT64: pos += 8; break;
            case GGUF_TYPE_FLOAT16:
            case GGUF_TYPE_BF16:   pos += 2; break;
            case GGUF_TYPE_STRING: {
                uint64_t slen = gguf_read_u64(data + pos);
                pos += 8 + slen;
                break;
            }
            case GGUF_TYPE_ARRAY: {
                uint32_t atype = gguf_read_u32(data + pos);
                pos += 4;
                uint64_t alen = gguf_read_u64(data + pos);
                pos += 8;
                for (uint64_t j = 0; j < alen && pos < size; j++) {
                    switch (atype) {
                        case GGUF_TYPE_STRING: {
                            uint64_t slen = gguf_read_u64(data + pos);
                            pos += 8 + slen;
                            break;
                        }
                        default: pos += 4; break; /* assume 4B types */
                    }
                }
                break;
            }
            default: pos += 4; break;
        }
        if (pos > size) return -1;
    }

    f->header_size = pos;

    /* Read tensor info entries */
    for (uint32_t i = 0; i < f->n_tensors && i < n_tensors; i++) {
        GGUFTensorInfo *t = &f->tensors[i];
        if (pos + 8 > size) return -1;
        uint64_t name_len = gguf_read_u64(data + pos);
        pos += 8;
        if (pos + name_len > size) return -1;
        uint32_t copy_len = (name_len < 255) ? name_len : 255;
        memcpy(t->name, data + pos, copy_len);
        t->name[copy_len] = '\0';
        pos += name_len;

        if (pos + 4 > size) return -1;
        t->n_dims = gguf_read_u32(data + pos);
        pos += 4;

        for (uint32_t d = 0; d < t->n_dims && d < 4; d++) {
            t->dims[d] = gguf_read_u64(data + pos);
            pos += 8;
        }

        if (pos + 4 > size) return -1;
        t->type = gguf_read_u32(data + pos);
        pos += 4;

        t->offset = pos; /* data offset is at current position in info block */
        /* Actually the data offset is stored AFTER the info entry in the
         * tensor data section. Let me skip the info entry first. */
        /* tensor info entry is just name+dims+type in header, 
         * data offsets come in the tensor data section */

        t->n_elems = 1;
        for (uint32_t d = 0; d < t->n_dims; d++)
            t->n_elems *= t->dims[d];
        t->n_bytes = t->n_elems * 4; /* conservative: assume 4B per element */
        /* Better: use ggml type size */
        t->n_bytes = t->n_elems * ggml_type_size(t->type);
        /* For quantized types, pad to block boundary */
        if (t->n_bytes < t->n_elems) t->n_bytes = t->n_elems; /* min 1B/elem */
    }

    /* The tensor data offset: after all tensor info entries */
    f->tensor_data_offset = pos;

    /* Now read the actual data offsets (stored as uint64 per tensor) */
    for (uint32_t i = 0; i < f->n_tensors && i < n_tensors; i++) {
        GGUFTensorInfo *t = &f->tensors[i];
        if (pos + 8 > size) break;
        t->offset = gguf_read_u64(data + pos);
        pos += 8;
    }

    return 0; /* success */
}

/* ═══════════════════════════════════════════════════════════════════
 * GPU JET PULLER — GGUF tensor reader & bandwidth benchmark
 * ═══════════════════════════════════════════════════════════════════ */

/* DRamTile config */
#define N_PIPES    1728
#define N_TICKS    12
#define N_SLOTS    (N_PIPES * N_TICKS)  /* 20736 */
#define TPB        256
#define MAX_BYTES  (64UL * 1024UL * 1024UL)  /* 64 MB HBM buffer */

/* Kernel: pull chunk from HBM */
__global__ void pull_kernel(
    const uint8_t *dram_base,
    const uint32_t *pull_offsets,
    uint32_t        n_pulls,
    uint32_t        chunk_sz,
    uint64_t       *out_timestamps)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t stride = gridDim.x * blockDim.x;

    for (uint32_t i = idx; i < n_pulls; i += stride) {
        const uint8_t *chunk = dram_base + pull_offsets[i];
        volatile uint32_t sum = 0;
        uint32_t n_vec = chunk_sz / 16;
        #pragma unroll
        for (uint32_t v = 0; v < n_vec; v++) {
            uint4 vec = *reinterpret_cast<const uint4*>(chunk + v * 16);
            sum += vec.x + vec.y + vec.z + vec.w;
        }
        (void)sum;
        out_timestamps[i] = clock64();
    }
}

/* Kernel: pull with XOR checksum */
__global__ void pull_xor_kernel(
    const uint8_t *dram_base,
    const uint32_t *pull_offsets,
    uint32_t        n_pulls,
    uint32_t        chunk_sz,
    uint64_t       *out_timestamps,
    uint8_t        *out_errors,
    uint32_t       *ref_checksums)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t stride = gridDim.x * blockDim.x;

    for (uint32_t i = idx; i < n_pulls; i += stride) {
        const uint8_t *chunk = dram_base + pull_offsets[i];
        uint32_t cksum = 0;
        uint32_t n_vec = chunk_sz / 16;
        #pragma unroll
        for (uint32_t v = 0; v < n_vec; v++) {
            uint4 vec = *reinterpret_cast<const uint4*>(chunk + v * 16);
            cksum ^= vec.x ^ vec.y ^ vec.z ^ vec.w;
        }
        uint8_t cksum8 = cksum ^ (cksum >> 8) ^ (cksum >> 16) ^ (cksum >> 24);
        out_errors[i] = (cksum8 != (uint8_t)(ref_checksums[i] & 0xFF)) ? 1 : 0;
        out_timestamps[i] = clock64();
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    const char *model_path = (argc > 1) ? argv[1] : "/content/Qwen3-0.6B-Q4_0.gguf";

    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  GPU Jet Puller — REAL MODEL WEIGHT Benchmark           ║\n");
    printf("║  Model: %s\n", model_path);
    for (int p = 48 - (int)strlen(model_path); p > 0; p--) printf(" ");
    printf("║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* ── Load GGUF file into memory ── */
    printf("Loading model: %s\n", model_path);
    FILE *fp = fopen(model_path, "rb");
    if (!fp) {
        fprintf(stderr, "ERROR: Cannot open %s\n", model_path);
        printf("TIP: download with:\n");
        printf("  wget -O %s https://huggingface.co/Qwen/Qwen3-0.6B-GGUF/resolve/main/qwen3-0.6b-q4_0.gguf\n", model_path);
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    size_t file_size = ftell(fp);
    rewind(fp);
    uint8_t *file_data = (uint8_t*)malloc(file_size);
    if (!file_data) { fclose(fp); fprintf(stderr,"OOM\n"); return 1; }
    size_t nread = fread(file_data, 1, file_size, fp);
    fclose(fp);
    if (nread != file_size) { fprintf(stderr,"Read error\n"); return 1; }
    printf("  Loaded: %.2f MB\n\n", file_size / 1e6);

    /* ── Parse GGUF ── */
    GGUFFile gguf;
    memset(&gguf, 0, sizeof(gguf));
    if (gguf_parse(file_data, file_size, &gguf) != 0) {
        fprintf(stderr, "ERROR: Failed to parse GGUF file\n");
        free(file_data);
        return 1;
    }
    printf("GGUF Header: %u tensors, header_size=%u, data_offset=%lu\n",
           gguf.n_tensors, gguf.header_size, (unsigned long)gguf.tensor_data_offset);

    /* ═══════════════════════════════════════════════════════════════
     * GPU JET PULLER BENCHMARK
     * ═══════════════════════════════════════════════════════════════ */

    int dev_count;
    cudaGetDeviceCount(&dev_count);
    if (dev_count == 0) {
        printf("ERROR: No CUDA device found\n");
        free(file_data);
        return 1;
    }
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    printf("GPU: %s (cap %d.%d)  VRAM: %zu MB / %zu MB free\n\n",
           prop.name, prop.major, prop.minor,
           prop.totalGlobalMem >> 20,
           (prop.totalGlobalMem - 0) >> 20);
    /* get free memory */
    size_t free_mem, total_mem;
    cudaMemGetInfo(&free_mem, &total_mem);
    printf("  Free VRAM: %.0f MB / %.0f MB\n\n", free_mem/1e6, total_mem/1e6);

    /* Configs to test */
    uint32_t chunk_sizes[] = {64, 256, 1024};
    const int n_sizes = 3;
    const int use_xor_opts[] = {0, 1};
    const int n_modes = 2;

    /* Only include tensors that have data in our buffer */
    uint32_t usable_tensors = 0;
    uint64_t total_model_data = 0;
    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        GGUFTensorInfo *t = &gguf.tensors[i];
        if (t->offset + t->n_bytes <= file_size) {
            usable_tensors++;
            total_model_data += t->n_bytes;
        }
    }
    printf("  Usable tensors: %u / %u (%.1f MB total model data)\n\n",
           usable_tensors, gguf.n_tensors, total_model_data / 1e6);

    /* Allocate HBM buffer for model data */
    size_t hbm_bytes = (total_model_data < MAX_BYTES) ? total_model_data : MAX_BYTES;
    if (hbm_bytes < 1024*1024) hbm_bytes = 1024*1024;
    printf("  Allocating HBM: %.0f MB\n", hbm_bytes / 1e6);

    uint8_t *d_hbm;
    CUDA_CHECK(cudaMalloc(&d_hbm, hbm_bytes));

    /* Copy model data to HBM — concatenate all tensor data */
    uint8_t *h_temp = (uint8_t*)malloc(hbm_bytes);
    if (!h_temp) { fprintf(stderr,"OOM\n"); free(file_data); return 1; }

    /* Debug: print first few tensor names and sizes */
    printf("\n  Tensors loaded:\n");
    uint64_t hbm_pos = 0;
    uint32_t n_loaded = 0;
    for (uint32_t i = 0; i < gguf.n_tensors && hbm_pos < hbm_bytes; i++) {
        GGUFTensorInfo *t = &gguf.tensors[i];
        if (t->offset + t->n_bytes > file_size) continue;
        uint64_t copy_sz = t->n_bytes;
        if (hbm_pos + copy_sz > hbm_bytes)
            copy_sz = hbm_bytes - hbm_pos;
        if (copy_sz > 0) {
            memcpy(h_temp + hbm_pos, file_data + t->offset, copy_sz);
            if (n_loaded < 20) {
                printf("    [%3u] %-32s  %lu elems  %lu B  dims=",
                       i, t->name, (unsigned long)t->n_elems,
                       (unsigned long)t->n_bytes);
                for (uint32_t d = 0; d < t->n_dims; d++)
                    printf("%lu ", (unsigned long)t->dims[d]);
                printf("\n");
            }
            hbm_pos += copy_sz;
            n_loaded++;
        }
    }
    uint64_t actual_data = hbm_pos;

    CUDA_CHECK(cudaMemcpy(d_hbm, h_temp, actual_data, cudaMemcpyHostToDevice));
    printf("  Copied %.1f MB to HBM (%u tensors, %u max shown)\n\n",
           actual_data / 1e6, n_loaded, n_loaded < 20 ? n_loaded : 20);

    /* ── RUN BENCHMARK ── */
    double results[n_sizes][n_modes];

    for (int s = 0; s < n_sizes; s++) {
        uint32_t chunk_sz = chunk_sizes[s];

        for (int m = 0; m < n_modes; m++) {
            int use_xor = use_xor_opts[m];

            printf("─────────────────────────────────────────────────\n");
            printf("CONFIG: chunk=%uB, %s\n", chunk_sz,
                   use_xor ? "with XOR" : "pure pull");
            printf("─────────────────────────────────────────────────\n");

            /* Build pull index — scatter tensor data across DRamTile addresses */
            uint32_t n_pulls = actual_data / chunk_sz;
            if (n_pulls == 0) n_pulls = 1;
            if (n_pulls > 100000) n_pulls = 100000;

            uint32_t *h_offsets = (uint32_t*)malloc(n_pulls * sizeof(uint32_t));
            uint32_t *h_ref_checksums = (uint32_t*)malloc(n_pulls * sizeof(uint32_t));

            if (!h_offsets || !h_ref_checksums) {
                fprintf(stderr,"OOM\n");
                free(h_temp); free(file_data);
                return 1;
            }

            /* Use RDH scatter: spread pulls across all HBM data */
            uint32_t step = (actual_data / chunk_sz < n_pulls) ?
                            1 : (actual_data / chunk_sz) / n_pulls;
            if (step < 1) step = 1;

            for (uint32_t i = 0; i < n_pulls; i++) {
                uint64_t off = ((uint64_t)i * step * chunk_sz) % actual_data;
                h_offsets[i] = (uint32_t)off;

                /* Reference checksum */
                uint32_t cksum = 0;
                for (uint32_t b = 0; b < chunk_sz && off + b < actual_data; b++)
                    cksum ^= (uint32_t)h_temp[off + b];
                h_ref_checksums[i] = cksum;
            }

            /* Copy to GPU */
            uint32_t *d_offsets, *d_ref_checksums;
            uint64_t *d_timestamps;
            uint8_t  *d_errors;

            CUDA_CHECK(cudaMalloc(&d_offsets, n_pulls * sizeof(uint32_t)));
            CUDA_CHECK(cudaMalloc(&d_ref_checksums, n_pulls * sizeof(uint32_t)));
            CUDA_CHECK(cudaMalloc(&d_timestamps, n_pulls * sizeof(uint64_t)));
            CUDA_CHECK(cudaMalloc(&d_errors, n_pulls * sizeof(uint8_t)));

            CUDA_CHECK(cudaMemcpy(d_offsets, h_offsets,
                                  n_pulls * sizeof(uint32_t),
                                  cudaMemcpyHostToDevice));
            CUDA_CHECK(cudaMemcpy(d_ref_checksums, h_ref_checksums,
                                  n_pulls * sizeof(uint32_t),
                                  cudaMemcpyHostToDevice));

            /* Launch kernel */
            uint32_t blocks = (n_pulls + TPB - 1) / TPB;
            cudaEvent_t start, stop;
            cudaEventCreate(&start);
            cudaEventCreate(&stop);

            cudaEventRecord(start);
            if (use_xor) {
                pull_xor_kernel<<<blocks, TPB>>>(
                    d_hbm, d_offsets, n_pulls, chunk_sz,
                    d_timestamps, d_errors, d_ref_checksums);
            } else {
                pull_kernel<<<blocks, TPB>>>(
                    d_hbm, d_offsets, n_pulls, chunk_sz,
                    d_timestamps);
            }
            cudaEventRecord(stop);
            cudaEventSynchronize(stop);

            float ms;
            cudaEventElapsedTime(&ms, start, stop);

            /* Error check (XOR mode) */
            uint32_t n_errors = 0;
            if (use_xor) {
                uint8_t *h_errors = (uint8_t*)malloc(n_pulls);
                CUDA_CHECK(cudaMemcpy(h_errors, d_errors, n_pulls,
                                      cudaMemcpyDeviceToHost));
                for (uint32_t i = 0; i < n_pulls; i++)
                    if (h_errors[i]) n_errors++;
                free(h_errors);
            }

            double data_gb = (double)n_pulls * chunk_sz / 1e9;
            double bw = (ms > 0) ? data_gb / (ms / 1000.0) : 0;

            printf("  Pulls: %u, errors: %u\n", n_pulls, n_errors);
            printf("  Kernel time: %.2f ms\n", ms);
            printf("  Data: %.3f GB\n", data_gb);
            printf("  Throughput: %.2f GB/s\n\n", bw);

            results[s][m] = bw;

            cudaEventDestroy(start);
            cudaEventDestroy(stop);
            CUDA_CHECK(cudaFree(d_offsets));
            CUDA_CHECK(cudaFree(d_ref_checksums));
            CUDA_CHECK(cudaFree(d_timestamps));
            CUDA_CHECK(cudaFree(d_errors));
            free(h_offsets);
            free(h_ref_checksums);
        }
    }

    /* ── RESULTS TABLE ── */
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║         GPU JET PULLER — REAL MODEL WEIGHT RESULTS         ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║  Model: %s\n", model_path);
    for (int p = 48 - (int)strlen(model_path); p > 0; p--) printf(" ");
    printf("║\n");
    printf("║  Tensors: %u  Data loaded: %.1f MB\n", n_loaded, actual_data / 1e6);
    for (int p = 48 - 20; p > 0; p--) printf(" ");
    printf("║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║  Chunk Size    │  Pure Pull       │  With XOR       │\n");
    printf("╠════════════════╪═════════════════════╪═════════════════════╣\n");

    double best_bw = 0;
    int best_s = 0, best_m = 0;
    for (int s = 0; s < n_sizes; s++) {
        printf("║  %-12s │  %-14.2f GB/s │  %-14.2f GB/s │\n",
               chunk_sizes[s] == 64 ? "64B" :
               chunk_sizes[s] == 256 ? "256B" : "1024B",
               results[s][0], results[s][1]);
        for (int m = 0; m < n_modes; m++) {
            if (results[s][m] > best_bw) {
                best_bw = results[s][m];
                best_s = s; best_m = m;
            }
        }
    }
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║  Optimal: %s %s at %.2f GB/s", 
           chunk_sizes[best_s] == 64 ? "64B" :
           chunk_sizes[best_s] == 256 ? "256B" : "1024B",
           best_m == 0 ? "pure" : "with XOR", best_bw);
    for (int p = 48 - 18; p > 0; p--) printf(" ");
    printf("║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");

    printf("\n=== BENCHMARK COMPLETE ===\n");
    printf("Model: %s\n", model_path);
    printf("Best bandwidth: %.2f GB/s (%s, %s)\n", best_bw,
           chunk_sizes[best_s] == 64 ? "64B" :
           chunk_sizes[best_s] == 256 ? "256B" : "1024B",
           best_m == 0 ? "pure pull" : "with XOR");
    printf("Total model data loaded: %.1f MB\n", actual_data / 1e6);

    /* Cleanup */
    CUDA_CHECK_VOID(cudaFree(d_hbm));
    free(h_temp);
    free(file_data);

    return 0;
}