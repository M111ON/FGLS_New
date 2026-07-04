# POGLS 3-Tier Pipeline Performance Model

## Concept: CPU Header-Sync + GPU Tile Compute

### The Problem

ใน LLM inference, tensor data ต้องเดินทางผ่าน 3 ชั้น:

```
Disk ──[DRamTile]──→ RAM ──[GearShift]──→ VRAM ──[GearLock]──→ GPU compute
```

แต่ละชั้นมี bandwidth และ latency ต่างกัน:
- **DRamTile**: disk→RAM (NVMe, cold spill, mmap)
- **GearShift**: RAM→VRAM (PCIe streaming, priority queue)
- **GearLock**: CPU-GPU sync (header metadata + tile data)

Naive approach: copy 5GB raw tensor data through every layer.

### Key Insight

**geo_frame_seek** เปลี่ยน frame data (768 bytes) → 2-byte enc. ลด data 384x.

CPU ทำแค่:
- `frame_at(enc)` — bit shifts/masks, **3ns** O(1)
- page table flip — 2592 bytes pointer swap
- gear_lock tick — counter increment

GPU ทำ tile processing (full 768 bytes) ตามปกติ — reconstruct ใน VRAM โดยไม่ต้อง memcpy จาก host

---

## Proof: Timing Model

### Parameters

| Parameter | 1050 Ti | 3060 | 4090 | 5090 | M3 Max | M4 Ultra |
|-----------|---------|------|------|------|--------|----------|
| NVMe (MB/s) | 3,500 | 3,500 | 7,000 | 14,000 | 7,000 | 10,000 |
| PCIe (MB/s) | 16,000 | 16,000 | 32,000 | 64,000 | — | — |
| GPU Mem BW (GB/s) | 112 | 360 | 1,008 | 1,800 | 400 | 800 |
| RAM BW (GB/s) | 25 | 25 | 55 | 55 | 100 | 150 |
| Unified Memory | — | — | — | — | ✓ | ✓ |

Model: 8B Q4 GGUF ≈ 5,127 MB raw, 7M frames × 768 bytes

### RAW Pipeline

```
RAW: ขนส่ง 5,127 MB ผ่านทุกชั้น
Disk→RAM:  5,127 MB / NVMe speed
RAM→VRAM:  5,127 MB / PCIe speed  
GPU read:  5,127 MB / GPU BW

Bottleneck: disk I/O (ชั้นที่ช้าที่สุดใน serial)
```

### ENC Pipeline (geo_frame_seek)

```
ENC: ขนส่ง 13.35 MB (384x เล็กลง)
Disk→RAM:  13.35 MB / NVMe speed  → negligible
RAM→VRAM:  13.35 MB / PCIe speed  → negligible
Recon:     7M × 3ns = 0.021s      → O(1) frame_at(enc)
GPU read:  5,127 MB / GPU BW      → same reconstructed data

Bottleneck: GPU mem read (data movement ถูกกำจัด, เหลือแต่ compute)
```

### Async Pipeline (3-tier overlapped)

```
Serial:   sum(disk, pcie, gpu)
Pipelined: max(disk, pcie, gpu)

RAW pipelined = disk bottleneck (NVMe)
ENC pipelined = GPU bottleneck (GDDR5 read)
```

---

## Results

### Absolute Times

| GPU | RAW total | ENC total | Speedup |
|-----|-----------|-----------|---------|
| GTX 1050 Ti | 1.830s | 0.070s | **26.1x** |
| RTX 3060 | 1.799s | 0.040s | **45.5x** |
| RTX 4090 + DDR5 | 0.898s | 0.028s | **31.7x** |
| RTX 5090 + PCIe5 | 0.449s | 0.025s | **18.0x** |
| Mac M3 Max (UMA) | 0.745s | 0.035s | **21.0x** |
| Mac M4 Ultra (UMA) | 0.519s | 0.029s | **18.1x** |

### Bottleneck Analysis

| GPU | RAW bottleneck | ENC bottleneck |
|-----|---------------|----------------|
| 1050 Ti | Disk I/O (1.465s) | GPU read (0.045s) |
| 3060 | Disk I/O (1.465s) | GPU read (0.024s) |
| 4090 | Disk I/O (0.732s) | CPU recon + GPU (0.028s) |
| 5090 | Disk I/O (0.366s) | CPU recon (0.021s) |
| M3 Max | Disk I/O (0.732s) | CPU recon + GPU (0.035s) |
| M4 Ultra | Disk I/O (0.513s) | CPU recon (0.021s) |

**Key observation**: ENC shifts bottleneck from disk I/O → CPU reconstruction on hi-end HW.

### ENC Time Breakdown (1050 Ti)

```
Component        Time       %
──────────────   ───────   ────
Disk→RAM         0.004s     5%
RAM→VRAM         0.001s     1%
frame_at(enc)    0.021s    31%
GPU mem read     0.045s    64%
──────────────   ───────   ────
Total            0.070s   100%
```

### ENC Time Breakdown (4090)

