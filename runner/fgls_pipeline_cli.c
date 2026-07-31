/* fgls_pipeline_cli.c — CLI for FGLS Geometric Weight Storage Pipeline
 * ═══════════════════════════════════════════════════════════════════════
 *
 * Commands:
 *   fgls_pipeline encode  <input.bin> <output.gfuf> [--strategy=N] [--geojump=TYPE]
 *   fgls_pipeline decode  <input.gfuf> <output.bin>
 *   fgls_pipeline bench   [--tensors=N] [--size=BYTES]
 *   fgls_pipeline pull    [--iterations=N]
 *   fgls_pipeline verify  <input.bin> <decoded.bin>
 *   fgls_pipeline stats
 *
 * Build:
 *   gcc -O2 -std=c11 -I. -Icollection -Icollection/src \
 *       -Icollection/core/pogls_engine/twin_core \
 *       -Icollection/core/pogls_engine \
 *       -Icollection/core/pogls_engine/core \
 *       -Icollection/core/core \
 *       -Icollection/rdh \
 *       -Irunner \
 *       -DFGLS_PIPELINE_IMPLEMENTATION \
 *       runner/fgls_pipeline.c runner/fgls_pipeline_cli.c runner/dramtile_store.c \
 *       collection/dgls/geo/src/geo_jump.c \
 *       -lm -o fgls_pipeline.exe
 *
 * GPU Build (nvcc):
 *   nvcc -O2 -std=c++17 -arch=sm_61 \
 *       -I. -Icollection -Icollection/src \
 *       -Icollection/core/pogls_engine/twin_core \
 *       -Icollection/core/pogls_engine \
 *       -Icollection/core/pogls_engine/core \
 *       -Icollection/core/core \
 *       -Icollection/rdh \
 *       -Irunner \
 *       -DFGLS_PIPELINE_IMPLEMENTATION \
 *       runner/fgls_pipeline.cu runner/fgls_pipeline_cli.c runner/dramtile_store.c \
 *       collection/dgls/geo/src/geo_jump.c \
 *       -lcudart -o fgls_pipeline.exe
 *
 * ═══════════════════════════════════════════════════════════════════════ */

#include "fgls_pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ──────────────────────────────────────────────────────────────────────
 * CLI State
 * ────────────────────────────────────────────────────────────────────── */

static FglsPipeline g_pipe;
static int g_verbose = 0;

static const char *g_path = NULL;
static size_t g_max_bytes = 64 << 20;  /* 64 MB default */
static size_t g_cold_cap = 0;
static const char *g_cold_path = NULL;
static uint64_t g_seed_gen2 = 0x9E3779B97F4A7C15ULL;
static uint64_t g_seed_gen3 = 0xBF58476D1CE4E5B9ULL;
static uint64_t g_bundle[6] = {0};
static uint32_t g_flags = FGLS_PIPE_DEFAULT;
static int g_contour_strategy = CODEC_STRIDE37;
static int g_geojump_type = JUMP_HILBERT;
static uint32_t g_geojump_param = 1;

/* ──────────────────────────────────────────────────────────────────────
 * Helpers
 * ────────────────────────────────────────────────────────────────────── */

