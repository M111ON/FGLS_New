/*
 * llama_pogls_backend.c — S83-B4 backend
 *
 * Minimal file-stream backend for testing chunked model streaming.
 * It parses the GGUF tensor table, then streams each layer window
 * from the model file into the caller-provided RAM buffer.
 *
 * This backend intentionally stays independent from the llama.cpp
 * tensor internals so it can run against the public Windows build.
 */

#include "llama_pogls_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#ifdef POGLS_WINDOWS
static int64_t file_tell64(FILE *f) { return _ftelli64(f); }
static int file_seek64(FILE *f, int64_t off, int origin) { return _fseeki64(f, off, origin); }
#else
static int64_t file_tell64(FILE *f) { return ftello(f); }
static int file_seek64(FILE *f, int64_t off, int origin) { return fseeko(f, (off_t)off, origin); }
#endif

static uint64_t backend_file_size(FILE *f)
{
    int64_t cur = file_tell64(f);
    if (cur < 0) return 0;
    if (file_seek64(f, 0, SEEK_END) != 0) return 0;
    int64_t end = file_tell64(f);
    if (end < 0) return 0;
    (void)file_seek64(f, cur, SEEK_SET);
    return (uint64_t)end;
}

static int backend_read_exact(FILE *f, uint8_t *buf, uint64_t size)
{
    while (size > 0) {
        size_t chunk = size > (uint64_t)UINT32_MAX ? (size_t)UINT32_MAX : (size_t)size;
        if (fread(buf, 1, chunk, f) != chunk) return -1;
        buf += chunk;
        size -= chunk;
    }
    return 0;
}

static const char *backend_basename(const char *path)
{
    const char *base = path;
    for (const char *p = path; p && *p; ++p) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    return base;
}

static void backend_model_name_from_path(const char *path, char *out, size_t out_sz)
{
    const char *base = backend_basename(path);
    size_t len = strlen(base);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, base, len);
    out[len] = '\0';
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
}

static int backend_count_tensor_window(const ModelIndex *mi,
                                      uint32_t layer_start,
                                      uint32_t layer_end,
                                      uint64_t *out_required_bytes,
                                      uint32_t *out_total_layers,
                                      uint32_t *out_max_layer_bytes)
{
    if (!mi || !out_required_bytes || !out_total_layers || !out_max_layer_bytes)
        return POGLS_BACK_ERR_NULL;

    uint32_t total = pogls_model_index_total_layers(mi);
    if (total == 0) return POGLS_BACK_ERR_PARSE;

    if (layer_end == 0 || layer_end >= total) layer_end = total - 1;
    if (layer_start > layer_end) return POGLS_BACK_ERR_LAYER;

    uint64_t max_layer = 0;
    uint64_t total_bytes = 0;
    for (uint32_t i = layer_start; i <= layer_end; ++i) {
        ModelLayerRecord rec;
        int rc = pogls_model_index_get(mi, i, &rec);
        if (rc != MIDX_OK) return POGLS_BACK_ERR_PARSE;
        uint64_t layer_bytes = rec.byte_end - rec.byte_start;
        if (layer_bytes > max_layer) max_layer = layer_bytes;
        total_bytes += layer_bytes;
    }

    *out_required_bytes = max_layer;
    *out_total_layers = (layer_end - layer_start) + 1u;
    *out_max_layer_bytes = (uint32_t)max_layer;
    (void)total_bytes;
    return POGLS_BACK_OK;
}

