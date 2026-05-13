/*
 * test_geopixel_session_feed.c
 * End-to-end: file scan session + GPX4 META emit
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GEOPIXEL_SESSION_FEED_IMPL
#include "geopixel_session_feed.h"

static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); _pass++; } \
    else      { printf("  FAIL  %s\n", msg); _fail++; } \
} while (0)

static void t01_scan_metrics(void)
{
    GeopixelSessionMetrics m;
    int r = geopixel_session_scan_file("invention.txt", &m);
    printf("\nT01: scan file metrics\n");
    CHECK(r == 0, "scan_file returns 0");
    CHECK(m.file_bytes > 0u, "file_bytes > 0");
    CHECK(m.total_chunks > 0u, "total_chunks > 0");
    CHECK(m.emitted_chunks == m.total_chunks, "emitted == total");
}

static void t02_emit_meta_gpx4(void)
{
    GeopixelSessionMetrics m;
    FILE *f;
    uint8_t hdr[16];
    int r = geopixel_session_scan_file("invention.txt", &m);
    printf("\nT02: emit meta gpx4\n");
    CHECK(r == 0, "scan_file before emit");

    r = geopixel_session_write_meta_gpx4("build/integration/session_meta.gpx4",
                                         &m, 1u, 1u);
    CHECK(r == 0, "write_meta_gpx4 returns 0");

    f = fopen("build/integration/session_meta.gpx4", "rb");
    CHECK(f != NULL, "output file exists");
    if (!f) return;

    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        CHECK(0, "read header bytes");
        return;
    }
    fclose(f);

    CHECK(hdr[0] == 'G' && hdr[1] == 'P' && hdr[2] == 'X' && hdr[3] == '4',
          "header magic is GPX4");
    CHECK(hdr[6] == 0 && hdr[7] == 1, "n_layers = 1");
}

static void t03_timeline_sequence_layers(void)
{
    const char *paths[3] = {
        "build/integration/frame_000.bin",
        "build/integration/frame_001.bin",
        "build/integration/frame_002.bin"
    };
    FILE *f;
    uint8_t hdr[16];
    int r;
    int i;
    printf("\nT03: sequence -> timeline layers\n");

    for (i = 0; i < 3; i++) {
        f = fopen(paths[i], "wb");
        CHECK(f != NULL, "create frame file");
        if (!f) return;
        fputc(0x10 + i, f);
        fputc(0x20 + i, f);
        fputc(0x30 + i, f);
        fclose(f);
    }

    r = geopixel_timeline_write_sequence_gpx4_ex(
        "build/integration/timeline_sequence.gpx4",
        paths,
        3u,
        64u, 64u,
        24u, 1u,
        1u,
        GPX_TIMELINE_META
    );
    CHECK(r == 0, "timeline writer returns 0");

    f = fopen("build/integration/timeline_sequence.gpx4", "rb");
    CHECK(f != NULL, "timeline output exists");
    if (!f) return;
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        CHECK(0, "read timeline header");
        return;
    }
    fclose(f);
    CHECK(hdr[0] == 'G' && hdr[1] == 'P' && hdr[2] == 'X' && hdr[3] == '4',
          "timeline header magic is GPX4");
    CHECK(hdr[6] == 0 && hdr[7] == 4, "n_layers = 4 (AHDR + 3 frames)");
}

static void t04_timeline_anim_codec_auto(void)
{
    const char *paths[2] = {
        "build/integration/rgb_frame_000.raw",
        "build/integration/rgb_frame_001.raw"
    };
    FILE *f;
    int r;
    int i, p;
    uint8_t hdr[16];
    printf("\nT04: sequence AUTO -> anim codec path\n");

    for (i = 0; i < 2; i++) {
        f = fopen(paths[i], "wb");
        CHECK(f != NULL, "create rgb frame file");
        if (!f) return;
        /* 2x2 RGB = 12 bytes */
        for (p = 0; p < 12; p++) fputc((i * 40 + p) & 0xFF, f);
        fclose(f);
    }

    r = geopixel_timeline_write_sequence_gpx4_ex(
        "build/integration/timeline_anim_auto.gpx4",
        paths,
        2u,
        2u, 2u,
        12u, 1u,
        1u,
        GPX_TIMELINE_AUTO
    );
    CHECK(r == 0, "AUTO writer returns 0");

    f = fopen("build/integration/timeline_anim_auto.gpx4", "rb");
    CHECK(f != NULL, "AUTO output exists");
    if (!f) return;
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        CHECK(0, "read AUTO header");
        return;
    }
    fclose(f);
    CHECK(hdr[0] == 'G' && hdr[1] == 'P' && hdr[2] == 'X' && hdr[3] == '4',
          "AUTO header magic is GPX4");
    CHECK(hdr[6] == 0 && hdr[7] == 3, "AUTO fallback layers = 3 (AHDR + 2 frames)");
}

int main(void)
{
    printf("=== Geopixel Session Feed Integration ===\n");
    t01_scan_metrics();
    t02_emit_meta_gpx4();
    t03_timeline_sequence_layers();
    t04_timeline_anim_codec_auto();
    printf("\n=== RESULT: %d PASS  %d FAIL ===\n", _pass, _fail);
    return _fail ? 1 : 0;
}
