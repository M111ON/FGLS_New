# Fibo Tick — Three-View Entropy Container Architecture

> เอกสารสำหรับตัวเอง — integration ของ ribcage, fibo_spine, barrier ที่ใส่ให้ fibo_tick

---

## แก่นของระบบ

Fibo Tick **ไม่ใช่** component เดี่ยว — มันคือ **สามมุมมองของตำแหน่งเดียวกัน** บน field 20736 ช่อง

```
rdh_capture(data) → enc (2 bytes) → frame_at(enc)
                                        ↓
                   ┌─────────────────────────────────────┐
                   │  View 1: frame_seek (geom)          │
                   │    face(0..11) slot(0..119)         │
                   │    ico_idx(0..161) phase(0..11)     │
                   ├─────────────────────────────────────┤
                   │  View 2: fibo_spine (spine)         │
                   │    pipe_id(0..1727) tick(0..11)     │
                   │    Jet Bridge at tick 11             │
                   ├─────────────────────────────────────┤
                   │  View 3: p5h_ribcage (flower)       │
                   │    flower_id(0..1727) phase(0..9)   │
                   │    texture(inner/outer) barrier      │
                   └─────────────────────────────────────┘
```

ทั้งสาม view มองตำแหน่ง **เดียวกัน** — แค่ผ่านเลนส์ต่างกัน

---

## ไฟล์ที่เกี่ยวข้อง

| ระบบ | ไฟล์ | ที่อยู่ |
|:-----|:-----|:-------|
| Integration core | `fibo_tick.h` | `core/fibo_tick.h` |
| Frame seek | `geo_frame_seek.h` | `core/geo_frame_seek.h` |
| RDH capture | `rdh_capture.h` | `collection/rdh/rdh_capture.h` |
| Fibo spine + P5HRibcage | `fibo_spine.h` | `collection/dgls/geo/include/fibo_spine.h` |
| P5H flower field + barrier | `p5h_ribcage.h` | `collection/include/p5h_ribcage.h` |
| Integration test | `test_fibo_tick.c` | `pipeline/test_fibo_tick.c` |

---

## สถาปัตยกรรม

### 1. พื้นที่ — 20736 ตำแหน่ง

```
GEO_FULL = 144 × 144 = 20736

แบ่งเป็น:
  1728 pipes × 12 ticks = 20736    (fibo_spine)
  1728 flowers × 12 phases = 20736  (p5h_ribcage)
  12 faces × 120 slots × 144 phases = 20736  (เดา, เว้นแต่ timeline 1440)
```

Timeline 1440 = 12 faces × 120 slots = stride-37 walk
Full field 20736 = 12 × 1728 = timeline × 14.4

### 2. Tick = synchronization axis

ทุก tick (0..11) มีความหมายต่างกันในแต่ละ view:

| tick | frame_seek | fibo_spine | p5h_ribcage | storage action |
|:-----|:-----------|:-----------|:-------------|:---------------|
| 0 | phase 0 | cycle start | BARRIER_SYNC | FREEZE |
| 1 | phase 1 | normal | BARRIER_ENTER (new flower) | MAIN |
| 2..10 | phase 2..10 | normal | pipe room (9 phases) | PIPE |
| 11 | phase 11 | JET BRIDGE trigger | pipe room phase 10 | BRIDGE |

### 3. Storage actions (4 modes)

```
FT_STORE_MAIN   (0) — store ที่ container[face][slot][ico_idx]
                    ใช้ตอน tick 1 (normal entry)

FT_STORE_PIPE   (1) — อยู่ใน pipe room (tick 2..10)
                    inner/outer texture routing:
                      outer phase (0,2,4,6,8) → เก็บที่ outer container
                      inner phase (1,3,5,7,9) → เก็บที่ inner container

FT_STORE_BRIDGE (2) — Jet Bridge (tick 11)
                    data ถูกส่งเข้าระบบ residual_space
                    ผ่าน bond_key → เก็บแบบ timeless
                    ไม่ต้อง container เลย

FT_STORE_FREEZE (3) — barrier sync (tick 0)
                    ที่ convergence boundary
                    data ทั้งหมดใน cycle นี้ถูก freeze
                    ไว้ให้ container รู้ว่าถึงเวลาสรุป
```

### 4. จำนวน slot ต่อ action (บน 1440-cycle)

```
FREEZE: 120 slots (ทุก enc ที่ tick=0)
MAIN:   120 slots (ทุก enc ที่ tick=1)
PIPE:   1080 slots (ทุก enc ที่ tick=2..10)
BRIDGE: 120 slots (ทุก enc ที่ tick=11)
รวม:    1440 slots
```