int pogls_backend_preflight(const char *gguf_path, const char *model_name,
                            uint64_t window_size,
                            uint64_t *out_required_bytes,
                            uint32_t *out_total_layers)
{
    if (!gguf_path || !out_required_bytes || !out_total_layers)
        return POGLS_BACK_ERR_NULL;

    FILE *f = fopen(gguf_path, "rb");
    if (!f) return POGLS_BACK_ERR_IO;

    uint64_t file_sz = backend_file_size(f);
    fclose(f);

    ModelIndex mi;
    memset(&mi, 0, sizeof(mi));

    char derived_name[64];
    const char *name = model_name;
    if (!name || !*name) {
        backend_model_name_from_path(gguf_path, derived_name, sizeof(derived_name));
        name = derived_name;
    }

    int parse_rc = gguf_parse_to_model_index(gguf_path, &mi, name);
    if (parse_rc != 0) return POGLS_BACK_ERR_PARSE;

    uint32_t total_layers = pogls_model_index_total_layers(&mi);
    uint64_t max_layer_bytes = 0;
    for (uint32_t i = 0; i < total_layers; ++i) {
        ModelLayerRecord rec;
        if (pogls_model_index_get(&mi, i, &rec) != MIDX_OK)
            return POGLS_BACK_ERR_PARSE;
        uint64_t layer_bytes = rec.byte_end - rec.byte_start;
        if (layer_bytes > max_layer_bytes)
            max_layer_bytes = layer_bytes;
    }

    if (window_size != 0 && max_layer_bytes > window_size)
        return POGLS_BACK_ERR_WINSZ;

    *out_required_bytes = max_layer_bytes;
    *out_total_layers = total_layers;

    (void)file_sz;
    return POGLS_BACK_OK;
}

int pogls_backend_run(const PoglsBackendCfg *cfg, PoglsBackendStats *stats)
{
    if (!cfg || !cfg->gguf_path || !cfg->ram_window || !stats)
        return POGLS_BACK_ERR_NULL;

    memset(stats, 0, sizeof(*stats));

    FILE *f = fopen(cfg->gguf_path, "rb");
    if (!f) return POGLS_BACK_ERR_IO;

    char derived_name[64];
    const char *name = cfg->model_name;
    if (!name || !*name) {
        backend_model_name_from_path(cfg->gguf_path, derived_name, sizeof(derived_name));
        name = derived_name;
    }

    ModelIndex mi;
    memset(&mi, 0, sizeof(mi));
    int parse_rc = gguf_parse_to_model_index(cfg->gguf_path, &mi, name);
    if (parse_rc != 0) {
        fclose(f);
        return POGLS_BACK_ERR_PARSE;
    }

    uint32_t total = pogls_model_index_total_layers(&mi);
    uint32_t start = cfg->layer_start;
    uint32_t end = cfg->layer_end == 0 || cfg->layer_end >= total ? total - 1 : cfg->layer_end;
    if (start > end) {
        fclose(f);
        return POGLS_BACK_ERR_LAYER;
    }

    uint64_t peak = 0;
    uint64_t window_cap = cfg->ram_window_size;
    uint8_t *window = (uint8_t *)cfg->ram_window;

    for (uint32_t layer_id = start; layer_id <= end; ++layer_id) {
        ModelLayerRecord rec;
        if (pogls_model_index_get(&mi, layer_id, &rec) != MIDX_OK) {
            fclose(f);
            return POGLS_BACK_ERR_PARSE;
        }

        uint64_t layer_bytes64 = rec.byte_end - rec.byte_start;
        size_t layer_bytes = (size_t)layer_bytes64;

        if (cfg->layer_cb) {
            int cb_rc = cfg->layer_cb(layer_id, total, &rec, cfg->cb_user_data);
            if (cb_rc != 0) {
                fclose(f);
                return POGLS_BACK_ERR_CB;
            }
        }

        if (window_cap != 0 && layer_bytes64 > window_cap) {
            fclose(f);
            return POGLS_BACK_ERR_WINSZ;
        }

        if (file_seek64(f, (int64_t)rec.byte_start, SEEK_SET) != 0) {
            fclose(f);
            return POGLS_BACK_ERR_IO;
        }

        if (layer_bytes > 0) {
            if (backend_read_exact(f, window, (uint64_t)layer_bytes) != 0) {
                fclose(f);
                return POGLS_BACK_ERR_IO;
            }
        }

        stats->layers_read++;
        stats->bytes_read += layer_bytes64;
        if (layer_bytes64 > peak) peak = layer_bytes64;
        if (layer_bytes64 > stats->peak_layer_bytes) stats->peak_layer_bytes = layer_bytes64;
    }

    stats->peak_layer_bytes = peak;
    fclose(f);
    return POGLS_BACK_OK;
}

void pogls_backend_print_stats(const PoglsBackendStats *st)
{
    if (!st) return;
    fprintf(stderr,
            "[backend] layers_read=%u skipped=%u bytes_read=%" PRIu64 " peak_layer=%" PRIu64 "\n",
            st->layers_read,
            st->layers_skipped,
            st->bytes_read,
            st->peak_layer_bytes);
}
