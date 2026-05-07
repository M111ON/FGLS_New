# GeoPixel v18 — Session Handoff O7+O8+O9
> Noise benchmark + Standalone decoder + Grid visualizer

---

## สถานะปัจจุบัน

### ✅ O7 — Noise Benchmark
วัด GPX compression ratio กับ 6 image types (256×256)

| pattern | gp15 (B) | gpx (B) | gpx/gp15 |
|---------|----------|---------|----------|
| gradient | 13,702 | 2,601 | **0.19** ← ดีสุด |
| checker | 808 | 738 | 0.91 |
| stripes | 814 | 752 | 0.92 |
| solid | 562 | 534 | 0.95 |
| smooth | 109,897 | 108,813 | 0.99 |
| noise | 250,516 | 252,564 | **1.008** ← แย่กว่า gp15 |

**สรุป:** `.gpx` เหมาะกับ spatially-coherent gradient image เท่านั้น
- noise = gpx ใหญ่กว่า gp15 (overhead ชนะ)
- geo geometry เป็น natural basis ของ gradient → compress ดีมาก
- เหมือน wavelet/DCT: codec ดีเมื่อ basis ตรงกับ signal

**Script:** `gen_bmp.py` (Python, standalone) — สร้าง BMP ทุก pattern + run benchmark loop

---

### ✅ O8 — Standalone `geo_gpx_decode.h`
Header-only GPX decoder ไม่ dependency บน `geopixel_v18.c`

**Public API:**
```c
#define GEO_GPX_DECODE_IMPL
#include "geo_gpx_decode.h"

int gpx_decode_to_bmp(const char *gpx, const char *bmp_out);  // → file
int gpx_decode_to_rgb(const char *gpx, uint8_t **px, int *w, int *h);  // → buffer (caller free)
```

**Dependencies:** `geo_o4_connector.h` + libpng + libzstd (ไม่ต้องการ headers อื่น)

**Verification:** pixel diff = 0 ทุก 5 patterns vs `geopixel_v18` reference ✓

**Bug ที่เจอระหว่าง port:**
- BMP row order: `bmp_save` ใน v18 = bottom-up (`y=H-1→0`) + positive H ใน DIB
- standalone เขียน top-down → fix แล้ว

**Build:**
```bash
gcc -O2 -I. -o myapp myapp.c -lzstd -lpng
# ไม่ต้อง -lm -lpthread (ตัดออกแล้ว)
```

---

### ✅ O9 — Grid Visualizer
`geo_gpx_visualizer.html` — single-file, drag-and-drop `.gpx` โดยตรงใน browser

**4 views:**
| View | Content |
|------|---------|
| Decoded | tile grid + avg geo color + blob_sz label per tile |
| GeoPixel Grid | raw O4 PNG (27×H) + tile row dividers |
| Field Map | trit/spoke/fibo/coset — toggle ได้ 4 modes |
| Tile Overlay | entropy heatmap (blob_sz → cool/warm color) |

**Controls:** zoom ×1–8, grid toggle, view toggle, field mode selector
**Hover:** tooltip แสดง tile info (idx, blob_sz, grid_rows, entropy rank)

**Insight จาก Field Map:** fibo mode แสดง stripe repeat ทุก 144 rows → ยืนยัน `GP_FIBO_MOD=144` ด้วยตา

---

## Files ปัจจุบัน

```
geopixel_v18.c         ← main encoder/decoder (ไม่แก้ตั้งแต่ O6)
geo_o4_connector.h     ← O4 grid bridge (frozen)
geo_tring_addr.h       ← TRing address (frozen)
geo_goldberg_tile.h    ← Goldberg tile (frozen)
geo_pixel.h            ← GeoPixel encode/decode (frozen)
geo_gpx_decode.h       ← NEW O8: standalone decoder
gen_bmp.py             ← NEW O7: benchmark script
geo_gpx_visualizer.html← NEW O9: browser visualizer
```

**Build command (unchanged):**
```bash
gcc -O3 -I. -o geopixel_v18 geopixel_v18.c -lm -lzstd -lpthread -lpng
./geopixel_v18 input.bmp          # encode → .gp15 + .gpx
./geopixel_v18 file.gp15          # decode gp15 → .bmp
./geopixel_v18 file.gpx out.bmp   # decode gpx → out.bmp
```

---

## Sacred Numbers (DO NOT CHANGE)
```
27 = 3³      O4_GRID_W
6912         O4_MAX_SLOTS = TRING_TOTAL
3            O4_CHUNK_BYTES per GeoPixel slot
144          GGT_FLUSH_PERIOD / GP_FIBO_MOD
6            GPX tile table entry = 2B(rows) + 4B(blob_sz)
16           GPX base header bytes
32           TILE size (pixels)
```

---

## Open Items สำหรับ O10+

### O10 — Ttype overlay ใน visualizer
อ่าน ttype byte แรกของ blob จาก tile table → แสดงสีต่างกัน per tile
(flat=green / gradient=blue / edge=yellow / noise=red)
ต้องการ: parse blob bytes ใน browser JS (ง่าย — แค่ byte แรกของ blob segment)

### O11 — Streaming GPX decoder
chunk-by-chunk โดยไม่ต้อง load ทั้ง file ใน RAM
เป้า: embedded / low-memory target
แนว: tile-at-a-time API พร้อม callback

### O12 — Format comparison CLI
gp15 vs gpx stats side-by-side (tile breakdown, entropy, timing)
อาจ merge เข้า geopixel_v18 main dispatch

### Architecture note
O8 decoder ใช้ `fmemopen` → non-portable บน MSVC/Windows
ถ้าต้องการ portability: replace ด้วย custom PNG read callback (`png_set_read_fn`)
