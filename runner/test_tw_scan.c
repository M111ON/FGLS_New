#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "../collection/tw_capture_int.h"

static uint64_t read_file(const char *path, uint8_t **buf)
{
    FILE *f = fopen(path, "rb");
    if (!f) { *buf = NULL; return 0; }
    fseek(f, 0, SEEK_END);
    uint64_t sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    *buf = (uint8_t*)malloc(sz);
    if (!*buf) { fclose(f); return 0; }
    fread(*buf, 1, sz, f);
    fclose(f);
    return sz;
}

static void scan_file(const char *label, const char *path)
{
    uint8_t *buf = NULL;
    uint64_t sz = read_file(path, &buf);
    if (!buf) { printf("[%s] CANNOT OPEN\n", label); return; }

    /* sig: mean of first half, mean of second half (treat as uint8) */
    uint64_t half = sz / 2;
    if (half == 0) half = sz;
    uint64_t sx = 0, sy = 0;
    for (uint64_t i = 0; i < half && i < sz; i++) sx += buf[i];
    uint64_t n2 = sz - half;
    for (uint64_t i = half; i < sz; i++) sy += buf[i];

    double mx = (double)sx / half;
    double my = (double)sy / (n2 > 0 ? n2 : 1);

    int64_t vx = (int64_t)(mx * TW_SCALE);
    int64_t vy = (int64_t)(my * TW_SCALE);

    TWCaptureInt cap;
    uint8_t is_tri;
    tw_capture_int_combined(vx, vy, &cap, &is_tri);

    printf("[%s] %s (%llu bytes)\n", label, path, (unsigned long long)sz);
    printf("  sig=%.4f,%.4f  v=(%lld,%lld)  TW_SCALE=%d\n", mx, my, (long long)vx, (long long)vy, TW_SCALE);
    printf("  zone=%u  slot=%u  resid=(%lld,%lld)  is_tri=%u\n",
        cap.zone, cap.slot, (long long)cap.resid_x, (long long)cap.resid_y, is_tri);
    if (cap.drain)
        printf("  DRAIN -> zone=%u slot=%u resid=(%lld,%lld)\n",
            cap.drain_zone, cap.drain_slot, (long long)cap.drain_resid_x, (long long)cap.drain_resid_y);
    else
        printf("  no drain\n");

    /* Reconstruct */
    int64_t rvx, rvy;
    tw_reconstruct_int_combined(&cap, is_tri, &rvx, &rvy);
    printf("  reconstruct=(%lld,%lld)  match=%s\n\n",
        (long long)rvx, (long long)rvy,
        (rvx == vx && rvy == vy) ? "YES" : "NO");

    free(buf);
}

int main()
{
    scan_file("CPL",      "..\\bake_out\\cosplay.qwen.cpl");
    printf("=== ALL SES PROFILES ===\n");
    scan_file("SES",      "..\\bake_out\\hello.ses");
    scan_file("SES",      "..\\bake_out\\hello_all.ses");
    scan_file("SES",      "..\\bake_out\\hello_ph.ses");
    scan_file("SES",      "..\\bake_out\\hello_prompt.ses");
    scan_file("SES",      "..\\bake_out\\prompt_hello.ses");
    scan_file("SES",      "..\\bake_out\\prompt_poem.ses");
    scan_file("SES",      "..\\bake_out\\prompt_ml.ses");
    scan_file("SES",      "..\\bake_out\\ml.ses");
    scan_file("SES",      "..\\bake_out\\poem_ph.ses");
    scan_file("SES",      "..\\bake_out\\multi_turn.ses");
    return 0;
}
