# CTD Phase 2 Architecture — Delta Encoding via Geometry

## ปัญหาที่ต้องแก้

Phase 1 พิสูจน์แล้วว่า CTDCard ทำงานเป็น index ได้ดี (0.004% overhead)
แต่ anchor assignment ยังใช้ name hash → ไม่มี semantic clustering

```
cos within-anchor ≈ cos across-anchor (-4% difference = noise)
แปลว่า anchor ไม่ได้ group tensors ที่คล้ายกันไว้ด้วยกัน
→ ยังไม่สามารถ reconstruct weights จาก geometry ได้
```

---

## 3 Building Blocks

### 1. Gosper Curve (Image 2 — hex indexing)
Gosper curve คือ space-filling curve บน hex grid ที่ **preserve locality**
- เซลล์ที่ index ใกล้กัน = อยู่ในพื้นที่ใกล้กันด้วย
- red lines ใน diagram = path ที่วิ่งผ่านทุก cell โดยไม่ข้าม
- **ใช้เป็น anchor assignment ที่ดีกว่า name hash**

```
แทนที่จะ: anchor = f(name_hash)
เปลี่ยนเป็น: cluster tensors by cosine similarity ก่อน
             แล้ว assign anchor ตาม Gosper index ของ cluster
             → tensors ที่คล้ายกัน = anchor ใกล้กัน = geo address ใกล้กัน
```

### 2. Pentagon Ring (Image 1 — 3-color ring)
Pentagon clusters วนเป็น ring 3 สี (blue/green/dark)
- แต่ละสี = type ของ tensor (attention / ffn / norm)
- ring = anchor group ที่ related กัน
- **ใช้เป็น semantic grouping layer บน anchor space**

```
ring ที่ 1 (attention tensors): q_proj, k_proj, v_proj, o_proj
ring ที่ 2 (ffn tensors):       gate_proj, up_proj, down_proj
ring ที่ 3 (norm tensors):      layernorm, embed, lm_head
```

Pentagon coprime property (gcd(5,6)=1) การันตีว่า path ผ่านทุก anchor ใน ring
ก่อนปิด loop — ไม่มี dead zone

### 3. Helix Fold (Image 3 — Fibo timeline)
Layers stack เป็น helix ตาม FiboClock
- lv1 = mean vector ของแต่ละ anchor ring (store ครั้งเดียว)
- lv2 = delta ของแต่ละ tensor จาก mean
- Header = CTDCard cover page (index)

```
Storage structure:
Header (CTDCard × N tensors)    → 9.4KB  [Phase 1, done]
lv1: mean per anchor ring       → เล็กมาก (~ring_count × weight_dim)
lv2: delta per tensor from mean → ขนาดขึ้นกับ delta distribution
```

---

## Phase 2 Target Architecture

```
Step 1: Cluster tensors by cosine similarity
        → find natural groups in weight space

Step 2: Assign anchor via Gosper index
        → locality-preserving: similar tensors = nearby anchor

Step 3: Compute mean vector per anchor ring (lv1)
        → store once per ring

Step 4: Store delta = tensor - mean (lv2)
        → ถ้า delta distribution tight → delta เล็กกว่า full weights มาก
        → สามารถ quantize delta ได้ aggressive กว่า full weights

Step 5: Reconstruct = mean + delta
        → lossless ถ้า delta stored full precision
        → lossy แต่ high quality ถ้า delta quantized
```

---

## Compression Target

```
ถ้า within-anchor std << across-anchor std:
  delta เล็ก → quantize delta ด้วย fewer bits
  target: 50% size reduction (lossless หรือ near-lossless)

เทียบกับ GGUF Q8: ~50% reduction แต่ lossy
CTD Phase 2 target: ~50% reduction แต่ reconstructable
```

---

## Validation ที่ต้องทำก่อน

```
1. วัด std deviation within-anchor vs across-anchor
   (ตอนนี้วัดแค่ cosine similarity)
   ถ้า std within << std across → delta encoding จะได้ผล

2. ลอง k-means cluster tensors แล้ว assign anchor
   วัด cos within vs across ใหม่
   ถ้า gap > 20% → anchor assignment ใหม่ดีกว่า

3. ลอง store delta แล้ววัด entropy
   ถ้า delta entropy ต่ำกว่า original → compression ได้จริง
```

---

## สรุป

```
Phase 1 (done):    name hash → anchor → CTDCard index, 20B/tensor
Phase 2 (next):    cosine cluster → Gosper anchor → mean+delta storage
                   Pentagon ring = semantic group layer
                   Helix fold = Fibo timeline multi-resolution

Key insight: geometry ต้องสะท้อน weight space จริงๆ
             ไม่ใช่แค่ name routing
             Gosper curve คือ bridge ระหว่างสองโลก
```

---

*CTD Phase 2 Architecture notes*
*ต่อจาก Phase 1 (ctd_tri.h, test_ctd_basic.c)*
*Session reference: pentagon gear → torus CTD → delta encoding*
