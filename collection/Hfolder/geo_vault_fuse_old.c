/*
 * geo_vault_fuse.c — GeoVault FUSE mount layer
 * ═════════════════════════════════════════════
 * mount .geovault as a read-only filesystem
 *
 * compile:
 *   gcc -O2 -o gvault_mount geo_vault_fuse.c \
 *       -lzstd -lfuse -D_FILE_OFFSET_BITS=64
 *
 * usage:
 *   gvault_mount <file.geovault> <mountpoint>
 *   ls <mountpoint>/
 *   cat <mountpoint>/README.md
 *   fusermount -u <mountpoint>
 *
 * design:
 *   - header + entries loaded at mount time (no decompress yet)
 *   - pixel buffer decompressed ONCE on first read (lazy)
 *   - reads are O(1) seek into decompressed frame buffer
 *   - directory structure rebuilt from stored paths
 *   - no write support (passive container)
 * ═════════════════════════════════════════════
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

/* ── global vault context ─────────────────────────────────────────── */
static GeoVaultCtx *g_vault = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── path helpers ─────────────────────────────────────────────────── */

/* strip leading slash for vault lookup */
static const char *strip_slash(const char *path) {
    while (*path == '/') path++;
    return path;
}

/* check if path is a directory prefix of any vault file */
static int is_vault_dir(const char *path) {
    if (!path || !path[0] || !strcmp(path, "/")) return 1;
    const char *rel = strip_slash(path);
    size_t rlen = strlen(rel);
    for (uint32_t i = 0; i < g_vault->hdr.n_files; i++) {
        const char *fp = g_vault->paths[i];
        /* fp starts with rel/ → rel is a directory */
        if (strncmp(fp, rel, rlen) == 0 && fp[rlen] == '/') return 1;
    }
    return 0;
}

/* find file index by path, -1 if not found */
static int find_file(const char *path) {
    const char *rel = strip_slash(path);
    return gv_find(g_vault, rel);
}

/* ── FUSE ops ─────────────────────────────────────────────────────── */

static int gvfs_getattr(const char *path, struct stat *st) {
    memset(st, 0, sizeof(*st));

    if (!strcmp(path, "/") || is_vault_dir(path)) {
        st->st_mode  = S_IFDIR | 0555;
        st->st_nlink = 2;
        return 0;
    }

    int idx = find_file(path);
    if (idx < 0) return -ENOENT;

    st->st_mode  = S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_size  = g_vault->entries[idx].raw_size;
    /* geometric address as inode — compound×pattern gives unique id */
    GeoVaultAddr a = gv_addr(g_vault->entries[idx].slot_start);
    st->st_ino   = (ino_t)((uint64_t)g_vault->entries[idx].frame_start * GV_RESIDUAL
                           + g_vault->entries[idx].slot_start + 1);
    (void)a;
    return 0;
}

static int gvfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi) {
    (void)offset; (void)fi;

    filler(buf, ".",  NULL, 0);
    filler(buf, "..", NULL, 0);

    const char *rel = (!strcmp(path, "/")) ? "" : strip_slash(path);
    size_t rlen = strlen(rel);

    /* collect direct children (files and subdirs) — deduplicate */
    char seen[GV_MAX_FILES][GV_MAX_PATH];
    uint32_t n_seen = 0;

    for (uint32_t i = 0; i < g_vault->hdr.n_files; i++) {
        const char *fp = g_vault->paths[i];

        /* check if fp is under rel/ */
        if (rlen > 0) {
            if (strncmp(fp, rel, rlen) != 0 || fp[rlen] != '/') continue;
            fp += rlen + 1;  /* relative to current dir */
        }

        /* extract direct child name (up to next slash) */
        char child[GV_MAX_PATH];
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

        /* dedup */
        int dup = 0;
        for (uint32_t j = 0; j < n_seen; j++)
            if (!strcmp(seen[j], child)) { dup = 1; break; }
        if (dup) continue;

        strncpy(seen[n_seen++], child, GV_MAX_PATH - 1);
        filler(buf, child, NULL, 0);
    }

    return 0;
}

