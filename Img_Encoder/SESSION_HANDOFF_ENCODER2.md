# GEOPIXEL ENCODER — Session Handoff

## สถานะล่าสุด
- **GEO3 encoder/decoder ทำงานได้ครบ**
- decode แบบ sorted raster order ดีกว่า spatial prediction (sp มี horizontal artifact)

## ไฟล์หลัก (ใน /home/claude หรือ rebuild จาก source)
| ไฟล์ | หน้าที่ |
|------|---------|
| `geopixel_v3.c` | encoder หลัก — compile: `gcc -O2 -o geopixel_v3 geopixel_v3.c -lm -lzstd` |
| `geo3_decode.c` | decoder (sorted raster) — compile: `gcc -O2 -o geo3_decode geo3_decode.c -lzstd` |
| `geo3_decode_sp.c` | decoder spatial prediction — ❌ ผลแย่กว่า อย่าใช้ |

## Format GEO3
```
[4B magic=GEO3] [4B w] [4B h]
[4B bmap_orig] [4B bmap_zst]
[4B ne_orig]   [4B ne_zst]
[4B dpos_orig] [4B dpos_zst]
[4B cnt_orig]  [4B cnt_zst]
[bmap zstd] [ne zstd] [dpos zstd] [cnt zstd]
```

## Stream encoding
- **bitmap**: 1 bit per tile (active/empty)
- **n_entries** (ne): u16 per tile = จำนวน unique axis positions
- **delta-pos** (dpos): varint — delta=1→`0x01`, delta=2→`0x02`, else `0x00`+u16
- **count** (cnt): varint — <128→1B, ≥128→`0x80|hi`+lo
- ทุก stream zstd-19

## Axis model
- 1D axis 0..767: R=0..255, G=256..511, B=512..767
- per tile 16×16: เก็บ histogram ของ axis positions
- decode: sorted pos → assign pixels raster order (histogram-only, ไม่ pixel-perfect)

## Benchmark ผลล่าสุด (merge_thresh=2)
| ภาพ | Raw | GEO3 | Ratio | PSNR |
|-----|-----|------|-------|------|
| test02 (holographic 1536²) | 6.75 MB | 411 KB | **16.83x** | 23.03 dB |
| preview (knight 1092²) | 3.41 MB | 364 KB | **9.62x** | 15.23 dB |

merge=0 lossless: 13.04x / 5.86x (Histogram PASS)

## Usage
```bash
./geopixel_v3 input.bmp [merge_thresh]   # 0=lossless, 1,2=lossy
./geo3_decode input.bmp.geo3 [out.bmp]
```

## Open issues / next steps
1. **preview ratio ต่ำกว่า test02** เพราะ avg 207 entries/tile (detail สูง) vs 81/tile
2. **count stream ยังใหญ่** (276-372KB หลัง zstd) — rice code / log-scale จะลดได้อีก
3. **tile size** ลองเป็น 8×8 อาจช่วย preview ได้ (fewer entries/tile)
4. **spatial prediction decode ❌** — horizontal artifact เห็นชัด, sorted order ดีกว่า
5. **export pipeline**: webp ผ่าน imagemagick ใช้ได้, bmp loader รองรับ BGR flip

## Dependencies
```bash
apt-get install libzstd-dev imagemagick
gcc -O2 ... -lm -lzstd
```