static void print_usage(const char *prog) {
    fprintf(stderr, "\n");
    fprintf(stderr, "FGLS Pipeline — Unified Geometric Weight Storage\n");
    fprintf(stderr, "═══════════════════════════════════════════════\n\n");
    fprintf(stderr, "Usage: %s <command> [options]\n\n", prog);
    fprintf(stderr, "Commands:\n");
    fprintf(stderr, "  encode  <input.bin> <output.gfuf>  Encode weight tensor\n");
    fprintf(stderr, "  decode  <input.gfuf> <output.bin>  Decode weight tensor\n");
    fprintf(stderr, "  bench   [--tensors=N] [--size=BYTES]  Benchmark throughput\n");
    fprintf(stderr, "  pull    [--iterations=N]              Run GPU pull / CPU verify\n");
    fprintf(stderr, "  verify  <original.bin> <decoded.bin>  Verify roundtrip\n");
    fprintf(stderr, "  stats                                 Show pipeline statistics\n");
    fprintf(stderr, "  help                                Show this help\n\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --path=FILE         DRamTile backing file (default: anonymous)\n");
    fprintf(stderr, "  --max-bytes=N       DRamTile capacity in bytes (default: 64MB)\n");
    fprintf(stderr, "  --cold-cap=N        Cold storage capacity (default: 0=disabled)\n");
    fprintf(stderr, "  --cold-path=FILE    Cold storage backing file\n");
    fprintf(stderr, "  --seed=HEX          Geometric seed (default: 0x9E3779B97F4A7C15)\n");
    fprintf(stderr, "  --strategy=N        Contour strategy: 0=seq, 1=stride37, 2=face, 3=grid\n");
    fprintf(stderr, "  --geojump=TYPE      GeoJump type: 0=hilbert, 1=peano, 2=pentagon, 3=mod, 4=invert\n");
    fprintf(stderr, "  --geojump-param=N   GeoJump parameter\n");
    fprintf(stderr, "  --gpu               Enable GPU jet puller (requires CUDA build)\n");
    fprintf(stderr, "  --hbm               Use HBM (cudaMalloc) instead of DRamTile mmap\n");
    fprintf(stderr, "  --no-geojump        Disable GeoJump routing layer\n");
    fprintf(stderr, "  --no-frameseek      Disable FrameSeek timeline\n");
    fprintf(stderr, "  --no-gearshift      Disable GearShift routing\n");
    fprintf(stderr, "  -v, --verbose       Verbose output\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s encode weights.bin model.gfuf --strategy=1 --geojump=0\n", prog);
    fprintf(stderr, "  %s decode model.gfuf weights_dec.bin\n", prog);
    fprintf(stderr, "  %s bench --tensors=100 --size=1048576\n", prog);
    fprintf(stderr, "  %s pull --iterations=3456 --gpu\n", prog);
    fprintf(stderr, "  %s stats\n", prog);
    fprintf(stderr, "\n");
}

static uint64_t parse_hex(const char *s) {
    uint64_t val = 0;
    while (*s) {
        char c = *s++;
        val <<= 4;
        if (c >= '0' && c <= '9') val |= c - '0';
        else if (c >= 'a' && c <= 'f') val |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') val |= c - 'A' + 10;
    }
    return val;
}

static size_t parse_size(const char *s) {
    char *end;
    double val = strtod(s, &end);
    if (end != s) {
        if (*end == 'K' || *end == 'k') val *= 1024;
        else if (*end == 'M' || *end == 'm') val *= 1024 * 1024;
        else if (*end == 'G' || *end == 'g') val *= 1024 * 1024 * 1024;
    }
    return (size_t)val;
}

static int load_file(const char *path, uint8_t **out_data, size_t *out_size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "Error: Cannot open '%s'\n", path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) {
        fclose(fp);
        return -1;
    }
    *out_data = (uint8_t*)malloc(sz);
    if (!*out_data) {
        fclose(fp);
        return -1;
    }
    size_t read = fread(*out_data, 1, sz, fp);
    fclose(fp);
    if (read != (size_t)sz) {
        free(*out_data);
        return -1;
    }
    *out_size = (size_t)sz;
    return 0;
}

static int save_file(const char *path, const uint8_t *data, size_t size) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "Error: Cannot create '%s'\n", path);
        return -1;
    }
    size_t written = fwrite(data, 1, size, fp);
    fclose(fp);
    return written == size ? 0 : -1;
}

/* ──────────────────────────────────────────────────────────────────────
 * Pipeline Init from CLI options
 * ────────────────────────────────────────────────────────────────────── */

