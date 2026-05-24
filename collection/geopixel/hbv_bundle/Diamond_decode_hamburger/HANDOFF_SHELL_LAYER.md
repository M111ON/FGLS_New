# HANDOFF — Shell Layer Wire + Lossless Proof
_Session: 2026-05-15_

## สิ่งที่พิสูจน์ได้แล้ว

reconstruct lossless ได้จาก **Cover เท่านั้น (552B)**
ไม่ต้องเก็บ geometry เลย

```
Cover (552B):
  FiboLayerHeader  32B  — fibo clock (tick_start, layer_seq, inv_witness)
  seeds[64]       512B  — seed per slot
  _pad              8B
```

derive ทั้งหมดจาก slot index `i`:
```c
face    = i % 12
channel = face % 3       // 0=Y 1=Cg 2=Co
flags   = (i >= 48) ? 0x01 : 0x00
invert  = XOR(seeds per channel)
```

## Critical Warning ⚠️

**ระบบนี้ไม่ใช่ image encoder**
ใช้แค่ flow structure ของ image sequence เป็น geometry template
ถ้า decode เป็น pixel → ได้ noise เต็มไปหมด — นั่นคือ wrong decoder

## Architecture

```
Cover header
  └─ seeds[64] + FiboLayerHeader
       └─ z=0: 64 slots → face/channel derive จาก index
       └─ z=1: next layer (layer_seq++)
       └─ z=144: closure (FIBO_CLOSURE)

1 z-layer = 1 HilbertPacket64 = 64 cells
cell[i]: seed=cover.seeds[i], face=i%12, channel=face%3
invert: XOR fold per channel (R/G/B = Y/Cg/Co)
```

## Files

| ไฟล์ | สถานะ |
|------|--------|
| `frustum_trit.h` | ✅ 10/10 PASS |
| `frustum_slot64.h` | ✅ 7/7 PASS |
| `diamond_shell_codec.h` | ✅ 7/7 PASS (lossless) |
| `test_lossless_minimal.c` | ✅ 64/64 PASS — proof |

## TODO ต่อไป

1. seed delta encoder (consecutive XOR บน seed table → บีบ 512B)
2. wire Cover เข้า hamburger cover page จริง
3. multi-z frame container (z0..zN + closure marker)
