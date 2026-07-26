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

#include "dramtile_store.h"

static DRamTileStore *g_store = NULL;
static char           g_filepath[DT_MAX_PATH];

static const char *strip_slash(const char *path) {
    while (*path == '/') path++;
    return path;
}

static int is_dir(const char *path) {
    if (!path || !path[0] || !strcmp(path, "/")) return 1;
    const char *rel = strip_slash(path);
    /* Reconstruct dot-name from path: "blk\0" -> "blk.0" */
    size_t rlen = strlen(rel);
    char *flat = (char *)malloc(rlen + 1);
    if (!flat) return 0;
    int fi = 0;
    for (size_t j = 0; j < rlen; j++) {
        flat[fi++] = (rel[j] == '/' || rel[j] == '\\') ? '.' : rel[j];
    }
    flat[fi] = '\0';
    /* Check if any tensor name starts with this prefix + "." */
    int is_directory = 0;
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (g_store->hash[i].dram_addr == 0) continue;
        if (g_store->hash[i].dram_addr & DT_KV_FLAG) continue;
        const char *name = g_store->hash[i].name;
        if (name[0] && strncmp(name, flat, strlen(flat)) == 0 && name[strlen(flat)] == '.') {
            is_directory = 1;
            break;
        }
    }
    free(flat);
    return is_directory;
}

static int find_tensor(const char *path) {
    const char *rel = strip_slash(path);
    /* Try exact match first (flat: "blk.0.attn_q.weight") */
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (g_store->hash[i].dram_addr == 0) continue;
        if (g_store->hash[i].dram_addr & DT_KV_FLAG) continue;
        if (!strcmp(g_store->hash[i].name, rel)) return i;
    }
    /* Try reconstructing dot-name from path separators: "blk\0\attn_q\weight" -> "blk.0.attn_q.weight" */
    size_t rlen = strlen(rel);
    char *flat = (char *)malloc(rlen + 1);
    if (!flat) return -1;
    int fi = 0;
    for (size_t j = 0; j < rlen; j++) {
        flat[fi++] = (rel[j] == '/' || rel[j] == '\\') ? '.' : rel[j];
    }
    flat[fi] = '\0';
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (g_store->hash[i].dram_addr == 0) continue;
        if (g_store->hash[i].dram_addr & DT_KV_FLAG) continue;
        if (!strcmp(g_store->hash[i].name, flat)) { free(flat); return i; }
    }
    free(flat);
    return -1;
}

static int dtfs_getattr(const char *path, struct fuse_stat *st) {
    memset(st, 0, sizeof(*st));
    if (!strcmp(path, "/") || is_dir(path)) {
        st->st_mode  = S_IFDIR | 0555;
        st->st_nlink = 2;
        return 0;
    }
    int idx = find_tensor(path);
    if (idx < 0) return -ENOENT;
    st->st_mode  = S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_size  = g_store->hash[idx].size;
    return 0;
}

static int dtfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         fuse_off_t offset, struct fuse_file_info *fi) {
    (void)offset; (void)fi;
    filler(buf, ".",  NULL, 0);
    filler(buf, "..", NULL, 0);
    const char *rel  = (!strcmp(path, "/")) ? "" : strip_slash(path);
    size_t      rlen = strlen(rel);
    char     seen[DT_HASH_SLOTS][DT_NAME_MAX];
    uint32_t n_seen = 0;
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (g_store->hash[i].dram_addr == 0) continue;
        if (g_store->hash[i].dram_addr & DT_KV_FLAG) continue;
        const char *fp = g_store->hash[i].name;
        if (fp[0] == '\0') continue;
        if (rlen > 0) {
            if (strncmp(fp, rel, rlen) != 0 || fp[rlen] != '.') continue;
            fp += rlen + 1;
        }
        /* Check if this is a leaf (no more dots after this segment) */
        const char *dot = strchr(fp, '.');
        char child[DT_NAME_MAX];
        if (dot) {
            size_t len = (size_t)(dot - fp);
            if (len >= DT_NAME_MAX) continue;
            strncpy(child, fp, len); child[len] = 0;
        } else {
            /* Leaf tensor — show as file */
            strncpy(child, fp, DT_NAME_MAX - 1);
            child[DT_NAME_MAX - 1] = 0;
        }
        if (!child[0]) continue;
        int dup = 0;
        for (uint32_t j = 0; j < n_seen; j++)
            if (!strcmp(seen[j], child)) { dup = 1; break; }
        if (dup) continue;
        snprintf(seen[n_seen++], DT_NAME_MAX, "%s", child);
        filler(buf, child, NULL, 0);
    }
    return 0;
}

static int dtfs_open(const char *path, struct fuse_file_info *fi) {
    int flags = fi->flags & O_ACCMODE;
    if (flags != O_RDONLY) return -EACCES;
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

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: dramtile_mount <file.dramtile> <mountpoint>\n");
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
    fprintf(stderr, "DRamTile mounted: %s (%u tensors, %zu bytes)\n",
            g_filepath, store.n_stored, store.used);
    int ret = fuse_main(argc - 1, argv + 1, &dtfs_ops, NULL);
    dt_store_destroy_twin(&store);
    return ret;
}