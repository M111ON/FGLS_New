# GEO Filter Codec — Spec v1

## หลักการ

**MAP NOT COMPRESS**
- Filter ค่า → binary cube → 3 projections → XOR delta
- Geometry = address = reconstruction
- ไม่ต้องเก็บ index/permutation

## ขั้นตอน

### Step 1: Filter by Value

```
Input: weights[N] ∈ {0, 1, 2, ..., 9}  (หรือ quantized values)

For each value v ∈ {0..9}:
  cube_v[x,y,z] = 1 ถ้า weights[x,y,z] == v
  cube_v[x,y,z] = 0 ถ้าไม่ใช่
```

### Step 2: Create 3 Projections (per value)

```
For cube_v (binary 10×10×10):

map_x[v][y,z] = cube_v[0,y,z] OR cube_v[1,y,z] OR ... OR cube_v[9,y,z]
map_y[v][x,z] = cube_v[x,0,z] OR cube_v[x,1,z] OR ... OR cube_v[x,9,z]
map_z[v][x,y] = cube_v[x,y,0] OR cube_v[x,y,1] OR ... OR cube_v[x,y,9]

Size: 3 × 100 bits = 300 bits per value
```

### Step 3: XOR Delta Encoding

```
First map (v=0): store full
  map_x[0], map_y[0], map_z[0]  = 300 bits

Delta for v=1..9:
  delta_x[v] = map_x[v] XOR map_x[v-1]
  delta_y[v] = map_y[v] XOR map_y[v-1]
  delta_z[v] = map_z[v] XOR map_z[v-1]

Delta ส่วนใหญ่ = 0 (sparse) → compress ด้วย RLE
```

### Step 4: Store

```
Format:
┌─────────────────────────────────────────┐
│ Header: 1 byte                          │
│   bits[7:4] = number of active values   │
│   bits[3:0] = cube dimension (10)       │
├─────────────────────────────────────────┤
│ Active value table: N_active × 1 byte   │
│   e.g., [1, 5, 8] ถ้ามี 3 ค่า active    │
├─────────────────────────────────────────┤
│ First maps (v=first_active):            │
│   map_x: 100 bits (12.5 bytes)          │
│   map_y: 100 bits (12.5 bytes)          │
│   map_z: 100 bits (12.5 bytes)          │
├─────────────────────────────────────────┤
│ Delta streams (v=second..last_active):  │
│   For each value:                       │
│     delta_x: sparse bitmap (RLE)        │
│     delta_y: sparse bitmap (RLE)        │
│     delta_z: sparse bitmap (RLE)        │
└─────────────────────────────────────────┘
```

## ตัวเลข Compression

### กรณีที่ 1: 1000 cells, 10 ค่า (Full)

```
Raw: 1000 × 4 bits = 4000 bits
Maps: 30 × 100 = 3000 bits
XOR delta: sparse → ~500 bits (est)
Total: ~3500 bits = 1.14x
```

### กรณีที่ 2: 1000 cells, 3 ค่า active

```
Raw: 4000 bits
Maps: 3 × 300 = 900 bits
Delta: sparse → ~200 bits
Total: ~1100 bits = 3.6x
```

### กรณีที่ 3: 1000 cells, 1 ค่า (80% ซ้ำ)

```
Raw: 4000 bits
Maps: 1 × 300 = 300 bits
Total: 300 bits = 13.3x
```

## Reconstruction (Lossless)

```
1. Read first maps: map_x[0], map_y[0], map_z[0]
2. For each delta v=1..9:
     map_x[v] = map_x[v-1] XOR delta_x[v]
     map_y[v] = map_y[v-1] XOR delta_y[v]
     map_z[v] = map_z[v-1] XOR delta_z[v]
3. Reconstruct cube from3 projections:
     cube_v[x,y,z] = map_x[v][y,z] AND map_y[v][x,z] AND map_z[v][x,y]
4. Assign weight: weights[x,y,z] = v ถ้า cube_v[x,y,z]=1
```

## ข้อจำกัด

1. **Binary cube assumption**: filter แล้ว cube เป็น {0,1} เท่านั้น
2. **Structure assumption**: ค่า 1 ต้องอยู่บนเส้นตรง (line) ผ่าน cube → 3 projections สร้างได้ lossless
3. **ถ้าค่า scatter แบบสุ่ม**: ต้องเก็บตำแหน่งเพิ่ม (ไม่ lossless จาก 3 projections เดียว)
4. **Dimension**: 10×10×10 = 1000 cells (fixed) — ขยายได้แต่ต้องคิด projection size ใหม่

## Integration Path

```
GGUF Q8_0 raw bytes
    ↓
Quantize to 10 buckets (0-9)
    ↓
For each 10×10×10 block:
    Filter → 30 maps → XOR delta → store
    ↓
Container (CRC-64)
    ↓
Output
```
