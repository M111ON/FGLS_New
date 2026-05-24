/*
 * geo_field_bridge.c — Geofield → JSON Bridge for Python Bond Layer
 * ═══════════════════════════════════════════════════════════════════════
 * Compile:
 *   gcc -O2 -I. -I../../geofield -o build/geo_field_bridge geo_field_bridge.c
 *
 * Usage:
 *   build/geo_field_bridge <file> [gp_level=2]
 *
 * Output: JSON to stdout
 *   {
 *     "file":        "<path>",
 *     "size":        <bytes>,
 *     "gp_level":    <level>,
 *     "chunks":      <total_64B_chunks>,
 *     "blocks":      <frustum_blocks>,
 *     "zone_resets": <count>,
 *     "skeleton":    { "ID":<n>, "FLAT":<n>, "DIFF":<n>, "BREF":<n>, "GEOM":<n>, "RAW":<n> },
 *     "roundtrip":   <"PASS"|"FAIL">,
 *     "topology":    { "content_type": "binary", "best_scale": 64, "routing_hint": "geometric" }
 *   }
 * ═══════════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#ifdef _WIN32
  #include <fcntl.h>
  #include <io.h>
#endif

/* ── Geofield headers ────────────────────────────────────────── */
#include "geo_field_core.h"

/* ── topology_fp: FNV-64 hash of first 16 bytes of file ──────── */
#define FNV_OFFSET 0xCBF29CE484222325ULL
#define FNV_PRIME  0x00000100000001B3ULL

static uint64_t fnv64(const uint8_t *data, size_t len) {
    uint64_t h = FNV_OFFSET;
    for (size_t i = 0; i < len && i < 16; i++) {
        h ^= (uint64_t)data[i];
        h *= FNV_PRIME;
    }
    return h;
}

/* ══════════════════════════════════════════════════════════════
 *  print_json: encode geofield stats as JSON
 * ══════════════════════════════════════════════════════════════ */
/* ── escape backslashes for JSON ────────────────────────────── */
static void print_json_escaped(const char *s) {
    for (; *s; s++) {
        if (*s == '\\') putchar('/');
        else if (*s == '"') putchar('\\'), putchar('"');
        else putchar(*s);
    }
}

static void print_json(const char *fpath,
                       const uint8_t *data, size_t sz,
                       int gp_level,
                       const GeoFieldEncodeStats *enc,
                       const GeoFieldDecodeStats *dec,
                       int roundtrip_ok) {
    (void)dec;  /* unused — reserved for future decode stats */
    uint64_t fp = fnv64(data, sz);

    printf("{\n");
    printf("  \"file\":        \"");
    print_json_escaped(fpath);
    printf("\",\n");
    printf("  \"size\":        %zu,\n", sz);
    printf("  \"chunks\":      %" PRIu64 ",\n", enc->total_chunks);
    printf("  \"blocks\":      %u,\n", (unsigned)enc->total_blocks);
    printf("  \"zone_resets\": %" PRIu64 ",\n", enc->zone_resets);
    printf("  \"skeleton\": {\n");
    printf("    \"ID\":   %u,\n", enc->skel_hits[0]);
    printf("    \"FLAT\": %u,\n", enc->skel_hits[1]);
    printf("    \"DIFF\": %u,\n", enc->skel_hits[2]);
    printf("    \"BREF\": %u,\n", enc->skel_hits[3]);
    printf("    \"GEOM\": %u,\n", enc->skel_hits[4]);
    printf("    \"RAW\":  %u\n",  enc->skel_hits[5]);
    printf("  },\n");
    printf("  \"roundtrip\":   \"%s\",\n", roundtrip_ok ? "PASS" : "FAIL");
    printf("  \"topology_fp\": \"%016llx\",\n", (unsigned long long)fp);
    printf("  \"topology\": {\n");
    printf("    \"content_type\": \"binary\",\n");
    printf("    \"best_scale\":   %d,\n", GF_CHUNK_SZ);
    printf("    \"chunk_size\":   %u,\n", GF_CHUNK_SZ);
    printf("    \"routing_hint\": \"geometric\",\n");
    printf("    \"gp_level\":     %d\n", gp_level);
    printf("  }\n");
    printf("}\n");
}

/* ══════════════════════════════════════════════════════════════
 *  MAIN
 * ══════════════════════════════════════════════════════════════ */
int main(int argc, char **argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    if (argc < 2) {
        fprintf(stderr, "Usage: geo_field_bridge <file> [gp_level=2]\n");
        return 1;
    }

    const char *fpath = argv[1];
    int gp_level = (argc >= 3) ? atoi(argv[2]) : 2;
    if (gp_level < 1) gp_level = 1;
    if (gp_level > 8) gp_level = 8;

    /* ── read input file ───────────────────────────────────── */
    FILE *fp = fopen(fpath, "rb");
    if (!fp) { fprintf(stderr, "ERROR: cannot open %s\n", fpath); return 1; }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    rewind(fp);
    if (fsize <= 0) { fprintf(stderr, "ERROR: empty file\n"); fclose(fp); return 1; }

    uint8_t *data = (uint8_t *)malloc((size_t)fsize);
    if (!data) { fprintf(stderr, "ERROR: malloc failed\n"); fclose(fp); return 1; }
    size_t nread = fread(data, 1, (size_t)fsize, fp);
    fclose(fp);
    if (nread != (size_t)fsize) { fprintf(stderr, "ERROR: read failed\n"); free(data); return 1; }

    /* ── geofield roundtrip ─────────────────────────────────── */
    GeoFieldEncodeStats enc_stats;
    GeoFieldDecodeStats dec_stats;
    memset(&enc_stats, 0, sizeof(enc_stats));
    memset(&dec_stats, 0, sizeof(dec_stats));

    int ret = geo_field_roundtrip(data, (size_t)fsize, (uint8_t)gp_level,
                                   &enc_stats, &dec_stats);
    int roundtrip_ok = (ret == 0);

    /* ── output JSON ────────────────────────────────────────── */
    print_json(fpath, data, (size_t)fsize, gp_level, &enc_stats, &dec_stats, roundtrip_ok);

    free(data);
    return roundtrip_ok ? 0 : 1;
}
