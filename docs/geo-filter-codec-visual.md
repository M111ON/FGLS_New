# GEO Filter Codec — Visual Spec

## หลักการ

```
FILTER → MAP → XOR DELTA → STORE
  ↓        ↓       ↓          ↓
binary   3D→2D   sparse    lossless
 cube    project  zeros     reconstruct
```

## ขั้นตอนทำงาน

```
原始 weights (1000 cells = 10×10×10)
    │
    ▼
┌─────────────────────────────────────┐
│ STEP 1: FILTER BY VALUE             │
│                                     │
│  weights[x,y,z] = 3                │
│       ↓                             │
│  cube_3[x,y,z] = 1  (ถ้าค่า=3)     │
│  cube_3[x,y,z] = 0  (ถ้าไม่ใช่)    │
│                                     │
│  ทำซ้ำสำหรับค่า 0-9                  │
│  → ได้ 10 binary cubes              │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ STEP 2: 3 PROJECTIONS (per cube)    │
│                                     │
│  cube_3:                            │
│  ┌──────────────────────┐          │
│  │ map_x[y,z] = OR(x)   │ 10×10   │
│  │ map_y[x,z] = OR(y)   │ 10×10   │
│  │ map_z[x,y] = OR(z)   │ 10×10   │
│  └──────────────────────┘          │
│                                     │
│  10 cubes × 3 maps = 30 maps       │
│  30 × 100 bits = 3000 bits         │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ STEP 3: XOR DELTA                   │
│                                     │
│  map[0] = full (เก็บทั้งหมด)         │
│  map[1] = map[0] XOR delta[1]      │
│  map[2] = map[1] XOR delta[2]      │
│  ...                                │
│  map[9] = map[8] XOR delta[9]      │
│                                     │
│  delta ส่วนใหญ่ = 0 (sparse)        │
│  → RLE compress                     │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ STEP 4: STORE                       │
│                                     │
│  ┌────────────────────────────┐    │
│  │ Header (1 byte)            │    │
│  │   active_count | dimension │    │
│  ├────────────────────────────┤    │
│  │ Active values (N bytes)    │    │
│  │   e.g., [1, 3, 5, 8]      │    │
│  ├────────────────────────────┤    │
│  │ First maps (37.5 bytes)    │    │
│  │   map_x[0] + map_y[0]     │    │
│  │   + map_z[0]              │    │
│  ├────────────────────────────┤    │
│  │ Delta streams (sparse)     │    │
│  │   delta_x[1..9]           │    │
│  │   delta_y[1..9]           │    │
│  │   delta_z[1..9]           │    │
│  │   (RLE compressed)        │    │
│  └────────────────────────────┘    │
└─────────────────────────────────────┘
```

## Reconstruction

```
┌─────────────────────────────────────┐
│ RECONSTRUCT (Lossless)              │
│                                     │
│ 1. Read first maps                  │
│    map_x[0], map_y[0], map_z[0]    │
│                                     │
│ 2. XOR deltas → restore all maps    │
│    map_x[1] = map_x[0] XOR Δx[1]  │
│    map_y[1] = map_y[0] XOR Δy[1]  │
│    map_z[1] = map_z[0] XOR Δz[1]  │
│    ...                              │
│                                     │
│ 3. 3D reconstruction                │
│    cube_v[x,y,z] =                 │
│      map_x[v][y,z] AND            │
│      map_y[v][x,z] AND            │
│      map_z[v][x,y]                │
│                                     │
│ 4. Assign weight                    │
│    weights[x,y,z] = v              │
│    ถ้า cube_v[x,y,z] = 1           │
└─────────────────────────────────────┘
```

## ตัวเลข Compression

```
Raw:          1000 × 4 bits = 4000 bits
Full (10):    3000 + ~500 = 3500 bits (1.14x)
3 active:     900 + ~200 = 1100 bits (3.6x)
1 active:     300 bits (13.3x)
```

## ข้อจำกัด

```
✓ Lossless:  ค่า 1 อยู่บนเส้นตรง (line) ผ่าน cube
✗ Not lossless: ค่า scatter สุ่ม (ต้องเก็บตำแหน่งเพิ่ม)
```
