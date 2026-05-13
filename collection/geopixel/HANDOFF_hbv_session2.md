# HBV Session 2 — Handoff Document

## State: WORKING ✅
pack/unpack SHA256-exact, multi-cycle, fibo LUT wired

---

## Files changed this session

| File | Changes |
|------|---------|
| `hb_vault.h` | header 8B→16B (orig+zdata), multi-cycle manifest (.gpx5.m) |
| `hamburger_pipe.h` | `hb_invert_record_enc` + codec param, skip clear-check RAW/NONE |
| `hamburger_encode.h` | multi-cycle encode_image loop, HbFiboLut, fibo_shell_walk include |
| `fibo_shell_walk.h` | unchanged, now wired into encode.h |

---

## Multi-cycle

```
total_tiles > HB_MAX_TILES(4096) → auto split
cycle 0 → out_c000.gpx5
cycle 1 → out_c001.gpx5
manifest → out.gpx5.m  (magic HBVM + n_cycles + total_tiles + W + H)
unpack reads manifest → iterate cycles → concat planes → reconstruct
```

---

## HbFiboLut (12B per tile)

```c
typedef struct {
    uint16_t tile_id;    // local id within cycle
    uint16_t cycle_id;   // which cycle file
    uint8_t  tick;       // fibo clock 0..143
    uint8_t  shell_n;    // cube shell 0..7 (= distance)
    uint8_t  spoke;      // tring spoke 0..5
    uint8_t  codec_id;   // GPX5_CODEC_*
    uint32_t invert_off; // direct seek offset in cycle file
} HbFiboLutEntry;

hb_fibo_lut_build(cycle_id, gpx5_file, out[])  // scan once → bake
hb_fibo_lut_seek(lut, n, tick, cycle)           // O(1) → offset
```

tick period = 1440 tiles (FSW_ENC_CYCLE)
tile[i] and tile[i+1440] share same tick

---

## Vision (next session priority)

```
QR scan → match hilbert signature
→ hb_fibo_lut_seek(tick) → invert_off
→ hb_tile_stream(file, invert_off, codec) → raw tile bytes
→ display AR / overlay

NO network. NO full decode. NO reprocess.
Just: match position → warp to offset → invert → show
```

เหมือน vinyl record — เข็มวางตรงร่อง เสียงออกทันที

---

## Next steps (priority order)

### 1. hb_tile_stream() — single tile decode (MOST IMPORTANT)
```c
// seek → decode 1 tile → return YCgCo pixels
int hb_tile_stream(
    const char      *gpx5_path,
    const HbFiboLutEntry *entry,  // from lut_seek
    uint8_t         *out_ycgco,   // caller alloc: tw*th*6 bytes
    uint32_t         out_sz);
```
Uses `invert_off` directly — no full file decode

### 2. CODEC_HILBERT decode fix
- needs `orig_sz` in invert stream entry
- currently imprecise (compact walk model)

### 3. Header frame (frame[0] = directory)
- special tile type = GPX5_TTYPE_DIRECTORY (new)
- stores: n_layers, layer_stride, codec_map[]
- enables: "scan frame[0] → know entire structure"

### 4. Threshold recalibrate
- FLAT=0 on real images → gate too strict
- lower HBC_THRESH_FLAT_X100 from 2000 → 500

---

## Sacred constants (FROZEN — never change)
```
GPX5_TICK_PERIOD  = 1440
GPX5_BLOCK_SZ     = 4896
GPX5_COMPOUNDS    = 144
HB_MAX_TILES      = 4096   (per cycle, not total)
HB_TILE_SZ_MAX    = 4896
HB_INVERT_ENTRY_SZ = 7
FSW_CLOCK_CYCLE   = 144
FSW_ENC_CYCLE     = 1440
```

## Include order (FROZEN)
```
gpx5_container.h
→ fibo_shell_walk.h (NEW this session)
→ hamburger_classify.h
→ hamburger_pipe.h
→ hamburger_encode.h
```
caller: include hamburger_encode.h only

## Test baseline
- 2 BMP files (1.7MB each) → pack → unpack → SHA256 MATCH ✅
- 3 files including temp.zip (4768 tiles) → 2 cycles → SHA256 MATCH ✅
- HbFiboLut: 8/8 PASS ✅
