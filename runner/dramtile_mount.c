/*
 * dramtile_mount.c — Mount DRamTile twin file as a filesystem via WinFsp FUSE
 *
 * Usage: dramtile_mount.exe <file.dramtile> <mountpoint>
 *
 * Shows tensor data as nested folders:
 *   /blk.0.attn_q.weight  →  blk / 0 / attn_q / weight
 *
 * Read-only. Each tensor = one file. Size = tensor byte count.
 * Tensor names use '.' separators — FUSE maps them to '/' directories.
 */

#define FUSE_USE_VERSION 26
#define _FILE_OFFSET_BITS 64

#include <fuse/fuse.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* ── Minimal DRamTile types (no RDH dependency) ─────────────── */

#define DT_HASH_SLOTS   512
#define DT_NAME_MAX     256
#define DT_HASH_NAME    48
#define DT_MAX_PATH     260
#define DT_KV_FLAG      0x80000000u

typedef struct {
    uint32_t dram_addr;
    size_t   offset;
    size_t   size;
    char     name[DT_HASH_NAME];
    uint32_t cold_offset;
    uint32_t session_tick;
} DRamTileHashEntry;

typedef struct {
    uint8_t          *base;
    size_t            capacity;
    size_t            used;
    int               is_mmap;
    DRamTileHashEntry hash[DT_HASH_SLOTS];
    uint32_t          n_stored;
    char              filepath[DT_MAX_PATH];
    int               is_twin;
    size_t            weight_boundary;
    size_t            kv_capacity;
    size_t            kv_used;
    uint8_t          *kv_base;
    uint8_t          *cold_base;
    size_t            cold_capacity;
    size_t            cold_used;
    char              cold_filepath[DT_MAX_PATH];
    int               is_cold_twin;
    uint32_t          session_tick;
    void            (*evict_cb)(const char *name, void *user);
    void             *evict_user;
    size_t            free_offs[512];
    size_t            free_sizes[512];
    int               free_count;
#ifdef _WIN32
    HANDLE            hColdFile;
    HANDLE            hColdMapping;
    HANDLE            hFile;
    HANDLE            hMapping;
#else
    int               cold_fd;
    int               fd;
#endif
    uint8_t           locked;
} DRamTileStore;

/* ── Function declarations from dramtile_store.c ──────────── */
int dt_store_init_twin(DRamTileStore *store, const char *filepath, size_t max_bytes);
void dt_store_destroy_twin(DRamTileStore *store);
int dt_store_load_dir(DRamTileStore *store);

static inline uint8_t *dt_routed_ptr(DRamTileStore *store, uint32_t slot) {
    uint32_t entry = store->hash[slot].dram_addr;
    if (entry & DT_KV_FLAG)
        return store->kv_base + store->hash[slot].offset;
    return store->base + store->hash[slot].offset;
}

/* ── Globals ──────────────────────────────────────────────── */
static DRamTileStore *g_store = NULL;
static char           g_filepath[DT_MAX_PATH];

/* ── FUSE helpers ─────────────────────────────────────────── */
static const char *strip_slash(const char *path) {
    while (*path == '/') path++;
    return path;
}

static int is_dir(const char *path) {
    if (!path || !path[0] || !strcmp(path, "/")) return 1;
    const char *rel = strip_slash(path);
    /* Reconstruct dot-name: "blk/0" → "blk.0" */
    size_t rlen = strlen(rel);
    char *flat = (char *)malloc(rlen + 1);
    if (!flat) return 0;
    int fi = 0;
    for (size_t j = 0; j < rlen; j++)
        flat[fi++] = (rel[j] == '/') ? '.' : rel[j];
    flat[fi] = '\0';
    /* Check if any tensor name starts with "flat." */
    int found = 0;
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (g_store->hash[i].dram_addr == 0) continue;
        if (g_store->hash[i].dram_addr & DT_KV_FLAG) continue;
        const char *name = g_store->hash[i].name;
        if (name[0] && strncmp(name, flat, strlen(flat)) == 0 && name[strlen(flat)] == '.') {
            found = 1;
            break;
        }
    }
    free(flat);
    return found;
}

