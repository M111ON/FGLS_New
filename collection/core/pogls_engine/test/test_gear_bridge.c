/*
 * test_gear_bridge.c — Twin Gear Bridge: CPU Dodeca + GPU Icosa with gear lock
 *
 * Compile (from repo root):
 *   gcc -O2 -std=c11 -Icollection/core/pogls_engine -Icollection/core/pogls_engine/core -Icollection/core/pogls_engine/twin_core -Icollection/src -I. -o test_gear_bridge.exe collection/core/pogls_engine/test/test_gear_bridge.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "gear_lock.h"
#include "icosa_twin_bridge.h"
#include "pogls_twin_bridge.h"
#include "twin_gear_bridge.h"

/* ── helpers ───────────────────────────────────────────────────── */
static int g_pass = 0, g_fail = 0;
#define CHECK(label, cond) do { \
    if (cond) { printf("  OK   %s\n", label); g_pass++; } \
    else      { printf("  FAIL %s\n", label); g_fail++; } \
} while(0)
#define SECTION(n) printf("\n[%s]\n", n)

static uint64_t _a(uint32_t i)  { return (uint64_t)i * 0x9E3779B185EBCA87ULL ^ 0x0001000100010001ULL; }
static uint64_t _v(uint32_t i)  { return (uint64_t)i * 0x6C62272E07BB0142ULL ^ 0xDEADBEEFCAFEBABEULL; }

static uint64_t g_bundle[GEO_BUNDLE_WORDS];
static void make_bundle(void) {
    for (uint32_t i = 0; i < GEO_BUNDLE_WORDS; i++)
        g_bundle[i] = 0xAAAAAAAAAAAAAAAAULL ^ ((uint64_t)(i+1) << 32);
}

static GeoSeed make_seed(void) {
    GeoSeed s;
    s.gen2 = 0x9E3779B97F4A7C15ULL;
    s.gen3 = 0x6C62272E07BB0142ULL;
    return s;
}

/* ── T1: Gear lock basic ops ───────────────────────────────────── */
static void t1_gear_basic(void)
{
    SECTION("T1: gear lock basic ops");
    uint8_t c144 = 144;
    GearLock gl = { .c144_ref = &c144 };

    CHECK("T1a: initial tag = 144", gear_tag(&gl) == 144);
    CHECK("T1b: cpu_ops = 0", gl.cpu_ops == 0);
    CHECK("T1c: gpu_ops = 0", gl.gpu_ops == 0);

    /* 128 CPU ops = 1 world */
    for (int i = 0; i < 128; i++) gear_cpu_tick(&gl);
    CHECK("T1d: cpu_worlds = 1 after 128", gl.cpu_worlds == 1);
    CHECK("T1e: cpu_ops = 128", gl.cpu_ops == 128);

    /* 162 GPU ops = 1 world */
    for (int i = 0; i < 162; i++) gear_gpu_tick(&gl, 1);
    CHECK("T1f: gpu_worlds = 1 after 162", gl.gpu_worlds == 1);
    CHECK("T1g: gpu_ops = 162", gl.gpu_ops == 162);
}

/* ── T2: Gear lock 20736 realignment ───────────────────────────── */
static void t2_gear_geo_full(void)
{
    SECTION("T2: gear lock 20736 = GEO_FULL realignment");
    uint8_t c144 = 144;
    GearLock gl = { .c144_ref = &c144 };

    for (uint32_t i = 0; i < GEAR_GEO_FULL; i++) {
        gear_cpu_tick(&gl);
        gear_gpu_tick(&gl, 1);
    }

    CHECK("T2a: cpu_ops = 20736",   gl.cpu_ops == GEAR_GEO_FULL);
    CHECK("T2b: gpu_ops = 20736",   gl.gpu_ops == GEAR_GEO_FULL);
    CHECK("T2c: cpu_worlds = 162",  gl.cpu_worlds == 162);
    CHECK("T2d: gpu_worlds = 128",  gl.gpu_worlds == 128);
    printf("  CPU: %u ops in %u worlds (%u/128)\n",
           gl.cpu_ops, gl.cpu_worlds, gl.cpu_ops);
    printf("  GPU: %u ops in %u worlds (%u/162)\n",
           gl.gpu_ops, gl.gpu_worlds, gl.gpu_ops);
    printf("  128*162 = 162*128 = %u = GEO_FULL ✓\n", GEAR_GEO_FULL);
}

