/*
 * hbv_cli.c — hb_vault command line tool
 *
 * Build (Windows):
 *   cl hbv_cli.c /I. /O2 /Fe:hbv.exe
 *   or
 *   gcc -O2 -I. -o hbv hbv_cli.c
 *
 * Usage:
 *   hbv pack   <folder>  <out.gpx5>
 *   hbv unpack <in.gpx5> <out_dir>  [--verify]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <dirent.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

#define HB_VAULT_IMPL
#include "hb_vault.h"

/* ── file collector ──────────────────────────────────────────────────── */
#define MAX_FILES 8192

static char  g_paths   [MAX_FILES][512];
static char  g_relnames[MAX_FILES][512];
static int   g_nfiles = 0;

#ifdef _WIN32
static void collect(const char *base, const char *rel) {
    char pattern[512];
    if (rel[0]) snprintf(pattern, sizeof(pattern), "%s\\%s\\*", base, rel);
    else         snprintf(pattern, sizeof(pattern), "%s\\*", base);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char newrel[512];
        if (rel[0]) snprintf(newrel, sizeof(newrel), "%s\\%s", rel, fd.cFileName);
        else         snprintf(newrel, sizeof(newrel), "%s", fd.cFileName);

        char fullpath[512];
        snprintf(fullpath, sizeof(fullpath), "%s\\%s", base, newrel);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            collect(base, newrel);
        } else {
            if (g_nfiles < MAX_FILES) {
                strncpy(g_paths[g_nfiles],    fullpath, 511);
                strncpy(g_relnames[g_nfiles], newrel,   511);
                g_nfiles++;
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
#else
static void collect(const char *base, const char *rel) {
    char dirpath[512];
    if (rel[0]) snprintf(dirpath, sizeof(dirpath), "%s/%s", base, rel);
    else         snprintf(dirpath, sizeof(dirpath), "%s", base);

    DIR *d = opendir(dirpath);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char newrel[512];
        if (rel[0]) snprintf(newrel, sizeof(newrel), "%s/%s", rel, ent->d_name);
        else         snprintf(newrel, sizeof(newrel), "%s", ent->d_name);

        char fullpath[512];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", base, newrel);

        struct stat st;
        if (stat(fullpath, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            collect(base, newrel);
        } else {
            if (g_nfiles < MAX_FILES) {
                strncpy(g_paths[g_nfiles],    fullpath, 511);
                strncpy(g_relnames[g_nfiles], newrel,   511);
                g_nfiles++;
            }
        }
    }
    closedir(d);
}
#endif

/* ── main ────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    if (argc < 4) {
        printf("usage:\n");
        printf("  hbv pack   <folder>  <out.gpx5>\n");
        printf("  hbv unpack <in.gpx5> <out_dir>  [--verify]\n");
        return 1;
    }

    const char *cmd = argv[1];

    /* ── pack ── */
    if (!strcmp(cmd, "pack")) {
        const char *folder   = argv[2];
        const char *out_path = argv[3];

        collect(folder, "");
        if (g_nfiles == 0) {
            fprintf(stderr, "no files found in %s\n", folder);
            return 1;
        }
        printf("packing %d files from %s\n", g_nfiles, folder);

        const char *paths[MAX_FILES], *rels[MAX_FILES];
        for (int i = 0; i < g_nfiles; i++) {
            paths[i] = g_paths[i];
            rels[i]  = g_relnames[i];
        }

        int r = hbv_pack_files(paths, rels, (uint32_t)g_nfiles, out_path);
        if (r == HB_OK) printf("vault → %s\n", out_path);
        else             fprintf(stderr, "pack failed (%d)\n", r);
        return r == HB_OK ? 0 : 1;
    }

    /* ── unpack ── */
    if (!strcmp(cmd, "unpack")) {
        const char *in_path = argv[2];
        const char *out_dir = argv[3];
        int verify = (argc > 4 && !strcmp(argv[4], "--verify")) ? 1 : 0;

        MKDIR(out_dir);
        int r = hbv_unpack(in_path, out_dir, verify);
        return r == HB_OK ? 0 : 1;
    }

    fprintf(stderr, "unknown command: %s\n", cmd);
    return 1;
}