---

## Mapping functions

### enc → pipe + tick
```c
uint16_t pipe_id = ft_enc_to_pipe(enc);    // 0..1439 (primary)
uint8_t  tick    = ft_enc_to_tick(enc);    // 0..11
```

### enc → flower + phase + texture
```c
uint16_t flower_id  = ft_enc_to_flower(enc);
uint8_t  phase      = ft_enc_to_phase_in_flower(enc);  // 0..9 or 255
uint8_t  texture    = ft_enc_to_texture(enc);           // OUTER/INNER
```

### enc → geom frame
```c
DualFrame f = frame_at(enc);   // f.face, f.slot, f.phase, f.ico_idx
```

### enc → field position
```c
uint16_t ring, wedge;
ft_enc_to_field(enc, &ring, &wedge);
```

### enc → storage action
```c
uint8_t action = ft_store_action(enc);
// FT_STORE_MAIN, FT_STORE_PIPE, FT_STORE_BRIDGE, FT_STORE_FREEZE
```

### Slot index (linear 0..20735)
```c
uint32_t idx = ft_slot_index(pipe_id, tick);     // pipe×12 + tick
ft_from_slot_index(idx, &pipe_id, &tick);         // inverse
```

---

## P5H field sync (barrier integration)

การ sync P5HField กับ enc timeline:
```c
P5HField field;
p5h_field_init(&field);

uint16_t enc = 0;
for (uint32_t t = 0; t < N; t++) {
    p5h_field_observe(&field, enc);
    
    if (p5h_is_barrier(&field)) {
        // tick 0 — convergence point
    }
    if (p5h_is_flower_start(&field)) {
        // new flower initialized: phase=0, texture=outer
    }
    
    P5HFlower *fl = p5h_field_peek(&field, field.flower_now);
    // fl->phase: 0..9
    // fl->texture: P5H_TEX_OUTER / P5H_TEX_INNER
    
    enc = ft_next(enc);
}
```

---

## Fibo spine full integration

For full spine + Jet Bridge (heap required):
```c
#include "fibo_spine.h"
#include "fibo_tick.h"

FiboSpine spine;
fibo_spine_init(&spine);

P5HRibcage ribcage;
p5h_ribcage_init(&ribcage, &spine);

// Per-pipe mode for independent tick advance
spine.mode = FS_MODE_PERPIPE;

// Walk enc timeline
uint16_t enc = 0;
for each data chunk {
    enc = (uint16_t)(rdh_capture(data, len, &cfg) % 1440);
    
    uint8_t tick = ft_enc_to_tick(enc);
    uint16_t pipe = ft_enc_to_pipe(enc);
    
    // Advance pipe + record in ribcage
    fibo_spine_pipe_tick(&spine, pipe);
    p5h_ribcage_step(&ribcage, pipe, tick, bond_key_of(data));
    
    // Jet Bridge at tick 11
    if (fibo_spine_pipe_is_bridge(&spine, pipe)) {
        jet_bridge_hop(&spine, pipe, data, 48, residual_fn);
    }
}

// Freeze at sync boundary
p5h_freeze_at_tick12(&ribcage);

p5h_ribcage_free(&ribcage);
```

---

## สรุป

**ก่อน integration:**
```
rdh_capture → enc → frame_seek → container
O(n)        O(1)   O(1)         linear
            2B     face/slot     flat store
```

**หลัง integration:**
```
rdh_capture → enc → 3 views → 4 storage modes → container + residual
O(n)        O(1)  O(1)      O(1) routing      tiered
            2B    geom       freeze/bridge/     main storage +
                  spine       pipe/main         timeless residual
                  flower
```

สามสิ่งที่เพิ่มเข้ามา:
1. **fibo_spine** — 1728 pipes × 12 ticks, Jet Bridge, residual space
2. **p5h_ribcage** — barrier sync, 10-phase pipe room, inner/outer texture
3. **fibo_tick.h** — bridge layer ที่ mapping ระหว่าง enc → ทั้ง 3 ระบบ

สิ่งที่ได้:
- เกมใหญ่ขึ้น — 4 storage mode แทนที่จะเป็น flat container
- หลากมุมมอง — geom address, pipe+tick, flower+phase
- Barrier sync — convergence point ทุกๆ 12 ticks
- Jet Bridge — escape hatch สำหรับ high-entropy data เข้า timeless residual

*— July 22, 2026*
