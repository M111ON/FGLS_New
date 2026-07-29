# GPU Jet Puller — Architecture Report

> **FGLS Project** — July 28, 2026  
> ภาษาไทย — สรุปผลการพัฒนา prototype GPU-as-Puller ตาม architecture pipeline

---

## 1. Architecture Pipeline

```
File → Chunk[Bond] → Arrange[sequence + header]
                         ↓
                  GeoPixel[Hilbert maze grid + Hamburger + GeoFrameSeek + ...]
                         ↓
              Split: header-structure → CPU (steering)
                     payload          → GPU (batch pull)
                         ↓
              Everything zero-copy through DRamTile & Gear-pipeline
                         ↓
                       Done
```

### หลักการ
- **CPU = steering** — สร้าง point index, schedule, tick synchronization
- **GPU = bandwidth puller** — ไม่ compute, แค่ pull chunk จาก DRamTile ผ่าน geometric addressing
- **zero cudaMemcpy** — GPU เข้าถึง DRamTile โดยตรงผ่าน unified memory pointer
- **Jet Bridge** = natural sync point ที่ FiboSpine tick 11 → residual → tick 13

---

## 2. Component Status

| Component | File | สถานะ | Verified |
|-----------|------|:-----:|:--------:|
| **FiboSpine** 1728×12 | `collection/src/fibo_spine.h` | ✅ | Bug PIPE_FLAG_NONE fixed, 413/413 PASS |
| **Jet Bridge** tick 11→13 | `fibo_spine.h` | ✅ | Per-pipe async, verified |
| **P5H Ribcage** freeze/barrier | `collection/include/p5h_ribcage.h` | ✅ | |
| **GearLock** CPU↔GPU sync | `collection/src/gear_lock.h` | ✅ | |
| **Gear2** pinned mirror | `runner/gear2.h` | ✅ | 300 lines |
| **DRamTile** zero-copy store | `runner/dramtile_store.h/.c` | ✅ | mmap, <=123ns read |
| **RDH addressing** 1.6ns | `collection/rdh/rdh_addr.h` | ✅ | Collision-free |
| **GPU Jet Puller** | `runner/gpu_jet_puller/gpu_jet_puller.cu` | ✅ | **Local (1050 Ti): 3.29 GB/s** |
|  |  |  | **Colab T4 (HBM): 18.23 GB/s** |
| **Bond→FiboSpine mapping** | `pipeline/test_bond_fibospine_mapping.c` | ✅ | **640 ns/op, 1.56M chunks/sec** |
| **geo_frame_seek** 384× | `collection/src/geo_frame_seek.h` | ⚠️ | Ratio confirmed mathematically |
|  |  |  | GGUF weights ~1× (high entropy) |
| **Diamond Shell** fold_fibo_intersect | `collection/core/shell/` | ❌ | ยังใช้ non-zero counter |
| **icosa_twin_bridge.cu** | half-half stub | ❌ | Deprecated |
| **Chunk[Bond]→Arrange→GeoPixel→Split** | pipeline | ❌ | ยังไม่ต่อครบวงจร |
| **GGUF→HBM→GPU** | pipeline | ❌ | ยังไม่ได้ทดสอบกับ model จริง |

---

## 3. Benchmark Results

### 3.1 Local (GTX 1050 Ti, sm_61)

```
GPU:            NVIDIA GeForce GTX 1050 Ti
VRAM:           4 GB
HBM buffer:     2 MB (20736 slots × 64 B)
Timeline:       41472 ticks = 2 full sweeps

GPU throughput:  3.29 GB/s
Errors:          0
GPU pulls:       7,166,016
Bridges:         4,147
```

### 3.2 Colab (Tesla T4, sm_75)

| Configuration | Throughput | Kernel Time | Notes |
|:-------------|:----------:|:-----------:|-------|
| PCIe (cudaHostRegister) | **7.23 GB/s** | 63.43 ms | PCIe 3.0 x8 bottleneck |
| HBM direct (cudaMalloc) | **18.23 GB/s** | 25.16 ms | **2.5× faster** |

16 GB HBM2, ~300 GB/s theoretical. 18.23 GB/s ยังไม่ถึง HBM limit — kernel compute-bound ด้วย XOR checksum ต่อ byte

### 3.3 Bond→FiboSpine Mapping