static int init_pipeline(void) {
    int r = fgls_pipeline_init(&g_pipe, g_path, g_max_bytes, g_cold_cap, g_cold_path,
                                g_seed_gen2, g_seed_gen3, g_bundle, g_flags);
    if (r != 0) {
        fprintf(stderr, "Pipeline init failed: %d\n", r);
        return r;
    }

    /* Override contour strategy if specified */
    if (g_contour_strategy >= 0 && g_contour_strategy < CODEC_COUNT) {
        codec_ctx *codec = (codec_ctx*)g_pipe.codec_ctx;
        codec->strategy = (CODEC_STRATEGY)g_contour_strategy;
    }

    /* Override GeoJump type if specified */
    if (g_geojump_type >= 0 && g_geojump_type <= JUMP_CAPO && g_pipe.geojump_router) {
        GeoJumpRouter *router = (GeoJumpRouter*)g_pipe.geojump_router;
        router->type = (GeoJumpType)g_geojump_type;
        router->param = g_geojump_param;
    }

    if (g_verbose) {
        fprintf(stderr, "[CLI] Pipeline initialized\n");
        fprintf(stderr, "  Path: %s\n", g_path ? g_path : "(anonymous)");
        fprintf(stderr, "  Max bytes: %zu\n", g_max_bytes);
        fprintf(stderr, "  Cold cap: %zu\n", g_cold_cap);
        fprintf(stderr, "  Flags: 0x%02X\n", g_flags);
        fprintf(stderr, "  Contour strategy: %s\n", codec_strategy_name((CODEC_STRATEGY)g_contour_strategy));
        if (g_pipe.geojump_router) {
            GeoJumpRouter *router = (GeoJumpRouter*)g_pipe.geojump_router;
            static const char *jump_names[] = {"hilbert","peano","pentagon","mod","invert","ground","capo"};
            fprintf(stderr, "  GeoJump: %s (param=%u)\n",
                    jump_names[router->type], router->param);
        }
    }
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────
 * Commands
 * ────────────────────────────────────────────────────────────────────── */

static int cmd_encode(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: encode <input.bin> <output.gfuf>\n");
        return 1;
    }
    const char *input = argv[1];
    const char *output = argv[2];

    uint8_t *data = NULL;
    size_t size = 0;
    if (load_file(input, &data, &size) != 0) return 1;

    if (g_verbose) fprintf(stderr, "[CLI] Loaded %zu bytes from %s\n", size, input);

    if (init_pipeline() != 0) {
        free(data);
        return 1;
    }

    /* Create tensor descriptor */
    FglsTensor tensor = {0};
    tensor.name = "input_tensor";
    tensor.data = data;
    tensor.size = size;
    tensor.dtype = DT_F32;
    tensor.ndim = 1;
    tensor.shape[0] = (uint32_t)size;

    /* Encode */
    int r = fgls_pipeline_encode(&g_pipe, &tensor);
    if (r != 0) {
        fprintf(stderr, "Encode failed: %d\n", r);
        free(data);
        return 1;
    }

    if (g_verbose) fprintf(stderr, "[CLI] Encoded to DRamTile\n");

    /* For now, save the DRamTile store state as output
     * In a real implementation, this would serialize the pipeline state */
    DtGearStore *dg = (DtGearStore*)g_pipe.dramtile_store;
    dtg_flush(dg);

    if (g_verbose) fprintf(stderr, "[CLI] Flushed to %s\n", output);

    free(data);
    fgls_pipeline_destroy(&g_pipe);
    return 0;
}

static int cmd_decode(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: decode <input.gfuf> <output.bin>\n");
        return 1;
    }
    const char *input = argv[1];
    const char *output = argv[2];

    /* For now, just initialize pipeline and decode from DRamTile */
    if (init_pipeline() != 0) return 1;

    /* Create tensor descriptor - in real impl, this would read from the .gfuf file */
    FglsTensor tensor = {0};
    tensor.name = "input_tensor";
    tensor.size = 1024;  /* placeholder */
    tensor.dtype = DT_F32;
    tensor.ndim = 1;
    tensor.shape[0] = 1024;

    uint8_t *decoded = (uint8_t*)malloc(tensor.size);
    if (!decoded) {
        fgls_pipeline_destroy(&g_pipe);
        return 1;
    }

    int r = fgls_pipeline_decode(&g_pipe, &tensor, decoded);
    if (r != 0) {
        fprintf(stderr, "Decode failed: %d\n", r);
        free(decoded);
        fgls_pipeline_destroy(&g_pipe);
        return 1;
    }

    if (save_file(output, decoded, tensor.size) != 0) {
        free(decoded);
        fgls_pipeline_destroy(&g_pipe);
        return 1;
    }

    if (g_verbose) fprintf(stderr, "[CLI] Decoded %zu bytes to %s\n", tensor.size, output);

    free(decoded);
    fgls_pipeline_destroy(&g_pipe);
    return 0;
}

