/*
 * bench_real_files.c — Test tensor_track with real files
 * ═══════════════════════════════════════════════════════════════════════
 *
 * Reads actual files (PDF, text, C source, binary) and runs them through
 * the full pipeline: data → enc → entropy → route → frame range
 *
 * Shows how different file types are classified and routed.
 * ═══════════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "gls_enclosure.h"
#include "tensor_track.h"
#include "geo_frame_seek.h"

/* ── File reader ────────────────────────────────────────────────────── */

static long read_file(const char *path, uint8_t *buf, long max_sz)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz > max_sz) sz = max_sz;
    rewind(f);
    long nread = (long)fread(buf, 1, (size_t)sz, f);
    fclose(f);
    return nread;
}

/* ── Byte distribution histogram ────────────────────────────────────── */

static void print_histogram(const uint8_t *data, long len)
{
    uint32_t hist[256] = {0};
    for (long i = 0; i < len; i++) hist[data[i]]++;

    /* Find top 5 most frequent bytes */
    printf("  Top 5 bytes: ");
    for (int round = 0; round < 5; round++) {
        uint32_t best = 0;
        int best_idx = 0;
        for (int b = 0; b < 256; b++) {
            if (hist[b] > best) { best = hist[b]; best_idx = b; }
        }
        if (best == 0) break;
        printf("0x%02X(%u) ", best_idx, best);
        hist[best_idx] = 0;
    }
    printf("\n");
}

/* ── Analyze one file ───────────────────────────────────────────────── */

static void analyze_file(const char *path, const char *label)
{
    uint8_t buf[65536];
    long sz = read_file(path, buf, sizeof(buf));

    if (sz < 0) {
        printf("  [SKIP] %s — file not found\n", label);
        return;
    }

    printf("  %s (%ld bytes)\n", label, sz);

    /* Entropy score */
    uint8_t ent = tt_entropy_score(buf, (uint32_t)sz);
    uint8_t cls = tt_entropy_class(ent);
    const char *class_names[] = { "structured", "moderate", "high", "random" };

    /* Route */
    TTRoute route = tt_route(0, cls);  /* enc=0 for demo */

    /* Frame range (Fibonacci) */
    FrameRange fr = frame_range(0, cls);

    printf("    entropy: %3u/255 (%s)\n", ent, class_names[cls]);
    printf("    strategy: %s%s\n",
           route.strategy == TT_STRAT_COMPRESS ? "COMPRESS" :
           route.strategy == TT_STRAT_RAW ? "RAW" :
           route.strategy == TT_STRAT_BRIDGE_CMP ? "BRIDGE_CMP" :
           route.strategy == TT_STRAT_BRIDGE_RAW ? "BRIDGE_RAW" : "STANDARD",
           route.is_compressed ? " (compressed)" : "");
    printf("    frame span: %u (tolerance: %u frames each side)\n",
           fr.span, fr.span);

    /* Byte distribution */
    print_histogram(buf, sz);

    /* Chunk analysis: split file into 48B chunks and analyze each */
    uint32_t n_chunks = (uint32_t)((sz + 47) / 48);
    uint32_t counts[4] = {0};  /* structured, moderate, high, random */
    uint32_t strat_counts[5] = {0};

    TTContext ctx;
    tt_init(&ctx);
    TTChunkRecord rec;

    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t off = i * 48;
        uint32_t chunk_len = (uint32_t)(sz - off);
        if (chunk_len > 48) chunk_len = 48;

        tt_ingest(&ctx, buf + off, chunk_len, &rec);
        counts[rec.entropy_class]++;
        TTRoute r = tt_route_record(&rec);
        strat_counts[r.strategy]++;
    }

    printf("    chunks: %u total\n", n_chunks);
    printf("    class distribution: S=%u M=%u H=%u R=%u\n",
           counts[0], counts[1], counts[2], counts[3]);
    printf("    strategy distribution: ST=%u CMP=%u RAW=%u BR_CMP=%u BR_RAW=%u\n",
           strat_counts[0], strat_counts[1], strat_counts[2],
           strat_counts[3], strat_counts[4]);
    printf("\n");
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(void)
{
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Tensor Track — Real File Analysis                     ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* ── PDFs ── */
    printf("── PDFs (binary, high entropy expected) ──\n");
    analyze_file("C:/Users/Administrator.AVENTADOR/Downloads/DGLS_Geometric_Storage.pdf",
                 "DGLS_Geometric_Storage.pdf");
    analyze_file("C:/Users/Administrator.AVENTADOR/Downloads/Geometric_Data_Architecture.pdf",
                 "Geometric_Data_Architecture.pdf");
    analyze_file("C:/Users/Administrator.AVENTADOR/Downloads/POGLS_MASTER_SCHEMATIC.pdf",
                 "POGLS_MASTER_SCHEMATIC.pdf");

    /* ── Text files ── */
    printf("── Text files (structured, low entropy expected) ──\n");
    analyze_file("I:/FGLS_new/AGENTS.md", "AGENTS.md");
    analyze_file("I:/FGLS_new/BENCHMARK_REPORT.md", "BENCHMARK_REPORT.md");
    analyze_file("C:/Users/Administrator.AVENTADOR/Downloads/api.txt", "api.txt");

    /* ── C source code ── */
    printf("── C source code (moderate entropy expected) ──\n");
    analyze_file("core/tensor_track.h", "tensor_track.h");
    analyze_file("core/geo_frame_seek.h", "geo_frame_seek.h");
    analyze_file("pipeline/test_tensor_track.c", "test_tensor_track.c");

    /* ── Summary ── */
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Summary:\n");
    printf("  - PDFs:        high entropy (255/3) → RAW routing\n");
    printf("  - Text:        structured (0-50/0) → COMPRESS routing\n");
    printf("  - C source:    moderate (50-150/1) → STANDARD/COMPRESS\n");
    printf("  - Frame range: Fibonacci scale (0,1,2,3) per class\n");

    return 0;
}