static int find_tensor(const char *path) {
    const char *rel = strip_slash(path);
    /* Exact match (flat: "blk.0.attn_q.weight") */
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (g_store->hash[i].dram_addr == 0) continue;
        if (g_store->hash[i].dram_addr & DT_KV_FLAG) continue;
        if (!strcmp(g_store->hash[i].name, rel)) return i;
    }
    /* Reconstruct from path separators: "blk/0/attn_q/weight" → "blk.0.attn_q.weight" */
    size_t rlen = strlen(rel);
    char *flat = (char *)malloc(rlen + 1);
    if (!flat) return -1;
    int fi = 0;
    for (size_t j = 0; j < rlen; j++)
        flat[fi++] = (rel[j] == '/') ? '.' : rel[j];
    flat[fi] = '\0';
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (g_store->hash[i].dram_addr == 0) continue;
        if (g_store->hash[i].dram_addr & DT_KV_FLAG) continue;
        if (!strcmp(g_store->hash[i].name, flat)) { free(flat); return i; }
    }
    free(flat);
    return -1;
}

/* ── FUSE operations ──────────────────────────────────────── */

static int dtfs_getattr(const char *path, struct fuse_stat *st) {
    fprintf(stderr, "[FUSE] getattr: %s\n", path);
    memset(st, 0, sizeof(*st));
    if (!strcmp(path, "/") || is_dir(path)) {
        st->st_mode  = S_IFDIR | 0555;
        st->st_nlink = 2;
        fprintf(stderr, "[FUSE] getattr: %s -> DIR (0%o)\n", path, st->st_mode);
        return 0;
    }
    int idx = find_tensor(path);
    if (idx < 0) {
        fprintf(stderr, "[FUSE] getattr: %s -> ENOENT\n", path);
        return -ENOENT;
    }
    st->st_mode  = S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_size  = g_store->hash[idx].size;
    fprintf(stderr, "[FUSE] getattr: %s -> FILE size=%zu\n", path, st->st_size);
    return 0;
}

static int dtfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         fuse_off_t offset, struct fuse_file_info *fi) {
    (void)offset; (void)fi;
    fprintf(stderr, "[FUSE] readdir: %s\n", path);
    filler(buf, ".",  NULL, 0);
    filler(buf, "..", NULL, 0);
    const char *rel  = (!strcmp(path, "/")) ? "" : strip_slash(path);
    /* Convert path separators to dots for matching: "blk/0" → "blk.0" */
    size_t rlen = strlen(rel);
    char rel_dot[256];
    if (rlen >= sizeof(rel_dot)) return 0;
    for (size_t i = 0; i < rlen; i++)
        rel_dot[i] = (rel[i] == '/') ? '.' : rel[i];
    rel_dot[rlen] = '\0';
    char     seen[DT_HASH_SLOTS][64];
    uint32_t n_seen = 0;
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (g_store->hash[i].dram_addr == 0) continue;
        if (g_store->hash[i].dram_addr & DT_KV_FLAG) continue;
        const char *fp = g_store->hash[i].name;
        if (fp[0] == '\0') continue;
        if (rlen > 0) {
            if (strncmp(fp, rel_dot, rlen) != 0 || fp[rlen] != '.') continue;
            fp += rlen + 1;
        }
        const char *dot = strchr(fp, '.');
        char child[64];
        if (dot) {
            size_t len = (size_t)(dot - fp);
            if (len >= sizeof(child)) continue;
            memcpy(child, fp, len); child[len] = 0;
        } else {
            strncpy(child, fp, sizeof(child) - 1);
            child[sizeof(child) - 1] = 0;
        }
        if (!child[0]) continue;
        int dup = 0;
        for (uint32_t j = 0; j < n_seen; j++)
            if (!strcmp(seen[j], child)) { dup = 1; break; }
        if (dup) continue;
        snprintf(seen[n_seen++], sizeof(seen[0]), "%s", child);
        filler(buf, child, NULL, 0);
    }
    return 0;
}