static int cmd_bench(int argc, char **argv) {
    uint32_t n_tensors = 100;
    size_t tensor_size = 1024 * 1024;  /* 1 MB */

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--tensors=", 10) == 0) {
            n_tensors = (uint32_t)atoi(argv[i] + 10);
        } else if (strncmp(argv[i], "--size=", 7) == 0) {
            tensor_size = parse_size(argv[i] + 7);
        }
    }

    if (g_verbose) {
        fprintf(stderr, "[CLI] Benchmark: %u tensors × %zu bytes\n", n_tensors, tensor_size);
    }

    if (init_pipeline() != 0) return 1;

    int r = fgls_pipeline_benchmark(&g_pipe, n_tensors, tensor_size);
    if (r < 0) {
        fprintf(stderr, "Benchmark failed with %d errors\n", -r);
    }

    fgls_pipeline_stats(&g_pipe, stdout);
    fgls_pipeline_destroy(&g_pipe);
    return r < 0 ? 1 : 0;
}

static int cmd_pull(int argc, char **argv) {
    uint32_t iterations = 3456;  /* 2 full GEO_FULL sweeps */

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--iterations=", 13) == 0) {
            iterations = (uint32_t)atoi(argv[i] + 13);
        }
    }

    if (g_verbose) {
        fprintf(stderr, "[CLI] Pull: %u iterations\n", iterations);
    }

    if (init_pipeline() != 0) return 1;

    int r = fgls_pipeline_pull(&g_pipe, iterations);
    if (r != 0) {
        fprintf(stderr, "Pull failed: %d\n", r);
    }

    fgls_pipeline_stats(&g_pipe, stdout);
    fgls_pipeline_destroy(&g_pipe);
    return r != 0 ? 1 : 0;
}

static int cmd_verify(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: verify <original.bin> <decoded.bin>\n");
        return 1;
    }
    const char *original_path = argv[1];
    const char *decoded_path = argv[2];

    uint8_t *orig_data = NULL, *dec_data = NULL;
    size_t orig_size = 0, dec_size = 0;

    if (load_file(original_path, &orig_data, &orig_size) != 0) return 1;
    if (load_file(decoded_path, &dec_data, &dec_size) != 0) {
        free(orig_data);
        return 1;
    }

    if (orig_size != dec_size) {
        fprintf(stderr, "Size mismatch: %zu vs %zu\n", orig_size, dec_size);
        free(orig_data);
        free(dec_data);
        return 1;
    }

    int errors = 0;
    for (size_t i = 0; i < orig_size; i++) {
        if (orig_data[i] != dec_data[i]) errors++;
    }

    if (errors == 0) {
        printf("VERIFY: PASS (%zu bytes match)\n", orig_size);
    } else {
        printf("VERIFY: FAIL (%d byte errors out of %zu)\n", errors, orig_size);
    }

    free(orig_data);
    free(dec_data);
    return errors == 0 ? 0 : 1;
}

