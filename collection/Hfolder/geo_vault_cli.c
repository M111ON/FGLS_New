/*
 * geo_vault_cli.c — GeoVault command-line tool
 * compile: gcc -O2 -o gvault geo_vault_cli.c -lzstd
 *
 * Usage:
 *   gvault pack   <folder>          → folder.geovault
 *   gvault unpack <file.geovault> [outdir]
 *   gvault list   <file.geovault>
 *   gvault verify <folder> <file.geovault>
 *   gvault cat    <file.geovault> <rel_path>   → stdout
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include "geo_vault.h"
#include "geo_vault_io.h"

/* ── recursive dir scan ──────────────────────────────────────────── */
static char  **g_abs_paths = NULL;
static char  **g_rel_paths = NULL;
static uint32_t g_n = 0;
static uint32_t g_cap = 0;

static void push_file(const char *abs, const char *rel) {
    if (g_n >= g_cap) {
        g_cap = g_cap ? g_cap * 2 : 64;
        g_abs_paths = realloc(g_abs_paths, g_cap * sizeof(char*));
        g_rel_paths = realloc(g_rel_paths, g_cap * sizeof(char*));
    }
    g_abs_paths[g_n] = strdup(abs);
    g_rel_paths[g_n] = strdup(rel);
    g_n++;
}

static void scan_dir(const char *abs_base, const char *rel_base) {
    DIR *d = opendir(abs_base);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char abs[4096], rel[4096];
        snprintf(abs, sizeof(abs), "%s/%s", abs_base, de->d_name);
        if (rel_base[0])
            snprintf(rel, sizeof(rel), "%s/%s", rel_base, de->d_name);
        else
            snprintf(rel, sizeof(rel), "%s", de->d_name);

        struct stat st;
        if (stat(abs, &st) != 0) continue;
        if (S_ISDIR(st.st_mode))  scan_dir(abs, rel);
        else if (S_ISREG(st.st_mode)) push_file(abs, rel);
    }
    closedir(d);
}

static void free_paths(void) {
    for (uint32_t i = 0; i < g_n; i++) { free(g_abs_paths[i]); free(g_rel_paths[i]); }
    free(g_abs_paths); free(g_rel_paths);
    g_abs_paths = g_rel_paths = NULL; g_n = g_cap = 0;
}

