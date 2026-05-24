# Diamond Shell Handoff — Session $(date)

## สถานะ
POC encoder พิสูจน์แล้ว ยังไม่มี decoder

## Files
- `diamond_shell_v2.h` — core pipeline (ใช้งานได้)
- `bench_shell_v2.c`   — benchmark

## Architecture
```
64B chunk → rotate64 (6 orientations) → DiamondBlock
         → fold_fibo_intersect() → pick best_rot (highest popcount)
         → classify: FLAT(2B) / SPARSE(9B) / DENSE(17B)
         → batch mode L1/L2/L3: group 8/64/512 chunks → 5B/chunk
```

## ผลที่ได้
- L0 single: 4–7x vs v2_old (1.9x)
- L2 batch:  ~12x vs v2_old
- rot_spread=6/6 = rotation scan ทำงานจริง
- dom_rot ต่างตาม data pattern (text→5, random→4, repetitive→3)

## Dependencies
- `pogls_fold.h` (DiamondBlock, fold_fibo_intersect, fold_xor_audit)
- `core/` folder จาก core.zip

## TODO ต่อไป (ตามลำดับ)
1. **decoder** — reverse rotation + reconstruct chunk จาก flag/seed/diff
2. **lossless verify** — encode→decode→memcmp ทุก chunk
3. **real files** — รัน bench กับไฟล์จริงแบบ bench_v2_realfiles.c
4. **image codec path** — feed XOR diff output เข้า PNG/WebP จริง
5. **atomic reshape hook** — wire pogls_atomic_reshape.h เป็น layer transition
