/*
 * geo_vault_fuse.c — GeoVault FUSE mount layer  (v2: read+write)
 * ═══════════════════════════════════════════════════════════════
 * mount .geovault as a read-write filesystem
 *
 * compile:
 *   gcc -O2 -o gvault_mount geo_vault_fuse.c \
 *       -lzstd -lfuse -D_FILE_OFFSET_BITS=64 -lpthread
 *
 * usage:
 *   gvault_mount <file.geovault> <mountpoint>
 *   ls / cat / cp / rm / mv inside mountpoint
 *   fusermount -u <mountpoint>
 *
 * design (read):
 *   - header + entries loaded at mount time (no decompress yet)
 *   - per-frame lazy decompress on first access (v2)
 *   - reads are O(1) seek into decompressed frame buffer
 *   - directory structure rebuilt from stored paths
 *
 * design (write):
 *   - create/write → write-buffer in memory, flush on release
 *   - release      → gv_update (in-place) or gv_add (append)
 *   - unlink       → removes entry; vault compacted on unmount if dirty
 *   - rename       → updates path + rebuilds hash index
 *   - mkdir        → virtual (dirs are implicit from file paths)
 *   - all mutations serialised by g_lock
 * ═══════════════════════════════════════════════════════════════
 */

#define FUSE_USE_VERSION 26
#define _FILE_OFFSET_BITS 64

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include "geo_vault.h"
#include "geo_vault_io.h"

/* ── global state ─────────────────────────────────────────────────── */
static GeoVaultCtx    *g_vault      = NULL;
static char            g_vault_path[4096];
static pthread_mutex_t g_lock       = PTHREAD_MUTEX_INITIALIZER;
static int             g_dirty      = 0;   /* 1 = unlink/rename → compact on unmount */

/* ── write buffer table ───────────────────────────────────────────────
 * Files opened for writing get a per-fh in-memory buffer.
 * Flushed to vault on release().
 * ──────────────────────────────────────────────────────────────────── */
#define GV_WBUF_MAX 256

typedef struct {
    uint64_t  fh;
    char      path[GV_MAX_PATH];
    uint8_t  *data;
    uint32_t  size;
    uint32_t  cap;
    int       dirty;
} GvWriteBuf;

static GvWriteBuf g_wbufs[GV_WBUF_MAX];
static uint32_t   g_wbuf_n  = 0;
static uint64_t   g_next_fh = 1;

static GvWriteBuf *_wbuf_get(uint64_t fh) {
    for (uint32_t i = 0; i < g_wbuf_n; i++)
        if (g_wbufs[i].fh == fh) return &g_wbufs[i];
    return NULL;
}

static GvWriteBuf *_wbuf_alloc(uint64_t fh, const char *path,
                                const uint8_t *init, uint32_t init_sz) {
    if (g_wbuf_n >= GV_WBUF_MAX) return NULL;
    GvWriteBuf *wb = &g_wbufs[g_wbuf_n++];
    wb->fh    = fh;
    wb->dirty = 0;
    strncpy(wb->path, path, GV_MAX_PATH - 1);
    wb->path[GV_MAX_PATH - 1] = 0;
    wb->cap  = init_sz ? init_sz : 4096;
    wb->data = malloc(wb->cap);
    wb->size = init_sz;
    if (init_sz && init) memcpy(wb->data, init, init_sz);
    return wb;
}

static void _wbuf_free(uint64_t fh) {
    for (uint32_t i = 0; i < g_wbuf_n; i++) {
        if (g_wbufs[i].fh == fh) {
            free(g_wbufs[i].data);
            g_wbufs[i] = g_wbufs[--g_wbuf_n];
            return;
        }
    }
}

