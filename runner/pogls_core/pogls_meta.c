#include "pogls_meta.h"

void pogls_meta_header_init(PoglsStoreHeader *hdr) {
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic   = POGLS_META_MAGIC;
    hdr->version = POGLS_META_VERSION;
}

uint64_t pogls_meta_data_off(const PoglsStoreHeader *hdr) {
    uint64_t off = sizeof(*hdr) + POGLS_INDEX_SZ;
    if (hdr->tensor_meta_off > 0) {
        off = hdr->tensor_meta_off +
              (uint64_t)hdr->tensor_meta_count * POGLS_META_ENTRY_SZ;
    }
    if (hdr->model_meta_off > 0) {
        uint64_t mend = hdr->model_meta_off + hdr->model_meta_sz;
        if (mend > off) off = mend;
    }
    if (hdr->gguf_path_off > 0) {
        uint64_t pend = hdr->gguf_path_off + hdr->gguf_path_sz;
        if (pend > off) off = pend;
    }
    return off;
}

void pogls_meta_entry_init(PoglsTensorMeta *e) {
    memset(e, 0, sizeof(*e));
}

int pogls_meta_read(const char *path,
                    PoglsStoreHeader *hdr_out,
                    uint8_t *idx_out,
                    void *meta_out,
                    uint64_t *data_off_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    uint32_t magic, version;
    if (fread(&magic, 4, 1, f) != 1) { fclose(f); return -1; }
    if (fread(&version, 4, 1, f) != 1) { fclose(f); return -1; }
    if (magic != POGLS_META_MAGIC) { fclose(f); return -1; }

    if (version == 1) {
        if (fseek(f, 64 - 8, SEEK_CUR) != 0) { fclose(f); return -1; }
        if (hdr_out) {
            memset(hdr_out, 0, sizeof(*hdr_out));
            hdr_out->magic   = magic;
            hdr_out->version = version;
            fseek(f, 8, SEEK_SET);
            uint32_t nt, fl;
            fread(&nt, 4, 1, f);
            fread(&fl, 4, 1, f);
            hdr_out->n_tensors = nt;
            hdr_out->flags     = fl;
        }
        if (idx_out) {
            if (fread(idx_out, POGLS_INDEX_SZ, 1, f) != 1) { fclose(f); return -1; }
        } else {
            if (fseek(f, POGLS_INDEX_SZ, SEEK_CUR) != 0) { fclose(f); return -1; }
        }
        if (data_off_out) *data_off_out = (uint64_t)64 + POGLS_INDEX_SZ;
        fclose(f);
        return 0;
    }

    PoglsStoreHeader hdr;
    fseek(f, 0, SEEK_SET);
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return -1; }
    if (hdr_out) *hdr_out = hdr;

    if (idx_out) {
        if (fread(idx_out, POGLS_INDEX_SZ, 1, f) != 1) { fclose(f); return -1; }
    } else {
        if (fseek(f, POGLS_INDEX_SZ, SEEK_CUR) != 0) { fclose(f); return -1; }
    }

    if (meta_out && hdr.tensor_meta_count > 0) {
        uint64_t sz = (uint64_t)hdr.tensor_meta_count * POGLS_META_ENTRY_SZ;
        if (fread(meta_out, sz, 1, f) != 1) { fclose(f); return -1; }
    }

    if (data_off_out) *data_off_out = pogls_meta_data_off(&hdr);
    fclose(f);
    return 0;
}

int pogls_meta_write(const char *path,
                     const PoglsStoreHeader *hdr,
                     const uint8_t *idx,
                     const PoglsTensorMeta *meta,
                     const uint8_t *model_meta,
                     const uint8_t *data,
                     uint64_t data_sz)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    if (fwrite(hdr, sizeof(*hdr), 1, f) != 1) { fclose(f); return -1; }

    uint64_t idx_sz = POGLS_INDEX_SZ;
    if (idx) {
        if (fwrite(idx, idx_sz, 1, f) != 1) { fclose(f); return -1; }
    } else {
        uint8_t zero[4096] = {0};
        for (uint64_t w = 0; w < idx_sz; w += sizeof(zero)) {
            uint64_t chunk = idx_sz - w;
            if (chunk > sizeof(zero)) chunk = sizeof(zero);
            if (fwrite(zero, chunk, 1, f) != 1) { fclose(f); return -1; }
        }
    }

    if (meta && hdr->tensor_meta_count > 0) {
        uint64_t meta_sz = (uint64_t)hdr->tensor_meta_count * POGLS_META_ENTRY_SZ;
        if (fwrite(meta, meta_sz, 1, f) != 1) { fclose(f); return -1; }
    }

    if (model_meta && hdr->model_meta_sz > 0) {
        if (fwrite(model_meta, hdr->model_meta_sz, 1, f) != 1) { fclose(f); return -1; }
    }

    if (data && data_sz > 0) {
        if (fwrite(data, data_sz, 1, f) != 1) { fclose(f); return -1; }
    }

    fclose(f);
    return 0;
}

const PoglsTensorMeta* pogls_meta_find(const PoglsTensorMeta *meta,
                                        uint32_t count, uint32_t addr)
{
    for (uint32_t i = 0; i < count; i++)
        if (meta[i].addr == addr)
            return &meta[i];
    return NULL;
}

const PoglsTensorMeta* pogls_meta_find_name(const PoglsTensorMeta *meta,
                                             uint32_t count, const char *name)
{
    for (uint32_t i = 0; i < count; i++)
        if (strcmp(meta[i].name, name) == 0)
            return &meta[i];
    return NULL;
}