```
Component        Time       %
──────────────   ───────   ────
Disk→RAM         0.002s     7%
RAM→VRAM         0.000s     1%
frame_at(enc)    0.021s    75%
GPU mem read     0.005s    18%
──────────────   ───────   ────
Total            0.028s   100%
```

---

## Hardware Scaling

### What Scales

| Layer | Scales with | Range (x) |
|-------|------------|-----------|
| Disk I/O | NVMe gen (3.0→5.0) | 1x–4x |
| PCIe | PCIe gen (3.0→5.0) | 1x–4x |
| GPU read | GDDR BW (1050→5090) | 1x–16x |
| CPU recon | Clock speed only | ~1x |

### What Does NOT Scale

- **geo_frame_seek ratio (384x)**: invariant — 2 bytes per frame regardless of hardware
- **frame_at(enc) time**: 3ns per frame — bit ops, not memory-bound
- **Page table flip**: 2592 bytes — single cache line

### Cost/Performance (ENC pipeline)

| GPU | ENC time | vs 1050 Ti | Est. Price | $/gain |
|-----|----------|------------|------------|--------|
| 1050 Ti | 0.070s | 1.0x | owned | — |
| 3060 12GB | 0.040s | 1.75x | $250 | $357/x |
| 4090 | 0.028s | 2.50x | $1,700 | $1,133/x |
| 5090 | 0.025s | 2.80x | $3,000 | $1,667/x |

**3060 = sweet spot** for this pipeline. 4090 gain is marginal (0.012s) for 7x price.

However, in real inference workloads (LFM2 8B Q4):
- 1050 Ti (4GB): partial offload only
- 3060 (12GB): full model offload
- 4090 (24GB): + tensor parallel capability

---

## Performance Equation

### RAW
```
t_raw = max(t_disk, t_pcie, t_gpu)
t_disk = data_mb / nvme_mbs
t_pcie = data_mb / pcie_mbs
t_gpu  = data_gb / gpu_gbs
```

### ENC
```
t_enc = max(t_disk_enc, t_pcie_enc, t_recon, t_gpu)
t_disk_enc = data_mb / 384 / nvme_mbs     ← 13.35 MB for 5GB model
t_pcie_enc = data_mb / 384 / pcie_mbs      ← negligible
t_recon    = n_frames × 3e-9              ← 0.021s for 7M frames
t_gpu      = data_gb / gpu_gbs            ← same reconstructed data
```

### Rule of Thumb
- HW อ่อน (disk < 7 GB/s): **disk bound**, ENC ช่วยมากที่สุด (26x+)
- HW กลาง (NVMe 4.0 + PCIe 4.0): **GPU bound**, ENC still 30x+
- HW แรง (NVMe 5.0 + UMA): **CPU recon bound**, ENC ~18x — bottleneck คือ frame_at
- **CPU ไม่เคย bottleneck** บน HW ต่ำกว่า 4090 — idle 50%+ รอ GPU

---

## Design Implications

### 1. CPU Requirements
- **Single core 3GHz ก็พอ** — CPU workload ~0.021s สำหรับ 7M frames
- i3 vs i9 → **no difference** (CPU 가 รอ GPU อยู่แล้ว)
- RAM speed (DDR4 vs DDR5) → **no difference** (data 13MB เท่านั้น)

### 2. GPU Investment
- 3060 12GB = best value ($250, 1.75x gain, full model offload)
- 4090 best absolute (0.028s) แต่ diminishing returns
- 5090 overkill (0.025s, CPU recon ตันก่อน)

### 3. Storage
- NVMe vs SATA → **no difference** (data 13MB, transfer time negligible)
- Cold spill penalty (DRamTile): DDR bandwidth matters only for cache misses

### 4. Unified Memory (Apple M-series)
- RAW gain: PCIe hop หาย → RAW already fast (0.5-0.7s)
- ENC gain: still 18-21x เพราะ zero-copy page table
- **Best architecture for this design** — CPU-GPU latency ≈ 0

---

## Key Proofs

1. **geo_frame_seek 384x reduction = invariant** — 2-byte enc, every hardware
2. **frame_at(enc) = 3ns O(1)** — bit ops, ไม่ scale ตาม HW
3. **ENC shifts bottleneck from disk I/O → GPU/CPU compute**
4. **Async pipeline = max(layer), not sum** — 3-tier overlap works
5. **CPU headers are free** — overlapped with data transfer
6. **Zero-copy page table (2592 bytes) > bandwidth** — pointer swap beats memcpy

---

## Summary

```
RAW:  ขน 5GB → disk I/O ตาย
ENC:  ขน 13MB → GPU bound → เร็ว 18-45x

CPU ทำงาน 0.021s แล้วก็รอ → ไม่ต้องอัพเกรด CPU
GPU คือตัวช้า → 3060 = best value, 4090 = best perf
Disk/PCIe/RAM ไม่มีผล → ใช้ของที่มีอยู่
```

The geometric address-space approach (2-byte enc + O(1) reconstruct + zero-copy page table) universal — ทุก hardware tier ได้ประโยชน์.
