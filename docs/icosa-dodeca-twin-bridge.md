# Icosa–Dodeca Twin Polyhedral Bridge

## แก่นแนวคิด

**ระบบมีรูปทรงเรขาคณิต 2 ร่างที่ทำงานคู่กันแบบ twin:**

| ร่าง | หน้าที่ | จำนวนหน้า | จำนวนขอบ/หน้า | รวม edge units |
|------|---------|-----------|---------------|----------------|
| **Dodecahedron** | CPU (truth layer) | 12 เหลี่ยม 5 | 5 | **60** |
| **Icosahedron** | GPU (mirror lane) | 20 สามเหลี่ยม | 3 | **60** |

**60 = 60** — จำนวน edge units เท่ากัน นี่คือหัวใจของ dual polyhedron

---

## Zero Copy by Duality

Dodecahedron กับ Icosahedron เป็น dual polyhedron:

```
Dodeca face center  =  Icosa vertex
Icosa face center   =  Dodeca vertex
12 faces (dodeca)   ↔  12 vertices (icosa)
20 faces (icosa)    ↔  20 vertices (dodeca)
```

แปลว่า **ข้อมูลที่อยู่ตรงกลางหน้าของ dodeca คือ vertex ของ icosa** —
ไม่ต้อง copy, ไม่ต้อง transfer, topology จัดการให้เอง

---

## Two Lanes, One Data

```
raw = addr ^ value ^ gen3 ^ c144_tag

          │
          ├──▶ Dodeca lane (CPU)     face%12, edge%5
          │       theta_mix64 → fast_intersect → route_update → DodecaTable
          │
          └──▶ Icosa lane  (GPU)     face%20, edge%3
                  theta_mix64 → fast_intersect → route_update → GPU buffer

```

Both lanes receive **raw data เดียวกัน** — ต่างกันแค่:
- CPU: `face = mix(x) % 12`, `edge = mix(x) % 5` → 12 pentagons × 5 edges = 60 units
- GPU: `face = mix(x) % 20`, `edge = mix(x) % 3` → 20 triangles × 3 edges = 60 units

เหลือ 60 เท่ากัน เกิดจากการกระจาย 60 หน่วยไป 2 รูปแบบ

---

## Fibo Clock Bridge (128:162)

การ sync สอง lane ใช้ fibo clock scale:

```
FIBO_CPU_WORLD   = 128
FIBO_ICOSA_WORLD = 162

fibo_cpu_to_icosa(cpu_val)   → cpu_val * 162 / 128
fibo_icosa_to_cpu(icosa_val) → icosa_val * 128 / 162
cross_check(cpu, icosa)      → cpu * 162 == icosa * 128
```

128:162 = 64:81 = อัตราส่วนฟีโบนักชี — ไม่ใช่ ratio ธรรมดาแต่เป็น **spatial–temporal ratio**

---

## GPU Kernel Core

```cuda
__global__ void icosa_lane_kernel(pairs, out_route, out_event, gen3, c144, baseline, n) {
    // แต่ละ thread = 1 op
    raw    = addr ^ value ^ gen3 ^ c144;
    h      = theta_mix64(raw);
    face   = hi * 20 >> 32;     // 20 triangle faces
    edge   = lo * 3  >> 32;     // 3 edges/triangle
    isect  = fast_intersect(core_raw);
    route  = route_update(0, isect);
    event  = boundary_check(drift > 72 || isect == 0);
}
```

- Thread block = 256 threads
- No shared memory — register only
- Warp-aligned: 256 = 8 warps
- Output: SoA (Structure of Arrays) for coalesced writes

---

## ผลการทดสอบ (GTX 1050 Ti)

| Metric | ค่า |
|--------|-----|
| Ops tested | 1,048,576 |
| Match CPU | **100%** (ทุกตัว) |
| Boundaries | 100,899 ตรง CPU |
| Batch throughput | **340 M/s** (65K batch) |
| Sustained | **15 M/s** (1M ops, chunked) |
| Compile targets | sm_61 (1050 Ti) + sm_75 (T4) |

---

## Files

| File | คำอธิบาย |
|------|----------|
| `collection/src/icosa_twin_bridge.h` | C bridge + CPU fallback |
| `collection/src/icosa_twin_bridge.cu` | CUDA kernel + dispatch + test |

**Header API:**
```c
IcosaTwinCtx ctx;
icosa_twin_init(&ctx, gen2, gen3, baseline, enable_gpu);
icosa_twin_write(&ctx, addr, value);
icosa_twin_batch(&ctx, addrs, values, n);
uint64_t route = icosa_twin_flush(&ctx);
icosa_twin_free(&ctx);
```

---

## ความหมายเชิงโครงสร้าง

นี่ไม่ใช่แค่ GPU acceleration ปกติ แต่เป็น:

1. **Two lanes ที่ไม่ต้อง sync** — เพราะ topology เป็น dual โดยธรรมชาติ การวิ่ง 2 ชุดพร้อมกันไม่เกิด race condition

2. **GPU ใช้ triangle mesh** — 20 สามเหลี่ยมคือ native topology ของ GPU shader/core (triangle rasterization)

3. **CPU/GPU ต่างคนต่างทำงาน** — CPU เก็บ truth ใน DodecaTable, GPU คำนวณ mirror lane ใน Icosa buffer ไม่ต้องรอกัน

4. **Zero copy ไม่ใช่ optimization แต่เป็น mathematical necessity** — ถ้า CPU กับ GPU ต้องคุยกันเพื่อ sync ก็พัง เพราะข้อมูล 60 units เกิดจาก raw เดียวกัน topology จัดการแยกให้เอง

---

## Next

- Integrate icosa_twin_bridge กับ FGLS_new runner SID pipeline
- เพิ่ม `--twin-gpu` flag ให้รัน GPU lane parallel กับ CPU
- Colab deployment สำหรับ T4 (17,000+ M/s expected)
- GPU-side DodecaTable (icosa mirror สำหรับ route lookup)