static int cmd_stats(int argc, char **argv) {
    (void)argc; (void)argv;

    if (init_pipeline() != 0) return 1;

    fgls_pipeline_stats(&g_pipe, stdout);
    fgls_pipeline_destroy(&g_pipe);
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────
 * Main
 * ────────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    /* Parse global options first */
    int cmd_start = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "encode") == 0 ||
            strcmp(argv[i], "decode") == 0 ||
            strcmp(argv[i], "bench") == 0 ||
            strcmp(argv[i], "pull") == 0 ||
            strcmp(argv[i], "verify") == 0 ||
            strcmp(argv[i], "stats") == 0 ||
            strcmp(argv[i], "help") == 0) {
            cmd_start = i;
            break;
        }
        if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
            g_path = argv[++i];
        } else if (strncmp(argv[i], "--path=", 7) == 0) {
            g_path = argv[i] + 7;
        } else if (strcmp(argv[i], "--max-bytes") == 0 && i + 1 < argc) {
            g_max_bytes = parse_size(argv[++i]);
        } else if (strncmp(argv[i], "--max-bytes=", 12) == 0) {
            g_max_bytes = parse_size(argv[i] + 12);
        } else if (strcmp(argv[i], "--cold-cap") == 0 && i + 1 < argc) {
            g_cold_cap = parse_size(argv[++i]);
        } else if (strncmp(argv[i], "--cold-cap=", 11) == 0) {
            g_cold_cap = parse_size(argv[i] + 11);
        } else if (strcmp(argv[i], "--cold-path") == 0 && i + 1 < argc) {
            g_cold_path = argv[++i];
        } else if (strncmp(argv[i], "--cold-path=", 12) == 0) {
            g_cold_path = argv[i] + 12;
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            g_seed = parse_hex(argv[++i]);
        } else if (strncmp(argv[i], "--seed=", 7) == 0) {
            g_seed = parse_hex(argv[i] + 7);
        } else if (strcmp(argv[i], "--strategy") == 0 && i + 1 < argc) {
            g_contour_strategy = atoi(argv[++i]);
        } else if (strncmp(argv[i], "--strategy=", 11) == 0) {
            g_contour_strategy = atoi(argv[i] + 11);
        } else if (strcmp(argv[i], "--geojump") == 0 && i + 1 < argc) {
            g_geojump_type = atoi(argv[++i]);
        } else if (strncmp(argv[i], "--geojump=", 10) == 0) {
            g_geojump_type = atoi(argv[i] + 10);
        } else if (strcmp(argv[i], "--geojump-param") == 0 && i + 1 < argc) {
            g_geojump_param = (uint32_t)atoi(argv[++i]);
        } else if (strncmp(argv[i], "--geojump-param=", 16) == 0) {
            g_geojump_param = (uint32_t)atoi(argv[i] + 16);
        } else if (strcmp(argv[i], "--gpu") == 0) {
            g_flags |= FGLS_PIPE_GPU_PULL;
        } else if (strcmp(argv[i], "--hbm") == 0) {
            g_flags |= FGLS_PIPE_GPU_HBM;
        } else if (strcmp(argv[i], "--no-geojump") == 0) {
            g_flags &= ~FGLS_PIPE_GEOJUMP;
        } else if (strcmp(argv[i], "--no-frameseek") == 0) {
            g_flags &= ~FGLS_PIPE_FRAMESEEK;
        } else if (strcmp(argv[i], "--no-gearshift") == 0) {
            g_flags &= ~FGLS_PIPE_GEARSHIFT;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
            g_flags |= FGLS_PIPE_VERBOSE;
        }
    }

    const char *cmd = argv[cmd_start];
    int cmd_argc = argc - cmd_start - 1;
    char **cmd_argv = argv + cmd_start + 1;

    if (strcmp(cmd, "help") == 0) {
        print_usage(argv[0]);
        return 0;
    } else if (strcmp(cmd, "encode") == 0) {
        return cmd_encode(cmd_argc, cmd_argv);
    } else if (strcmp(cmd, "decode") == 0) {
        return cmd_decode(cmd_argc, cmd_argv);
    } else if (strcmp(cmd, "bench") == 0) {
        return cmd_bench(cmd_argc, cmd_argv);
    } else if (strcmp(cmd, "pull") == 0) {
        return cmd_pull(cmd_argc, cmd_argv);
    } else if (strcmp(cmd, "verify") == 0) {
        return cmd_verify(cmd_argc, cmd_argv);
    } else if (strcmp(cmd, "stats") == 0) {
        return cmd_stats(cmd_argc, cmd_argv);
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        print_usage(argv[0]);
        return 1;
    }
}