static int _wbuf_write(GvWriteBuf *wb, const char *buf,
                        size_t size, off_t offset) {
    uint32_t end = (uint32_t)offset + (uint32_t)size;
    if (end > wb->cap) {
        uint32_t newcap = wb->cap * 2;
        while (newcap < end) newcap *= 2;
        wb->data = realloc(wb->data, newcap);
        wb->cap  = newcap;
    }
    if ((uint32_t)offset > wb->size)
        memset(wb->data + wb->size, 0, (uint32_t)offset - wb->size);
    memcpy(wb->data + offset, buf, size);
    if (end > wb->size) wb->size = end;
    wb->dirty = 1;
    return (int)size;
}

/* ── path helpers ─────────────────────────────────────────────────── */

static const char *strip_slash(const char *path) {
    while (*path == '/') path++;
    return path;
}

static int is_vault_dir(const char *path) {
    if (!path || !path[0] || !strcmp(path, "/")) return 1;
    const char *rel  = strip_slash(path);
    size_t      rlen = strlen(rel);
    for (uint32_t i = 0; i < g_vault->hdr.n_files; i++) {
        const char *fp = g_vault->paths[i];
        if (strncmp(fp, rel, rlen) == 0 && fp[rlen] == '/') return 1;
    }
    return 0;
}

static int find_file(const char *path) {
    return gv_find(g_vault, strip_slash(path));
}

/* ── ensure frame(s) for a file index are loaded ─────────────────── */
static int _ensure_frames(int idx) {
    if (!g_vault->frame_cache) return gv_load_frames(g_vault);
    GeoVaultFileEntry *e   = &g_vault->entries[idx];
    uint32_t global_end    = e->frame_start * GV_RESIDUAL + e->slot_start
                           + (e->slot_count > 0 ? e->slot_count - 1 : 0);
    uint32_t frame_end     = global_end / GV_RESIDUAL;
    for (uint32_t f = e->frame_start; f <= frame_end; f++)
        if (!g_vault->frame_cache[f])
            if (gv_load_frame(g_vault, f) < 0) return -1;
    return 0;
}

/* ── FUSE ops ─────────────────────────────────────────────────────── */

static int gvfs_getattr(const char *path, struct stat *st) {
    memset(st, 0, sizeof(*st));
    if (!strcmp(path, "/") || is_vault_dir(path)) {
        st->st_mode  = S_IFDIR | 0755;
        st->st_nlink = 2;
        return 0;
    }
    int idx = find_file(path);
    if (idx < 0) return -ENOENT;
    st->st_mode  = S_IFREG | 0644;
    st->st_nlink = 1;
    st->st_size  = g_vault->entries[idx].raw_size;
    st->st_ino   = (ino_t)((uint64_t)g_vault->entries[idx].frame_start
                           * GV_RESIDUAL + g_vault->entries[idx].slot_start + 1);
    return 0;
}

static int gvfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi) {
    (void)offset; (void)fi;
    filler(buf, ".",  NULL, 0);
    filler(buf, "..", NULL, 0);

    const char *rel  = (!strcmp(path, "/")) ? "" : strip_slash(path);
    size_t      rlen = strlen(rel);
    char     seen[GV_MAX_FILES][GV_MAX_PATH];
    uint32_t n_seen = 0;

    for (uint32_t i = 0; i < g_vault->hdr.n_files; i++) {
        const char *fp = g_vault->paths[i];
        if (rlen > 0) {
            if (strncmp(fp, rel, rlen) != 0 || fp[rlen] != '/') continue;
            fp += rlen + 1;
        }
        char        child[GV_MAX_PATH];
        const char *slash = strchr(fp, '/');
        if (slash) {
            size_t len = (size_t)(slash - fp);
            if (len >= GV_MAX_PATH) continue;
            strncpy(child, fp, len); child[len] = 0;
        } else {
            strncpy(child, fp, GV_MAX_PATH - 1);
            child[GV_MAX_PATH - 1] = 0;
        }
        if (!child[0]) continue;
        int dup = 0;
        for (uint32_t j = 0; j < n_seen; j++)
            if (!strcmp(seen[j], child)) { dup = 1; break; }
        if (dup) continue;
        snprintf(seen[n_seen++], GV_MAX_PATH, "%s", child);
        filler(buf, child, NULL, 0);
    }
    return 0;
}