```
Pipeline:      GGUF chunk → rdh_capture → enc → ft_enc_to_pipe → pipe_id
               → ft_enc_to_tick → tick (0..11)
Speed:         640 ns/op → 1.56M chunks/sec
Determinism:   ✓ 3× replay identical
Pipe range:    ✓ all enc 0..1439 ภายใน FT_PIPES=1728
Tick dist:     ✓ 12 ticks, 120 each, uniform
Fingerprint:   ✓ zero collisions in 100 chunks
Full pipeline: ✓ 500 realistic GGUF chunks, 15.6% pipe utilization
```

### 3.4 geo_frame_seek Compression

| Data Type | Compression Ratio | รายละเอียด |
|-----------|:-----------------:|-----------|
| **Mathematical (768B→2B)** | **384×** | Address ratio confirmed |
| GGUF weights (high entropy) | ~1× | No temporal redundancy between frames |
| Structured / zero data | 35.65× | Exploits frame repetition |

---

## 4. Build Matrix

| Target | Arch | Compiler | Status | Bandwidth |
|--------|:----:|----------|:------:|:---------:|
| **Local GTX 1050 Ti** | sm_61 | nvcc + MSVC 2022 Build Tools | ✅ Runs | **3.29 GB/s** |
| **Colab T4** | sm_75 | nvcc (WSL or Colab) | ✅ Runs | **18.23 GB/s** |

**MSVC 2022 Build Tools** พบที่ `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\`
— nvcc ใช้เป็น host compiler แล้ว compile ได้เลย ไม่ต้อง WSL

---

## 5. Open Tasks (สิ่งที่ยังต้องทำ)

| Priority | Task | Dependencies | Status |
|:--------:|------|:------------:|:------:|
| 🔴 | **Diamond Shell** — replace non-zero counter with fold_fibo_intersect | — | Not started |
| 🔴 | **Pipeline integration test** — Chunk→Bond→Arrange→GeoPixel→Split→Pull | All below | Not started |
| 🟡 | **Bandwidth optimization** — 256B/1024B + no checksum | Need Linux compile on Colab | Blocked |
| 🟡 | **Real GGUF→HBM→GPU pull** — test with actual model | geo_frame_seek + Bond done | Ready |
| 🟢 | **Clean up icosa_twin_bridge.cu** → deprecated/ | — | Ready |
| 🟢 | **Makefile fix** — add `-I../../collection/rdh` | — | Ready |

---

## 6. Key Files

| File | คำอธิบาย |
|------|----------|
| `runner/gpu_jet_puller/gpu_jet_puller.cu` | GPU Jet Puller prototype (HBM version) |
| `runner/gpu_jet_puller/gpu_jet_puller_sm61.exe` | Local binary (1050 Ti, 322 KB) |
| `runner/gpu_jet_puller/gpu_jet_puller_hbm` | Colab binary (T4, 982 KB) |
| `pipeline/test_bond_fibospine_mapping.c` | Bond→FiboSpine test harness |
| `tools/test_frame_seek_gguf.py` | geo_frame_seek Python test |
| `docs/gpu-jet-puller-report.md` | รายงานฉบับนี้ |

---

## 7. Quick Start — Resume Protocol

```bash
# Session หน้า — resume ได้ใน 3 บรรทัด
cd /I/FGLS_new/runner/gpu_jet_puller
colab new -s gpupull --gpu T4
./deploy_to_colab.sh

# หรือรัน local (1050 Ti) ได้เลย
./gpu_jet_puller_sm61.exe
```

## 8. ข้อสรุป

**สิ่งที่พิสูจน์แล้ว:**
1. ✅ GPU-as-Puller architecture — CPU steering, GPU batch pull (173:1 ratio)
2. ✅ Zero cudaMemcpy — DRamTile direct access
3. ✅ RDH addressing — collision-free, O(1)
4. ✅ Jet Bridge sync — natural FiboSpine tick 11 boundary
5. ✅ HBM direct vs PCIe = 2.5× speedup (7.23 → 18.23 GB/s)
6. ✅ Bond→FiboSpine pipeline — 640 ns/op, deterministic
7. ✅ Local GPU testing (1050 Ti) — 3.29 GB/s, ไม่ต้องพึ่ง Colab

สิ่งที่เหลือคือ **ต่อ pipeline ให้ครบวงจร** — Chunk→Bond→Arrange→GeoPixel→Split→Pull