/* ── mkdir -p ─────────────────────────────────────────────────────── */
static void mkdirp(const char *path) {
    char tmp[4096]; snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

/* ── commands ─────────────────────────────────────────────────────── */
static int cmd_pack(const char *folder) {
    /* strip trailing slash */
    char base[4096]; snprintf(base, sizeof(base), "%s", folder);
    size_t bl = strlen(base);
    if (bl && base[bl-1] == '/') base[bl-1] = 0;

    scan_dir(base, "");
    if (g_n == 0) { fprintf(stderr, "No files found in %s\n", folder); return 1; }

    char out[4096]; snprintf(out, sizeof(out), "%s.geovault", base);

    printf("Packing %u files from %s\n", g_n, base);
    int r = gv_write((const char**)g_abs_paths,
                     (const char**)g_rel_paths, g_n, out);
    if (r == 0) printf("Created: %s\n", out);
    else        fprintf(stderr, "Pack failed: %d\n", r);

    free_paths();
    return r == 0 ? 0 : 1;
}

static int cmd_unpack(const char *vault_path, const char *outdir) {
    GeoVaultCtx *ctx = gv_open(vault_path);
    if (!ctx) { fprintf(stderr, "Cannot open %s\n", vault_path); return 1; }

    /* determine output directory */
    char base[4096];
    if (outdir) {
        snprintf(base, sizeof(base), "%s", outdir);
    } else {
        /* strip .geovault suffix */
        snprintf(base, sizeof(base), "%s", vault_path);
        char *dot = strstr(base, ".geovault");
        if (dot) *dot = 0;
        strcat(base, "_out");
    }
    mkdirp(base);

    printf("Unpacking %u files to %s\n", ctx->hdr.n_files, base);
    if (gv_load_frames(ctx) < 0) {
        fprintf(stderr, "Failed to decompress pixel buffer\n");
        gv_close(ctx); return 1;
    }

    uint32_t ok = 0;
    for (uint32_t i = 0; i < ctx->hdr.n_files; i++) {
        uint8_t *data = gv_extract(ctx, i);
        if (!data) { printf("  [!] %s  (extract failed)\n", ctx->paths[i]); continue; }

        char out[4096]; snprintf(out, sizeof(out), "%s/%s", base, ctx->paths[i]);
        /* mkdir parent */
        char parent[4096]; snprintf(parent, sizeof(parent), "%s", out);
        char *slash = strrchr(parent, '/');
        if (slash) { *slash = 0; mkdirp(parent); }

        FILE *f = fopen(out, "wb");
        if (f) { fwrite(data, 1, ctx->entries[i].raw_size, f); fclose(f); ok++; }
        else   { fprintf(stderr, "  [!] cannot write %s\n", out); }

        printf("  [%c] %s (%u B)\n", f ? 'v' : '!', ctx->paths[i],
               ctx->entries[i].raw_size);
        free(data);
    }

    printf("\n%u/%u extracted\n", ok, ctx->hdr.n_files);
    gv_close(ctx);
    return ok == ctx->hdr.n_files ? 0 : 1;
}

static int cmd_list(const char *vault_path) {
    GeoVaultCtx *ctx = gv_open(vault_path);
    if (!ctx) { fprintf(stderr, "Cannot open %s\n", vault_path); return 1; }
    gv_list(ctx);
    gv_close(ctx);
    return 0;
}

static int cmd_verify(const char *folder, const char *vault_path) {
    /* scan folder */
    char base[4096]; snprintf(base, sizeof(base), "%s", folder);
    size_t bl = strlen(base); if (bl && base[bl-1]=='/') base[bl-1]=0;
    scan_dir(base, "");

    GeoVaultCtx *ctx = gv_open(vault_path);
    if (!ctx) { fprintf(stderr, "Cannot open %s\n", vault_path); free_paths(); return 1; }
    if (gv_load_frames(ctx) < 0) { gv_close(ctx); free_paths(); return 1; }

    uint32_t ok = 0, total = g_n;
    for (uint32_t i = 0; i < g_n; i++) {
        int idx = gv_find(ctx, g_rel_paths[i]);
        if (idx < 0) { printf("  [MISSING] %s\n", g_rel_paths[i]); continue; }

        /* read original */
        FILE *f = fopen(g_abs_paths[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); uint32_t sz = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *orig = malloc(sz); fread(orig, 1, sz, f); fclose(f);

        /* extract from vault */
        uint8_t *dec = gv_extract(ctx, (uint32_t)idx);
        int match = dec && (ctx->entries[idx].raw_size == sz) &&
                    memcmp(orig, dec, sz) == 0;

        printf("  [%s] %s (%u B)\n", match ? "OK" : "FAIL", g_rel_paths[i], sz);
        if (match) ok++;
        free(orig); free(dec);
    }

    printf("\nVerify: %u/%u PASS\n", ok, total);
    gv_close(ctx); free_paths();
    return ok == total ? 0 : 1;
}

static int cmd_cat(const char *vault_path, const char *rel_path) {
    GeoVaultCtx *ctx = gv_open(vault_path);
    if (!ctx) return 1;
    int idx = gv_find(ctx, rel_path);
    if (idx < 0) { fprintf(stderr, "Not found: %s\n", rel_path); gv_close(ctx); return 1; }
    if (gv_load_frames(ctx) < 0) { gv_close(ctx); return 1; }
    uint8_t *data = gv_extract(ctx, (uint32_t)idx);
    if (data) { fwrite(data, 1, ctx->entries[idx].raw_size, stdout); free(data); }
    gv_close(ctx);
    return 0;
}

/* gvault put <file.geovault> <src_file> [rel_path]
 *   Adds or replaces a file.  Uses in-place if same/smaller, append otherwise. */
static int cmd_put(const char *vault_path, const char *src_file, const char *rel_path) {
    /* read source file */
    FILE *f = fopen(src_file, "rb");
    if (!f) { fprintf(stderr, "Cannot open: %s\n", src_file); return 1; }
    fseek(f, 0, SEEK_END); uint32_t sz = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(sz ? sz : 1);
    if (sz) { size_t r = fread(data, 1, sz, f); (void)r; }
    fclose(f);

    /* default rel_path = basename of src_file */
    if (!rel_path) {
        rel_path = strrchr(src_file, '/');
        rel_path = rel_path ? rel_path + 1 : src_file;
    }

    /* open vault in read+write mode with exclusive lock */
    GeoVaultCtx *ctx = gv_open_rw(vault_path);
    if (!ctx) { free(data); fprintf(stderr, "Cannot open vault: %s\n", vault_path); return 1; }

    /* fp is already r+b from gv_open_rw — no need to reopen */

    /* decide strategy */
    int existing = gv_find(ctx, rel_path);
    int r;
    if (existing >= 0 && sz <= ctx->entries[existing].raw_size) {
        /* try in-place first */
        r = gv_update(ctx, rel_path, data, sz);
        if (r == -3) {
            /* compressed grew — fall back to append */
            fprintf(stderr, "gv_put: in-place compressed too large, falling back to append\n");
            r = gv_add(ctx, rel_path, data, sz);
        }
    } else {
        r = gv_add(ctx, rel_path, data, sz);
    }

    free(data);
    gv_close(ctx);

    if (r < 0) { fprintf(stderr, "put failed: %d\n", r); return 1; }
    printf("put: %s → %s  (%u bytes, %s)\n",
           src_file, rel_path, sz,
           (r == 0 && existing < 0) ? "new" :
           (r == 1)                  ? "replaced (append)" : "replaced (in-place)");
    return 0;
}

/* gvault compact <file.geovault>
 *   Rebuild vault, reclaim orphaned frame space. */
static int cmd_compact(const char *vault_path) {
    GeoVaultCtx *ctx = gv_open(vault_path);
    if (!ctx) { fprintf(stderr, "Cannot open: %s\n", vault_path); return 1; }

    /* get file size before */
    fseek(ctx->fp, 0, SEEK_END);
    long before = ftell(ctx->fp);

    int r = gv_compact(ctx, vault_path);
    gv_close(ctx);

    if (r < 0) { fprintf(stderr, "compact failed: %d\n", r); return 1; }

    /* get file size after */
    FILE *f = fopen(vault_path, "rb");
    long after = 0;
    if (f) { fseek(f, 0, SEEK_END); after = ftell(f); fclose(f); }

    printf("compact: %.1f KB → %.1f KB  (saved %.1f KB)\n",
           before/1024.0, after/1024.0, (before-after)/1024.0);
    return 0;
}

/* ── main ─────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    if (argc < 3) {
        printf("GeoVault — geometric file container\n");
        printf("  gvault pack    <folder>\n");
        printf("  gvault unpack  <file.geovault> [outdir]\n");
        printf("  gvault list    <file.geovault>\n");
        printf("  gvault verify  <folder> <file.geovault>\n");
        printf("  gvault cat     <file.geovault> <path>\n");
        printf("  gvault put     <file.geovault> <src_file> [rel_path]\n");
        printf("  gvault compact <file.geovault>\n");
        return 1;
    }

    const char *cmd = argv[1];
    if      (!strcmp(cmd, "pack")    && argc >= 3) return cmd_pack(argv[2]);
    else if (!strcmp(cmd, "unpack")  && argc >= 3) return cmd_unpack(argv[2], argc>3?argv[3]:NULL);
    else if (!strcmp(cmd, "list")    && argc >= 3) return cmd_list(argv[2]);
    else if (!strcmp(cmd, "verify")  && argc >= 4) return cmd_verify(argv[2], argv[3]);
    else if (!strcmp(cmd, "cat")     && argc >= 4) return cmd_cat(argv[2], argv[3]);
    else if (!strcmp(cmd, "put")     && argc >= 4) return cmd_put(argv[2], argv[3], argc>4?argv[4]:NULL);
    else if (!strcmp(cmd, "compact") && argc >= 3) return cmd_compact(argv[2]);
    else { fprintf(stderr, "Unknown command: %s\n", cmd); return 1; }
}