static int gvfs_open(const char *path, struct fuse_file_info *fi) {
    int idx   = find_file(path);
    int flags = fi->flags & O_ACCMODE;

    if (flags == O_WRONLY || flags == O_RDWR) {
        pthread_mutex_lock(&g_lock);
        uint64_t fh = g_next_fh++;

        uint8_t *init_data = NULL;
        uint32_t init_sz   = 0;
        if (idx >= 0 && !(fi->flags & O_TRUNC)) {
            if (_ensure_frames(idx) == 0) {
                init_data = gv_extract(g_vault, (uint32_t)idx);
                init_sz   = g_vault->entries[idx].raw_size;
            }
        }
        GvWriteBuf *wb = _wbuf_alloc(fh, strip_slash(path), init_data, init_sz);
        free(init_data);
        pthread_mutex_unlock(&g_lock);
        if (!wb) return -ENOMEM;
        fi->fh = fh;
        return 0;
    }

    if (idx < 0) return -ENOENT;
    pthread_mutex_lock(&g_lock);
    int r = _ensure_frames(idx);
    pthread_mutex_unlock(&g_lock);
    fi->fh = 0;
    return (r < 0) ? -EIO : 0;
}

static int gvfs_create(const char *path, mode_t mode,
                        struct fuse_file_info *fi) {
    (void)mode;
    pthread_mutex_lock(&g_lock);
    uint64_t    fh = g_next_fh++;
    GvWriteBuf *wb = _wbuf_alloc(fh, strip_slash(path), NULL, 0);
    pthread_mutex_unlock(&g_lock);
    if (!wb) return -ENOMEM;
    fi->fh = fh;
    return 0;
}

static int gvfs_read(const char *path, char *buf, size_t size,
                      off_t offset, struct fuse_file_info *fi) {
    /* read from write-buffer if open for write */
    if (fi->fh) {
        pthread_mutex_lock(&g_lock);
        GvWriteBuf *wb = _wbuf_get(fi->fh);
        if (wb) {
            if ((uint64_t)offset >= wb->size) { pthread_mutex_unlock(&g_lock); return 0; }
            if ((uint64_t)offset + size > wb->size)
                size = (size_t)(wb->size - (uint64_t)offset);
            memcpy(buf, wb->data + offset, size);
            pthread_mutex_unlock(&g_lock);
            return (int)size;
        }
        pthread_mutex_unlock(&g_lock);
    }

    int idx = find_file(path);
    if (idx < 0) return -ENOENT;

    GeoVaultFileEntry *e = &g_vault->entries[idx];
    uint32_t raw_size    = e->raw_size;
    if ((uint64_t)offset >= raw_size) return 0;
    if ((uint64_t)offset + size > raw_size)
        size = (size_t)(raw_size - (uint64_t)offset);

    uint32_t chunk_start   = (uint32_t)(offset / GV_CHUNK_BYTES);
    uint32_t byte_in_chunk = (uint32_t)(offset % GV_CHUNK_BYTES);
    size_t   written       = 0;
    uint32_t global_slot   = e->frame_start * GV_RESIDUAL
                           + e->slot_start + chunk_start;

    while (written < size) {
        uint32_t frame    = global_slot / GV_RESIDUAL;
        uint32_t slot_inf = global_slot % GV_RESIDUAL;

        pthread_mutex_lock(&g_lock);
        uint8_t *fbuf = gv_get_frame(g_vault, frame);
        pthread_mutex_unlock(&g_lock);
        if (!fbuf) return -EIO;

        uint8_t  chunk[GV_CHUNK_BYTES];
        uint32_t src_abs     = (chunk_start + (uint32_t)(written / GV_CHUNK_BYTES))
                               * GV_CHUNK_BYTES;
        uint32_t remain_file = raw_size > src_abs ? raw_size - src_abs : 0;
        gv_unpack_chunk(fbuf, slot_inf, chunk,
                        remain_file < GV_CHUNK_BYTES ? remain_file : GV_CHUNK_BYTES);

        uint32_t from  = (written == 0) ? byte_in_chunk : 0;
        uint32_t avail = GV_CHUNK_BYTES - from;
        uint32_t take  = avail < (uint32_t)(size - written)
                       ? avail : (uint32_t)(size - written);
        memcpy(buf + written, chunk + from, take);
        written      += take;
        global_slot++;
        byte_in_chunk = 0;
    }
    return (int)written;
}