static int gvfs_open(const char *path, struct fuse_file_info *fi) {
    int idx = find_file(path);
    if (idx < 0) return -ENOENT;
    if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EACCES;

    pthread_mutex_lock(&g_lock);

    if (g_vault->frame_cache) {
        /* v2: load only the frames this file spans */
        GeoVaultFileEntry *e = &g_vault->entries[idx];
        uint32_t global_end  = e->frame_start * GV_RESIDUAL
                             + e->slot_start + e->slot_count - 1;
        uint32_t frame_end   = global_end / GV_RESIDUAL;
        int loaded = 0;
        for (uint32_t f = e->frame_start; f <= frame_end; f++) {
            if (!g_vault->frame_cache[f]) {
                if (gv_load_frame(g_vault, f) < 0) {
                    pthread_mutex_unlock(&g_lock);
                    return -EIO;
                }
                loaded++;
            }
        }
        if (loaded)
            fprintf(stderr, "[gvault] loaded %d frame(s) for %s  (frame %u..%u)\n",
                    loaded, path + (path[0]=='/'?1:0), e->frame_start, frame_end);
    } else {
        /* v1 compat: decompress everything once */
        if (!g_vault->frames) {
            fprintf(stderr, "[gvault] decompressing pixel buffer (v1)...\n");
            if (gv_load_frames(g_vault) < 0) {
                pthread_mutex_unlock(&g_lock);
                return -EIO;
            }
            fprintf(stderr, "[gvault] ready  frames=%u  %.1f KB\n",
                    g_vault->hdr.n_frames,
                    (double)g_vault->frames_size / 1024.0);
        }
    }

    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int gvfs_read(const char *path, char *buf, size_t size,
                      off_t offset, struct fuse_file_info *fi) {
    (void)fi;

    int idx = find_file(path);
    if (idx < 0) return -ENOENT;

    GeoVaultFileEntry *e = &g_vault->entries[idx];

    uint32_t raw_size = e->raw_size;
    if ((uint64_t)offset >= raw_size) return 0;
    if ((uint64_t)offset + size > raw_size)
        size = (size_t)(raw_size - (uint64_t)offset);

    uint32_t chunk_start   = (uint32_t)(offset / GV_CHUNK_BYTES);
    uint32_t byte_in_chunk = (uint32_t)(offset % GV_CHUNK_BYTES);

    size_t   written     = 0;
    uint32_t global_slot = e->frame_start * GV_RESIDUAL
                         + e->slot_start
                         + chunk_start;

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
        uint32_t take  = avail < (uint32_t)(size - written) ? avail : (uint32_t)(size - written);

        memcpy(buf + written, chunk + from, take);
        written += take;
        global_slot++;
        byte_in_chunk = 0;
    }

    return (int)written;
}

static int gvfs_statfs(const char *path, struct statvfs *sv) {
    (void)path;
    memset(sv, 0, sizeof(*sv));
    sv->f_bsize   = GV_CHUNK_BYTES;
    sv->f_blocks  = g_vault->hdr.total_raw / GV_CHUNK_BYTES;
    sv->f_bfree   = 0;
    sv->f_bavail  = 0;
    sv->f_files   = g_vault->hdr.n_files;
    sv->f_ffree   = 0;
    sv->f_namemax = GV_MAX_PATH;
    return 0;
}

static struct fuse_operations gvfs_ops = {
    .getattr = gvfs_getattr,
    .readdir = gvfs_readdir,
    .open    = gvfs_open,
    .read    = gvfs_read,
    .statfs  = gvfs_statfs,
};

/* ── main ─────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr,
            "GeoVault FUSE mount\n"
            "  usage: gvault_mount <file.geovault> <mountpoint> [fuse opts]\n"
            "  unmount: fusermount -u <mountpoint>\n");
        return 1;
    }

    const char *vault_path = argv[1];
    const char *mountpoint = argv[2];

    /* open vault (header + entries only, no decompress yet) */
    g_vault = gv_open(vault_path);
    if (!g_vault) {
        fprintf(stderr, "Cannot open vault: %s\n", vault_path);
        return 1;
    }

    fprintf(stderr, "[gvault] mounted %s → %s\n", vault_path, mountpoint);
    fprintf(stderr, "[gvault] files=%u  raw=%.1f KB  compressed=%.1f KB\n",
            g_vault->hdr.n_files,
            (double)g_vault->hdr.total_raw / 1024.0,
            (double)g_vault->hdr.compressed_sz / 1024.0);
    fprintf(stderr, "[gvault] pixel buffer decompression: lazy (on first read)\n");

    /* build fuse argv: [prog, mountpoint, -f, ...extra] */
    char *fuse_argv[64];
    int   fuse_argc = 0;
    fuse_argv[fuse_argc++] = argv[0];
    fuse_argv[fuse_argc++] = (char*)mountpoint;
    fuse_argv[fuse_argc++] = "-f";          /* foreground */
    fuse_argv[fuse_argc++] = "-o";
    fuse_argv[fuse_argc++] = "ro,allow_other,default_permissions";
    /* pass any extra fuse opts from argv[3..] */
    for (int i = 3; i < argc && fuse_argc < 63; i++)
        fuse_argv[fuse_argc++] = argv[i];

    int ret = fuse_main(fuse_argc, fuse_argv, &gvfs_ops, NULL);

    gv_close(g_vault);
    return ret;
}
