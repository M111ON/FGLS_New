# FGLS Colab Pack

Static Linux binaries for Colab — no compile, no install.

## Files

| File | Description | Standalone? |
|------|-------------|:---:|
| `geo_seek_bench` | Geometry seek benchmark (Linear/Hilbert/Morton/Random) | ✅ Yes |
| `fgls_linux` | FGLS Universal Codec (encode/decode/bench) | ✅ depends |
| `gguf_geo_benchmark` | GGUF inference: Array vs Engine vs Hash | Needs GGUF |
| `gguf_real_bench` | GGUF real tensor access pattern benchmark | Needs GGUF |
| `beam_hilbert_icosahedron` | Hilbert × Icosahedron (20736 grid) | ✅ Yes |
| `fgls_colab.ipynb` | Colab notebook — เปิดแล้วรันเลย | — |

## Usage on Colab

1. อัปโหลด zip นี้ไป Google Drive (หรือ `wget` จาก release)
2. เปิด `fgls_colab.ipynb` ใน Colab
3. รัน cell แรก (mount Drive + copy files)
4. รัน benchmark ที่ต้องการ

## Quick start (from scratch)

```python
# Upload binaries (or wget from raw GitHub)
!chmod +x geo_seek_bench fgls_linux gguf_* beam_hilbert_icosahedron

# Standalone — no model needed
!./geo_seek_bench
!./beam_hilbert_icosahedron

# FGLS codec test
!dd if=/dev/urandom of=test.bin bs=1024 count=64
!./fgls_linux bench test.bin
```

## Compile on WSL (Windows)

```bash
wsl -d Geomatt
cd /mnt/i/FGLS_new
gcc -static -O2 -Wall -o colab-pack/geo_seek_bench benchmark/geo_seek_bench_v5.c -lm -s
```