static int gvfs_write(const char *path, const char *buf, size_t size,
                       off_t offset, struct fuse_file_info *fi) {
    (void)path;
    pthread_mutex_lock(&g_lock);
    GvWriteBuf *wb = _wbuf_get(fi->fh);
    int r = wb ? _wbuf_write(wb, buf, size, offset) : -EBADF;
    pthread_mutex_unlock(&g_lock);
    return r;
}

static int gvfs_truncate(const char *path, off_t size) {
    pthread_mutex_lock(&g_lock);

    /* truncate open write buffer if path matches */
    for (uint32_t i = 0; i < g_wbuf_n; i++) {
        if (strcmp(g_wbufs[i].path, strip_slash(path)) == 0) {
            GvWriteBuf *wb = &g_wbufs[i];
            uint32_t newsz = (uint32_t)size;
            if (newsz > wb->cap) {
                wb->data = realloc(wb->data, newsz);
                wb->cap  = newsz;
            }
            if (newsz > wb->size)
                memset(wb->data + wb->size, 0, newsz - wb->size);
            wb->size  = newsz;
            wb->dirty = 1;
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
    }

    int idx = gv_find(g_vault, strip_slash(path));
    if (idx < 0) { pthread_mutex_unlock(&g_lock); return -ENOENT; }

    uint32_t newsz = (uint32_t)size;
    GeoVaultFileEntry *e = &g_vault->entries[idx];
    if (newsz == e->raw_size) { pthread_mutex_unlock(&g_lock); return 0; }

    _ensure_frames(idx);
    uint8_t *data    = gv_extract(g_vault, (uint32_t)idx);
    uint8_t *newdata = calloc(1, newsz ? newsz : 1);
    if (data && newsz > 0)
        memcpy(newdata, data, newsz < e->raw_size ? newsz : e->raw_size);
    free(data);

    int r = (newsz <= e->raw_size)
          ? gv_update(g_vault, strip_slash(path), newdata, newsz)
          : gv_add   (g_vault, strip_slash(path), newdata, newsz);
    free(newdata);
    pthread_mutex_unlock(&g_lock);
    return (r < 0) ? -EIO : 0;
}

static int gvfs_release(const char *path, struct fuse_file_info *fi) {
    (void)path;
    if (!fi->fh) return 0;

    pthread_mutex_lock(&g_lock);
    GvWriteBuf *wb = _wbuf_get(fi->fh);
    if (!wb || !wb->dirty) {
        if (wb) _wbuf_free(fi->fh);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    /* copy fields before _wbuf_free invalidates pointer */
    char     wpath[GV_MAX_PATH];
    uint32_t wsize = wb->size;
    uint8_t *wdata = malloc(wsize ? wsize : 1);
    memcpy(wdata, wb->data, wsize);
    strncpy(wpath, wb->path, GV_MAX_PATH - 1);
    wpath[GV_MAX_PATH - 1] = 0;
    _wbuf_free(fi->fh);

    int idx = gv_find(g_vault, wpath);
    int r;
    if (idx >= 0 && wsize <= g_vault->entries[idx].raw_size) {
        r = gv_update(g_vault, wpath, wdata, wsize);
        if (r == -3) r = gv_add(g_vault, wpath, wdata, wsize);
    } else {
        r = gv_add(g_vault, wpath, wdata, wsize);
    }
    free(wdata);
    pthread_mutex_unlock(&g_lock);

    if (r < 0) return -EIO;
    fprintf(stderr, "[gvault] flushed %s  %u bytes\n", wpath, wsize);
    return 0;
}

static int gvfs_unlink(const char *path) {
    pthread_mutex_lock(&g_lock);
    const char *rel = strip_slash(path);
    int idx = gv_find(g_vault, rel);
    if (idx < 0) { pthread_mutex_unlock(&g_lock); return -ENOENT; }

    uint32_t n = g_vault->hdr.n_files;
    free(g_vault->paths[idx]);
    g_vault->paths[idx]   = g_vault->paths[n - 1];
    g_vault->entries[idx] = g_vault->entries[n - 1];
    g_vault->hdr.n_files--;
    g_dirty = 1;
    _hidx_build(g_vault);

    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "[gvault] unlink %s  (compact on unmount)\n", rel);
    return 0;
}

static int gvfs_rename(const char *from, const char *to) {
    pthread_mutex_lock(&g_lock);
    const char *rel_from = strip_slash(from);
    const char *rel_to   = strip_slash(to);

    int idx = gv_find(g_vault, rel_from);
    if (idx < 0) { pthread_mutex_unlock(&g_lock); return -ENOENT; }

    free(g_vault->paths[idx]);
    g_vault->paths[idx]             = strdup(rel_to);
    g_vault->entries[idx].name_hash = gv_fnv1a(rel_to);
    g_dirty = 1;
    _hidx_build(g_vault);

    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "[gvault] rename %s -> %s\n", rel_from, rel_to);
    return 0;
}

