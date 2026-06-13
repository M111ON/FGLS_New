# SESSION HANDOFF — CTD Tensor Container
## Date: 2026-06-08 | Status: Phase 1 complete, Phase 2 in progress

---

## สถานะปัจจุบัน (DONE)

### Core files พร้อมใช้งาน
```
ctd_tri.h              — CTDCard 20B, FNV hash, dodeca face 0-11
ctd_octa.h             — Y3/Y6 centroid engine (snapshot→encode→recon)
pogls_weight_container.h — PWC V2/V3 decoder, random access O(1)
zone_card_v3.h         — ZoneCard routing layer
pogls_weight_container.py — Python wrapper
```

### Benchmark ที่พิสูจน์แล้ว
```
Kokoro  54 tensors:  max_err=0, Y6 lv1=93%, max_shift=0.629
SmolVLM 471 tensors: max_err=0, Y6 lv1=84%, max_shift=7.850
Face coverage: 12/12 ทั้งคู่
CTDCard overhead: 0.004% vs model size
Y6 vs Y3: stability +17% shift reduction, cost = 0
```

### Architecture ที่ confirmed
```
tri (3) → anchor/ctd
hex (6) → stable packing wall (d1=d2), boundary r=6
pent (5)→ traversal coprime (gcd(5,6)=1)
diamond → bridge gap pent↔hex (2 types: inward/outward)
Y3      → 3-feature centroid encoder
Y6      → hexagram = 2×Y3 averaged → more stable
```

---

## ปัญหาที่ยังค้างอยู่

### 1. Vision tensors (SmolVLM) max_shift สูง
```
Kokoro max_shift  = 0.629  (audio = low variance)
SmolVLM max_shift = 7.850  (vision = high variance)
→ lv2/lv3 tensors เยอะกว่า audio มาก
→ delta encoding จะ compress น้อยกว่าที่หวัง
```

### 2. Anchor assignment ยังใช้ name hash
```
cos within-anchor ≈ cos across-anchor (-4% = noise)
→ anchor ไม่ได้ group tensors ที่คล้ายกันไว้
→ delta mean ยังไม่ tight พอ
```

### 3. Delta encoding ยังไม่ implement
```
lv1 tensors (93% Kokoro, 84% SmolVLM) = stable
แต่ยังไม่ได้ store mean + delta จริงๆ
compression ratio จริงยังไม่รู้
```

---

## Plan Session ต่อไป

### Priority 1: วัด std deviation (validation)
```python
# ต้องรู้ก่อนว่า delta encodingจะได้ผลไหม
for each anchor_group:
    compute std_within = std(tensors in group)
    compute std_across = std(all tensors)
    
target: std_within / std_across < 0.5 → delta encoding viable
```

### Priority 2: ลอง Gosper-based anchor assignment
```
แทน name hash → cosine cluster → Gosper index
ถ้า cos within >> cos across (+20%) → ดีกว่า name hash
```

### Priority 3: implement delta storage
```
lv1: store mean vector per anchor (1 ครั้ง)
lv2: store delta = tensor - mean
วัด compression ratio จริง
target: 50% size reduction
```

### Priority 4: vision-specific scheme
```
SmolVLM max_shift 7.85 สูงเกินไป
น่าลอง Y9 (Y6 + diagonal tri) สำหรับ vision layer
หรือ scale ใหญ่กว่าสำหรับ vision-type tensors
```

---

## Key Insights จาก Session นี้

```
1. Pentagon coprime = traversal guarantee (gcd(5,6)=1)
2. Hexagram (Y6) = 2 triangle orbit → smoother centroid
3. Diamond = universal bridge pent↔hex, 2 types (inward/outward)
4. Square (4) = ใช้ไม่ได้ d1≠d2, diamond (rotated 45°) = ใช้ได้
5. Torus center = ctd invariant point ไม่ว่า tensor transform ยังไง
6. FiboClock = helix ไม่ใช่ circle → non-repeating, no deadlock
7. CTD bond = topology signature 2 bytes → lightweight similarity
8. Fibonacci multiples pattern = visual proof ของ gcd theory
9. Hex r=6 = hard boundary ของระบบ ถ้า edge=3
10. PWC zone_id = CTD geo address → O(1) seek by geometry
```

---

## Files ที่ต้องทำต่อ

```
[ ] ctd_delta.h    — mean + delta storage per anchor group
[ ] ctd_gosper.h   — Gosper-based anchor assignment  
[ ] test_delta.c   — validate compression ratio
[ ] benchmark_pwc_vs_safetensors.py — seek time comparison
```

---

*Handoff: tensorcontainer session 2026-06-08*
*Next: std deviation validation → Gosper anchor → delta encoding*
