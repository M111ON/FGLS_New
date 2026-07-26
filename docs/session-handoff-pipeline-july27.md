# Session Handoff: Geometric Grid Compression Pipeline

## วันที่: July 27, 2026

## สิ่งที่ทำเสร็จแล้ว

### Geo1State Breakthrough
พิสูจน์แล้วว่า **1 state (R) = ทั้ง tessellation**

Equal triangle → rotate 120° → 3 vertices → tessellate → ALL positions at k×R

Seed = R (fp16 = **2 bytes**) — เล็กที่สุดเท่าที่เป็นไปได้
- vs CenteredGrid: mean + std = 4B
- vs Vert24: center + radius + angle = 6B

**ผลลัพธ์จริงจาก 2 โมเดล (SmolLM2 + Qwen3):**
| Config | Seed | 4-bit Size | 4-bit Error | Ratio |
|--------|------|-----------|-------------|-------|
| **Geo1State** | **2B** | **20B** | **0.19%** | **0.59x** |
| CenteredGrid | 4B | 22B | 0.19% | 0.65x |
| Vert24 | 6B | 24B | 0.00% | 0.71x |
| FreeCentroid | 4B | 22B | 1.05% | 0.65x |

### บทเรียนสำคัญ
1. **Grid quality > centroid count** — CenteredGrid (formulaic) ชนะ FreeCentroid 5-6x
2. **Sorted weights are linear** — SortFirst 证明 Q8_0 int8 sorted = near-perfect ramp
3. **Equal triangle tessellation IS the grid** — positions at k×R, derive from 1 R
4. **4-bit = sweet spot** — 20B (0.59x) 0.19% error
5. **Cross-model stability** — error เหมือนกันทั้ง SmolLM2 และ Qwen3

## ไฟล์ที่สำคัญ

| ไฟล์ | คำอธิบาย |
|------|----------|
| `runner/pipeline_real_test.c` | **Main benchmark** — 8 configs × 5 bit-widths, compile `-Wall -Werror` clean |
| `runner/pipeline_tune.c` | Standalone tuning benchmark (8 variants) |
| `docs/pipeline_tune_report.md` | **Full report** — หลักการทำงาน + ผลพิสูจน์เปรียบเทียบ |
| `AGENTS.md` | มี Verification Loop Guard rule แล้ว |

## วิธีรัน benchmark
```bash
cd runner
gcc -O2 -Wall -Werror -o pipeline_real_test.exe pipeline_real_test.c -lm
./pipeline_real_test.exe "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf" 2000
./pipeline_real_test.exe "I:/model/Qwen3-0.6B-Q8_0.gguf" 2000
```

## สิ่งที่ควรทำต่อ (Pending Work)

1. **Implement actual decompression** — ตอนนี้ benchmark  đo encode-side error, ยังไม่มี decode roundtrip
2. **Test on BF16** — ตอนนี้ทดสอบแค่ Q8_0, ควรลอง BF16
3. **Integrate into GGUF pipeline** — เอามาใช้จริงกับ GGUF model files
4. **Explore higher tessellation layers** — 7→19→37 points จาก multi-layer tessellation
5. **Explore non-uniform grid spacing** — Fibonacci spiral grid, logarithmic spacing

## สิ่งที่ต้องรู้

- `model` = symlink ไป `I:\model\` (ไม่ใช่ junk)
- `model/` ไม่มีโมเดลจริง — ต้องใช้ `I:/model/*.gguf` ตรงๆ
- ไฟล์ `--help` และ `-f` = junk จากคำสั่งผิด → ลบแล้ว, .gitignore ป้องกันแล้ว
- `make test` = 135 PASS / 0 FAIL (test suite ไม่รวม runner standalone tools)
- `pipeline_real_test.c` เป็น standalone — ไม่ reference ใน Makefile