static int gvfs_mkdir(const char *path, mode_t mode) {
    (void)path; (void)mode;
    return 0;  /* virtual — dirs implicit from paths */
}

static int gvfs_rmdir(const char *path) {
    const char *rel  = strip_slash(path);
    size_t      rlen = strlen(rel);
    for (uint32_t i = 0; i < g_vault->hdr.n_files; i++) {
        const char *fp = g_vault->paths[i];
        if (strncmp(fp, rel, rlen) == 0 && fp[rlen] == '/')
            return -ENOTEMPTY;
    }
    return 0;
}

static int gvfs_statfs(const char *path, struct statvfs *sv) {
    (void)path;
    memset(sv, 0, sizeof(*sv));
    sv->f_bsize   = GV_CHUNK_BYTES;
    sv->f_blocks  = g_vault->hdr.total_raw / GV_CHUNK_BYTES;
    sv->f_bfree   = (fsblkcnt_t)(GV_RESIDUAL * 1024u);
    sv->f_bavail  = sv->f_bfree;
    sv->f_files   = g_vault->hdr.n_files;
    sv->f_ffree   = GV_MAX_FILES - g_vault->hdr.n_files;
    sv->f_namemax = GV_MAX_PATH;
    return 0;
}

static int gvfs_utimens(const char *path, const struct timespec tv[2]) {
    (void)path; (void)tv; return 0;
}
static int gvfs_chmod(const char *path, mode_t mode) {
    (void)path; (void)mode; return 0;
}
static int gvfs_chown(const char *path, uid_t uid, gid_t gid) {
    (void)path; (void)uid; (void)gid; return 0;
}


/* ── FUSE ops: xattr ──────────────────────────────────────────────── */

static int gvfs_setxattr(const char *path, const char *name,
                          const char *value, size_t size, int flags) {
    (void)flags;
    pthread_mutex_lock(&g_lock);
    int idx = find_file(path);
    int r   = (idx < 0) ? -ENOENT
            : gv_xattr_set(g_vault, idx, name, value, (uint32_t)size);
    pthread_mutex_unlock(&g_lock);
    return (r < 0) ? -ENOSPC : 0;
}

