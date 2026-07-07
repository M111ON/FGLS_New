# GPU Config Experiment — Vulkan vs CUDA Bridge

**Date:** July 6, 2026
**Model:** Qwen3-4B-Q5_0.gguf (2.63 GB, Q5_0 legacy quant)
**GPU:** 2× NVIDIA GeForce GTX 1050 Ti (SM6.1, 4096 MB VRAM each, driver 581.57 CUDA 13.0)
**Runner:** `llama_pogls_runner_sid_v2.exe` (b9528)

---

## 1. Problem

ต้องการรัน Qwen3-4B ด้วย `--ctx 16384` บน GPU — แต่ Vulkan ชน `ErrorDeviceLost` ทันทีที่ ctx > 8192

```
--ngl 36, ctx 8192  ✅  PASS
--ngl 36, ctx 10240 ❌  DeviceLost
--ngl 36, ctx 12288 ❌  DeviceLost
--ngl 36, ctx 16384 ❌  DeviceLost
```

## 2. Root Cause

Vulkan `sched_reserve` allocate compute buffer บน GPU VRAM ตาม worst-case graph size:
- ctx 8192: model 2.62 GB + KV 1.15 GB + compute ~100 MB → พอดีใน 4 GB
- ctx 10240+: model 2.62 GB + KV 1.44 GB + compute 361 MB → ชน 4 GB ceiling

VRAM per GPU = 3617 MB free (หลัง OS reservation)

## 3. Attempted Solutions

### 3.1 Reduce GPU layers (`--ngl N`)
| Config | ctx | Gen/tok | VRAM/GPU | Status |
|---|---|---|---|---|
| `--ngl 36` | 8192 | **91ms** 🏆 | 1.8 GB / 2.0 GB | ✅ |
| `--ngl 24` | 12288 | 627ms | 1.4 GB / 1.6 GB | ✅ ผ่านแต่ช้า |
| `--ngl 18` | 12288 | 980ms | 1.1 GB / 1.3 GB | ✅ ช้ามาก |
| `--ngl 36` | 10240+ | — | — | ❌ DeviceLost |

Layer ที่เหลือลง CPU → cross-device sync overhead ทำให้ speed ตกหนัก

### 3.2 Q5_0 (non-K-quant) on Vulkan
| Model | Quant | Vulkan | Gen/tok |
|---|---|---|---|
| Qwen3-4B | Q5_0 | ✅ 36/37 layers | 91ms |
| Qwen3-4B | Q4_K_M | ❌ DeviceLost | — |

Q5_0 ใช้ Vulkan ได้เพราะเป็น legacy quant (non-K) — K-quants (Q4_K_M, Q6_K) ไม่มี Vulkan kernel ใน b9733

### 3.3 CPU-only with larger ctx
| Config | ctx | Gen/tok | Notes |
|---|---|---|---|
| `--ngl 0` | 16384 | 1814ms | CPU compute, ใช้ได้แต่ช้า |

### 3.4 Full stack: CUDA bridge + DRamTile (🏆 winner)
Disable Vulkan DLL, use CUDA icosa bridge for GPU memory ops + CPU for compute:

```
--twin-gpu --gear-lock --dramtile --ctx 16384
```

| Component | Status | Notes |
|---|---|---|
| DRamTile (mmap) | ✅ 252/253 tensors | 2.8 GB anonymous mmap |
| icosa bridge (CUDA) | ✅ GTX 1050 Ti SM6.1 | 4096 MB VRAM, capacity 65536 |
| GearShift | ✅ 253 tensors registered | routing table from DRamTile |
| KV cache | ✅ 2304 MiB (16384 ctx) | all in system RAM |
| Inference | ✅ 1841ms/tok | CPU-bound (Q5_0) |

**No DeviceLost, no error.**

## 4. Key Files Modified

| File | Action | Reason |
|---|---|---|
| `runner/ggml-vulkan.dll` → `.bak` | Rename | ปิด Vulkan backend ป้องกัน compute buffer alloc บน VRAM |
| `runner/icosa_bridge.dll` (จาก `collection/src/icosa_bridge.sm61.dll`) | Copy | CUDA SM6.1 bridge สำหรับ GTX 1050 Ti |

## 5. Strategy Comparison

| Approach | Speed | Max ctx | GPU ใช้ทำอะไร |
|---|---|---|---|
| **Vulkan `--ngl 36`** | **91ms/tok** 🏆 | 8192 | Model compute |
| **CUDA bridge + CPU** | 1841ms/tok | **16384** 🏆 | SID twin swap + GearShift |
| Hybrid `--ngl 24` | 627ms/tok | 12288 | Mixed compute |

## 6. Note on CUDA Bridge

`icosa_bridge.dll` (compiled for SM 6.1 = Pascal) ให้ฟังก์ชัน:
- `icosa_gpu_memcpy_h2d()` — host→device copy
- `icosa_gpu_batch_memcpy_h2d()` — batch upload
- `icosa_gpu_dispatch()` — twin GPU routing
- `icosa_gpu_pin_host()` / `unpin_host()` — pinned memory

ไม่เกี่ยวข้องกับ model compute — ใช้สำหรับ SID twin buffer swap + GearShift streaming ระหว่าง CPU↔GPU เท่านั้น

## 7. Conclusion

- **Vulkan + --ngl = เร็วที่สุด (91ms/tok) แต่ ctx max ที่ 8192** — VRAM 4 GB เป็น bottleneck
- **CUDA bridge + DRamTile = ช้าที่สุด (1841ms/tok) แต่ ctx 16384+ ได้** — เหมาะกับ workload ที่ต้องการ context ไกล
- GTX 1050 Ti 768 CUDA core Pascal = computational bottleneck อยู่ดี ไม่ว่า backend ไหน
- การ rename DLL (`ggml-vulkan.dll` → `.bak`) เป็น technique ที่ใช้ disable Vulkan compute buffer โดยไม่ต้อง recompile
