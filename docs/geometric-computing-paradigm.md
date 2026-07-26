# Geometric Computing Paradigm

> "1, 2, 3, 4 แทน 1→process→2→process→3"
>
> Geometry = multi-dimensional LUT ที่ process built-in กับ structure
> เป็น mental model — ไม่มี physical constraint

---

## 1. The Core Shift: Sequential → Geometric

### Von Neumann (Sequential)
```
1 → process → 2 → process → 3 → process → 4
   ↑ step       ↑ step       ↑ step
   data นิ่ง     ALU ทำงาน    result ย้าย
```

- CPU-centric
- Data อยู่กับที่ → คำสั่งเคลื่อนย้าย
- ทุก operation = instruction cycle
- Process step = cost ทุกครั้ง

### Geometric (Structural)
```
1, 2, 3, 4

pointer → geometry → values
   ↑ implicit     ↑ inherent
   position       adjacency
   layer          transpose
   shift          distance
```

- Structure-centric
- Position เปลี่ยน → value เปลี่ยนตาม (ไม่ต้อง process)
- Operation = transformation of position, not data movement
- "Process" ถูกแทนที่ด้วย relationship ใน geometry

---

## 2. Geometry = Multi-Dimensional LUT

1D LUT:
```
index → value        (linear, ไม่มีความสัมพันธ์ระหว่าง slot)
0 → 42  
1 → -7
2 → 128
```

Geometric LUT (Dual Square 360×360):
```
(θ, φ, layer) → value         (3 มิติ)
XY(30, 120)   → weight A      ← outer/positive
YX(30, 120)   → weight B      ← inner/negative (transpose)

Operations ที่ built-in:
  adjacency   → neighbor = convolution (no loop)
  transpose   → XY↔YX    = sign flip  (no NOT)
  shift       → (θ+dθ, φ+dφ) = data movement (no memcpy)
  distance    → XOR(θ, φ) = magnitude  (no ALU)
  rotation    → continuous shift = permutation (no gather/scatter)
```

### สิ่งที่ 1D LUT ทำไม่ได้ แต่ Geometric LUT ทำได้:

| Operation | 1D LUT | Geometric LUT |
|-----------|--------|---------------|
| lookup | O(1) | O(1) |
| neighbor read | O(n) scan | O(1) by (θ±1, φ) |
| batch read | sequential | 6-direction parallel |
| sign variation | need extra bit | transpose = free |
| distance calc | subtract | XOR = 1 cycle |
| permutation | gather index | rotate (θ, φ) |
| wave modulation | per-element | radius = continuous |

---

## 3. Dual Square Geometry

```
┌─────────────────────────────────────┐
│                                     │
│  XY square (outer +)                │
│  ┌─────────────┐                    │
│  │ θ (0..359)  │  X=θ, Y=φ         │
│  │ φ (0..359)  │  129,600 slots    │
│  └──────┬──────┘                    │
│         │ transpose                 │
│  ┌──────┴──────┐                    │
│  │ θ (0..359)  │  X=φ, Y=θ         │
│  │ φ (0..359)  │  129,600 slots    │
│  └─────────────┘                    │
│  YX square (inner -)                │
│                                     │
│  Total: 259,200 addressable points  │
└─────────────────────────────────────┘
```

Properties:
- 360×360 = 129,600 slots/square — overcapacity สำหรับ Q8 (256)
- sign = layer (XY=+, YX=-) — 0 bits
- XOR(θ, φ) = weight magnitude
- Transpose = sign flip (โดยไม่ต้อง NOT gate)

---

## 4. Operations = Transformations of Position

โปรแกรม geometric computing = sequence ของ position transformation ไม่ใช่ data transformation:

```
Counterpart ใน sequential computing:

shift(dθ, dφ)     = data movement instruction
transpose()       = sign negation
XOR(θ, φ)         = ALU compute
neighbor(θ, φ)    = memory load from adjacent address
rotation(t)       = permutation (gather/scatter)
radius(θ, φ)      = wave modulation (variation)
layer(θ, φ)       = sign selection
```

แต่ละ operation = **เปลี่ยนตำแหน่ง** ไม่ใช่เปลี่ยนค่า — ค่าเปลี่ยนตาม position โดยอัตโนมัติ

---

## 5. Mental Model — No Physical Constraint

เพราะเป็น mental model (no hardware), ข้อจำกัดทางกายภาพไม่มี:

| Physical constraint | Mental model impact |
|--------------------|-------------------|
| cache line | ไม่มี — position = immediate decode |
| memory bandwidth | ไม่มี — 1 pointer = ∞ parallel reads |
| gate delay | ไม่มี — XOR is instantaneous in concept |
| storage density | ไม่มี — geometry scales independently |
| bus width | ไม่มี — 6 directions = 6-way parallel |

นี่คือ freedom ที่ 1D LUT ไม่มี:
- สามารถนิยาม operation ใหม่เรื่อยๆ
- ความสัมพันธ์ระหว่างตำแหน่ง = process ที่ built-in
- "Algorithm" = path บน geometry, ไม่ใช่ instruction sequence

---

## 6. Implication: 1, 2, 3, 4 แทน 1→process→2

```
Sequential:   1 →[step]→ 2 →[step]→ 3
              ↑  process  ↑  process  ↑
              
Geometric:    pointer →[geometry]→ [implicit relationships]
              1, 2, 3, 4 = structure elements
              "process" = adjacency, transpose, XOR
              "step" = shift position
```

1, 2, 3, 4 ไม่ใช่ sequence ของ operation — คือ **structure** ที่มีความสัมพันธ์ระหว่าง element ในตัว

---

## 7. Open Questions

- Wave modulation + radius variation → data density เพิ่มขึ้นเท่าไหร่?
- RDH capture รอบ (θ, φ) + L-Block redirect ใช้ทำ routing pattern อะไรได้บ้าง?
- Dual square geometry นี้ extend ไปยัง 3D (sphere) หรือ 4D (sphere×time) ด้วยหรือไม่?
- ถ้า operation = position shift → "program" = path ใน geometry → จะ compile ยังไง?
- ความสัมพันธ์นี้ใช้เป็น isomorphism ระหว่าง neural network topology กับ geometric field ได้ไหม?

---

*Started: July 24, 2026 — Brainstorm session on dual icosahedron/sphere/square*