static int gvfs_getxattr(const char *path, const char *name,
                          char *value, size_t size) {
    pthread_mutex_lock(&g_lock);
    int idx = find_file(path);
    if (idx < 0) { pthread_mutex_unlock(&g_lock); return -ENOENT; }
    int r = gv_xattr_get(g_vault, idx, name, value, (uint32_t)size);
    pthread_mutex_unlock(&g_lock);
    return (r < 0) ? -ENODATA : r;
}

static int gvfs_listxattr(const char *path, char *list, size_t size) {
    pthread_mutex_lock(&g_lock);
    int idx = find_file(path);
    if (idx < 0) { pthread_mutex_unlock(&g_lock); return -ENOENT; }
    int r = gv_xattr_list(g_vault, idx, list, (uint32_t)size);
    pthread_mutex_unlock(&g_lock);
    return (r < 0) ? -ERANGE : r;
}

static int gvfs_removexattr(const char *path, const char *name) {
    pthread_mutex_lock(&g_lock);
    int idx = find_file(path);
    if (idx < 0) { pthread_mutex_unlock(&g_lock); return -ENOENT; }
    int r = gv_xattr_remove(g_vault, idx, name);
    pthread_mutex_unlock(&g_lock);
    return (r < 0) ? -ENODATA : 0;
}

/* ── op table ─────────────────────────────────────────────────────── */
static struct fuse_operations gvfs_ops = {
    .getattr  = gvfs_getattr,
    .readdir  = gvfs_readdir,
    .open     = gvfs_open,
    .create   = gvfs_create,
    .read     = gvfs_read,
    .write    = gvfs_write,
    .truncate = gvfs_truncate,
    .release  = gvfs_release,
    .unlink   = gvfs_unlink,
    .rename   = gvfs_rename,
    .mkdir    = gvfs_mkdir,
    .rmdir    = gvfs_rmdir,
    .statfs   = gvfs_statfs,
    .utimens  = gvfs_utimens,
    .chmod    = gvfs_chmod,
    .chown      = gvfs_chown,
    .setxattr   = gvfs_setxattr,
    .getxattr   = gvfs_getxattr,
    .listxattr  = gvfs_listxattr,
    .removexattr= gvfs_removexattr,
};

/* ── main ─────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr,
            "GeoVault FUSE mount (read+write)\n"
            "  usage:   gvault_mount <file.geovault> <mountpoint> [fuse opts]\n"
            "  unmount: fusermount -u <mountpoint>\n");
        return 1;
    }

    snprintf(g_vault_path, sizeof(g_vault_path), "%s", argv[1]);

    g_vault = gv_open_rw(argv[1]);
    if (!g_vault) {
        fprintf(stderr, "Cannot open vault: %s\n", argv[1]);
        return 1;
    }

    fprintf(stderr, "[gvault] mounted %s -> %s  (read+write)\n",
            argv[1], argv[2]);
    fprintf(stderr, "[gvault] files=%u  raw=%.1f KB  compressed=%.1f KB\n",
            g_vault->hdr.n_files,
            (double)g_vault->hdr.total_raw    / 1024.0,
            (double)g_vault->hdr.compressed_sz / 1024.0);

    char *fuse_argv[64];
    int   fuse_argc = 0;
    fuse_argv[fuse_argc++] = argv[0];
    fuse_argv[fuse_argc++] = argv[2];
    fuse_argv[fuse_argc++] = "-f";
    fuse_argv[fuse_argc++] = "-o";
    fuse_argv[fuse_argc++] = "allow_other,default_permissions";
    for (int i = 3; i < argc && fuse_argc < 63; i++)
        fuse_argv[fuse_argc++] = argv[i];

    int ret = fuse_main(fuse_argc, fuse_argv, &gvfs_ops, NULL);

    if (g_dirty) {
        fprintf(stderr, "[gvault] dirty — compacting on unmount...\n");
        gv_compact(g_vault, g_vault_path);
    }

    gv_close(g_vault);
    return ret;
}