/* ── T3: TwinBridge with Icosa via gear lock (CPU fallback) ────── */
static void t3_gear_twin_write(void)
{
    SECTION("T3: TwinBridge + Icosa with gear lock");
    make_bundle();
    GeoSeed seed = make_seed();

    TwinGearBridge tg;
    twin_gear_init(&tg, seed, g_bundle, 0);

    CHECK("T3a: cpu initialized", tg.cpu.total_ops == 0);
    CHECK("T3b: gpu initialized", tg.gpu.ops_total == 0);
    CHECK("T3c: gear ref set",    tg.lock.c144_ref != NULL);
    CHECK("T3d: gear tag = 144",  gear_tag(&tg.lock) == 144);

    /* Write 144 ops (one c144 cycle) */
    for (int i = 0; i < 144; i++) {
        uint32_t tag = gear_tag(&tg.lock);
        icosa_twin_set_c144(&tg.gpu, tag);
        twin_bridge_write(&tg.cpu, _a(i), _v(i), 0, NULL);
        icosa_twin_write(&tg.gpu, _a(i), _v(i));
        gear_cpu_tick(&tg.lock);
        gear_gpu_tick(&tg.lock, 1);
    }

    CHECK("T3e: cpu got 144 ops",  tg.cpu.total_ops == 144);
    CHECK("T3f: gpu got 144 ops",  tg.gpu.ops_total == 144);
    CHECK("T3g: cpu_worlds = 1",   tg.lock.cpu_worlds == 1);
    printf("  cpu.flush_count=%u  gpu.boundaries=%llu\n",
           tg.cpu.flush_count, (unsigned long long)tg.gpu.boundaries_total);

    /* Verify both lanes used same c144_tag sequence */
    printf("  gear_tag remained %u (c144 not advancing — see note)\n",
           gear_tag(&tg.lock));
    printf("  NOTE: c144 advances via TwinBridge's fibo_clock_tick\n");
}

/* ── T4: Gear lock c144 tracking ───────────────────────────────── */
static void t4_gear_c144_advance(void)
{
    SECTION("T4: c144 advances via fibo clock in TwinBridge");
    make_bundle();

    TwinGearBridge tg;
    twin_gear_init(&tg, make_seed(), g_bundle, 0);

    uint32_t tags_seen[288];
    int flush_ops[144];
    int n_flush = 0;

    for (int i = 0; i < 288; i++) {
        tags_seen[i] = gear_tag(&tg.lock);
        icosa_twin_set_c144(&tg.gpu, tags_seen[i]);
        FiboEvent ev = twin_bridge_write(&tg.cpu, _a(i), _v(i), 0, NULL);
        icosa_twin_write(&tg.gpu, _a(i), _v(i));
        gear_cpu_tick(&tg.lock);
        gear_gpu_tick(&tg.lock, 1);
        if (ev & FIBO_EV_FLUSH) {
            if (n_flush < 144) flush_ops[n_flush++] = i;
        }
    }

    /* c144 starts at 144, decrements each op: 144, 143, ..., 1, 144, ... */
    CHECK("T4a: first tag = 144", tags_seen[0] == 144);
    CHECK("T4b: second tag = 143", tags_seen[1] == 143);
    CHECK("T4c: tag[143] = 1", tags_seen[143] == 1);
    CHECK("T4d: tag[144] = 144 (wrap)", tags_seen[144] == 144);
    CHECK("T4e: tag[287] = 1 (2nd cycle)", tags_seen[287] == 1);
    CHECK("T4e: n_flush >= 1", n_flush >= 1);
    printf("  tags: [0]=%u [1]=%u [143]=%u [144]=%u [287]=%u\n",
           tags_seen[0], tags_seen[1], tags_seen[143],
           tags_seen[144], tags_seen[287]);
    printf("  flushes at ops: ");
    for (int i = 0; i < n_flush && i < 5; i++)
        printf("%d ", flush_ops[i]);
    printf("\n");
}

/* ── main ───────────────────────────────────────────────────────── */
int main(void)
{
    printf("=== TWIN GEAR BRIDGE TEST ===\n");

    t1_gear_basic();
    t2_gear_geo_full();
    t3_gear_twin_write();
    t4_gear_c144_advance();

    printf("\n=== RESULTS: %d/%d passed ===\n", g_pass, g_pass + g_fail);
    return g_fail > 0 ? 1 : 0;
}
