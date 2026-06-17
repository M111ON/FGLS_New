#ifndef TWIN_GEAR_BRIDGE_H
#define TWIN_GEAR_BRIDGE_H

#include "gear_lock.h"
#include "pogls_twin_bridge.h"
#include "icosa_twin_bridge.h"

typedef struct {
    TwinBridge      cpu;
    IcosaTwinCtx    gpu;
    GearLock        lock;
    uint32_t        n_ops;
} TwinGearBridge;

static inline void twin_gear_init(TwinGearBridge *tg,
                                   GeoSeed seed,
                                   const uint64_t *bundle,
                                   int enable_gpu)
{
    twin_bridge_init(&tg->cpu, seed, bundle);
    icosa_twin_init(&tg->gpu, seed.gen2, seed.gen3,
                     diamond_baseline(), enable_gpu);
    tg->lock.c144_ref = &tg->cpu.fibo.clk.c144;
    tg->lock.cpu_ops = 0;
    tg->lock.gpu_ops = 0;
    tg->lock.cpu_worlds = 0;
    tg->lock.gpu_worlds = 0;
    tg->n_ops = 0;
}

static inline void twin_gear_write(TwinGearBridge *tg,
                                    uint64_t addr, uint64_t value,
                                    uint8_t slot_hot)
{
    uint32_t tag = gear_tag(&tg->lock);
    icosa_twin_set_c144(&tg->gpu, tag);
    twin_bridge_write(&tg->cpu, addr, value, slot_hot, NULL);
    icosa_twin_write(&tg->gpu, addr, value);
    gear_cpu_tick(&tg->lock);
    gear_gpu_tick(&tg->lock, 1);
    tg->n_ops++;
}

static inline void twin_gear_batch(TwinGearBridge *tg,
                                    const uint64_t *addrs,
                                    const uint64_t *values,
                                    uint32_t n,
                                    uint8_t slot_hot)
{
    if (n == 0) return;
    uint32_t tag = gear_tag(&tg->lock);
    icosa_twin_set_c144(&tg->gpu, tag);
    icosa_twin_batch(&tg->gpu, addrs, values, n);
    for (uint32_t i = 0; i < n; i++)
        twin_bridge_write(&tg->cpu, addrs[i], values[i], slot_hot, NULL);
    gear_cpu_tick(&tg->lock);
    gear_gpu_tick(&tg->lock, n);
    tg->n_ops += n;
}

static inline void twin_gear_flush(TwinGearBridge *tg) {
    twin_bridge_flush(&tg->cpu);
    icosa_twin_flush(&tg->gpu);
}

static inline void twin_gear_stats(const TwinGearBridge *tg, FILE *fp) {
    TwinBridgeStats cs = twin_bridge_stats(&tg->cpu);
    IcosaTwinStats   gs = icosa_twin_stats(&tg->gpu);
    fprintf(fp, "\n=== TWIN GEAR BRIDGE ===\n");
    fprintf(fp, "  c144_tag   = %u\n", gear_tag(&tg->lock));
    fprintf(fp, "  cpu_ops=%u gpu_ops=%u worlds: cpu=%u gpu=%u\n",
            tg->lock.cpu_ops, tg->lock.gpu_ops,
            tg->lock.cpu_worlds, tg->lock.gpu_worlds);
    fprintf(fp, "  --- CPU Dodeca ---\n");
    fprintf(fp, "  ops=%u writes=%u flush=%u density=%.3f qrpn=%u\n",
            cs.total_ops, cs.twin_writes, cs.flush_count, cs.write_density,
            cs.qrpn_fails);
    fprintf(fp, "  --- GPU Icosa ---\n");
    fprintf(fp, "  ops=%llu bnd=%llu gpu=%d flow=%llu\n",
            (unsigned long long)gs.ops_total,
            (unsigned long long)gs.boundaries_total,
            gs.gpu_ready,
            (unsigned long long)gs.route_addr);
}

#endif