static int dtfs_open(const char *path, struct fuse_file_info *fi) {
    if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EACCES;
    int idx = find_tensor(path);
    if (idx < 0) return -ENOENT;
    fi->fh = (uint64_t)(idx + 1);
    return 0;
}

static int dtfs_read(const char *path, char *buf, size_t size,
                      fuse_off_t offset, struct fuse_file_info *fi) {
    (void)path;
    int idx = (int)(fi->fh - 1);
    if (idx < 0 || idx >= DT_HASH_SLOTS) return -EIO;
    size_t tensor_sz = g_store->hash[idx].size;
    if ((size_t)offset >= tensor_sz) return 0;
    if (offset + size > tensor_sz) size = tensor_sz - (size_t)offset;
    uint8_t *ptr = dt_routed_ptr(g_store, (uint32_t)idx);
    if (!ptr) return -EIO;
    memcpy(buf, ptr + offset, size);
    return (int)size;
}

static int dtfs_statfs(const char *path, struct fuse_statvfs *st) {
    (void)path;
    memset(st, 0, sizeof(*st));
    st->f_bsize  = 4096;
    st->f_frsize = 4096;
    st->f_blocks = (fuse_fsblkcnt_t)(g_store->capacity / 4096);
    st->f_bfree  = (fuse_fsblkcnt_t)((g_store->capacity - g_store->used) / 4096);
    st->f_bavail = st->f_bfree;
    st->f_files  = g_store->n_stored;
    st->f_namemax = DT_NAME_MAX;
    return 0;
}

static void *dtfs_init(struct fuse_conn_info *conn) {
    (void)conn;
    return NULL;
}

static struct fuse_operations dtfs_ops = {
    .getattr = dtfs_getattr,
    .readdir = dtfs_readdir,
    .open    = dtfs_open,
    .read    = dtfs_read,
    .statfs  = dtfs_statfs,
    .init    = dtfs_init,
};

/* ── Main ─────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr,
            "usage: dramtile_mount <file.dramtile> <mountpoint>\n"
            "\n"
            "Mount a DRamTile twin file as a read-only directory.\n"
            "Tensor names with '.' become nested folders.\n"
            "\n"
            "Example:\n"
            "  dramtile_mount model.dramtile M:\\model\n"
            "  dir M:\\model\\blk\\0\\attn_q\n"
            "  type M:\\model\\blk\\0\\attn_q\\weight > weight.bin\n"
        );
        return 1;
    }

    static DRamTileStore store;
    g_store = &store;
    strncpy(g_filepath, argv[1], DT_MAX_PATH - 1);
    g_filepath[DT_MAX_PATH - 1] = '\0';

    if (dt_store_init_twin(&store, g_filepath, 1UL * 1024 * 1024 * 1024) != 0) {
        fprintf(stderr, "failed to open %s\n", g_filepath);
        return 1;
    }

    fprintf(stderr, "DRAMTile mounted: %s (%u tensors, %zu bytes)\n",
            g_filepath, store.n_stored, store.used);

    /* List tensors */
    fprintf(stderr, "\nTensors:\n");
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (store.hash[i].dram_addr == 0) continue;
        if (store.hash[i].dram_addr & DT_KV_FLAG) continue;
        fprintf(stderr, "  %s  (%zu bytes)\n", store.hash[i].name, store.hash[i].size);
    }
    fprintf(stderr, "\nMounting at %s ... (Ctrl+C to unmount)\n", argv[2]);
    fprintf(stderr, "[FUSE] calling fuse_main(%d, ...)\n", argc - 1);

    int ret = fuse_main(argc - 1, argv + 1, &dtfs_ops, NULL);
    fprintf(stderr, "[FUSE] fuse_main returned: %d\n", ret);
    dt_store_destroy_twin(&store);
    return ret;
}
