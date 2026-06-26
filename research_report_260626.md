# POGLS Runner CLI Research Report — 26 June 2026

## 1. Basic Inference

### 1.1 Prompt mode (one-shot, non-interactive)

```bash
runner\llama_pogls_runner_sid_v2.exe model.gguf --prompt "Hello" --max-new 64 --temp 0
```

Flags: `--prompt TEXT`, `--max-new N`, `--temp N`, `--top-p N`, `--top-k N`

### 1.2 Chat mode (interactive)

```bash
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat
```

Commands while chatting:
```
/exit           ออกจาก chat
/clear          ลบประวัติ + สร้าง context ใหม่
```

### 1.3 Script mode (non-interactive chat)

```bash
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --script commands.txt --max-new 48 --temp 0
```

`commands.txt` example:
```
Hello
How are you?
/exit
```

### 1.4 GPU layers

```bash
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --ngl 8
```

`--ngl N` = จำนวน layer ที่ offload ไป GPU (default: 0 = CPU only)

---

## 2. SID Weight Swap

### 2.1 Basic SID (single face)

```bash
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --sid-face 1
```

เปิด SID weight swapping (สลับน้ำหนักไป alternate tensors ทุก decode step)

### 2.2 Layer filter

```bash
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --sid-face 1 --sid-spoke 0,4,8
```

`--sid-spoke N|all` = filter เฉพาะ layer ที่ระบุ (หรือ all = ทุก layer)

### 2.3 Tensor filter

```bash
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --sid-face 1 --sid-slot "attn_output"
```

`--sid-slot STR` = filter เฉพาะ tensor ที่ชื่อมี substring นี้

### 2.4 Corruption patterns

```bash
# XOR corruption: flip N bytes ด้วยค่า 0x01 (default)
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --sid-face 1 --sid-corrupt 256 --sid-byte 0xFF

# หรือใช้ --sid-pattern แทน
#   xor:N    = XOR N bytes ด้วยค่า specified (ต้อง --sid-byte)
#   set:N    = set N bytes to specified value
#   zero     = zero N bytes
#   rot:K    = rotate right every K bytes
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --sid-face 1 --sid-corrupt 512 --sid-pattern "xor:1" --sid-byte 0x42
```

### 2.5 Adaptive corruption

```bash
# Scale corruption ตาม tensor hotness
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --sid-face 1 --sid-adaptive --sid-corrupt 1024
```

### 2.6 Multi-layer injection

```bash
# Inject ที่ N hottest layers พร้อมกัน
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --sid-face 1 --sid-multi 3
```

### 2.7 Time-travel checkpoints

```bash
# ระหว่าง chat:
/checkpoint my_state     # บันทึก checkpoint
/rewind my_state         # ย้อนกลับ
/ff my_state             # fast-forward
/branch my_state         # branch จาก checkpoint
/tt                      # ดูสถานะ time travel ring
```

หรือใช้ flag ตอนเริ่ม:
```bash
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --sid-checkpoint my_state
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --sid-rewind my_state
```

### 2.8 Disable SID

```bash
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --sid-disable
# = inference ปกติ, ไม่มี weight swap
```

---

## 3. Geometry / Bond Discovery

### 3.1 Bond discovery

```bash
# ค้นหา topology bonds ระหว่าง tensors
runner\llama_pogls_runner_sid_v2.exe model.gguf --bond
```

### 3.2 Hex grid (แทนที่ --bond)

```bash
# Hex grid aperture-7
runner\llama_pogls_runner_sid_v2.exe model.gguf --hex
# level: 1=coarse 2=mid(default) 3=fine
runner\llama_pogls_runner_sid_v2.exe model.gguf --hex --hex-level 3
```

### 3.3 TriHex tessellation

```bash
# TriHex arena coordinates
runner\llama_pogls_runner_sid_v2.exe model.gguf --trihex
# level: 1=sector 2=hex(default) 3=trihex
runner\llama_pogls_runner_sid_v2.exe model.gguf --trihex --trihex-level 3
```

### 3.4 Goldberg sphere

```bash
# Goldberg spherical projection ของ trihex
runner\llama_pogls_runner_sid_v2.exe model.gguf --goldberg
# Geodesic threshold
runner\llama_pogls_runner_sid_v2.exe model.gguf --goldberg --goldberg-threshold 1.5
```

### 3.5 Geodesic expansion

```bash
# ขยาย SID swap ไปยัง goldberg geodesic neighbors
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --sid-face 1 --sid-geodesic --sid-geo-radius 0.5
```

### 3.6 Hex+Goldberg hybrid

```bash
# Hybrid: hot cluster + geodesic neighbors
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --sid-face 1 --hybrid --hybrid-radius 0.01
```

---

## 4. Capture Pipeline

```bash
# จับภาพ tensor geometry หลัง decode, เขียนลง DIR/
runner\llama_pogls_runner_sid_v2.exe model.gguf --capture output_dir

# บวกเวลา SID ด้วย
runner\llama_pogls_runner_sid_v2.exe model.gguf --capture output_dir --sid-face 1
```

Output: ไฟล์ข้อมูล geometry ของแต่ละ tensor ทุกครั้งหลัง decode

---

## 5. Gear Lock (Twin GPU)

```bash
# เปิด twin GPU bridge + gear lock feedback
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --twin-gpu --gear-lock

# Threshold (0..1, default 0.30, lower = more swaps)
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --twin-gpu --gear-lock --gear-lock-threshold 0.15

# Log interval cycles
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --twin-gpu --gear-lock --gear-log 32
```

ข้อจำกัด: ต้องมี CUDA twin bridge (icosa lane) — ถ้าไม่มี flag จะไม่มีผล

---

## 6. Cosplay Perturbation

### 6.1 Load cosplay profile

```bash
# โหลด .cpl profile เพื่อ perturb tensor weights
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --cosplay cosplay.qwen.cpl
```

`.cpl` files (ใน `bake_out/experiment/`):
| File | Description |
|------|-------------|
| `cosplay.qwen.cpl` | Cosplay profile สำหรับ Qwen |
| `same-cpl.cpl` | Control (same face) |
| `full-170.cpl` | Full 170 tensor perturbation |
| `full-170-stride64.cpl` | Full with stride 64 |
| `full-170-stride32.cpl` | Full with stride 32 |
| `25-attn-output.cpl` | Only attn_output layers |

### 6.2 Compare mode

```bash
# เปรียบเทียบ output WITH/WITHOUT cosplay
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --cosplay cosplay.qwen.cpl --cosplay-compare
```

### 6.3 Multi-condition experiment

```bash
# รันหลาย conditions อัตโนมัติ เขียนผลลัพธ์ไปยัง DIR/
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --experiment bake_out/experiment/
```

Experiment runs แต่ละ `.cpl` ใน directory และบันทึกผลเทียบกัน

### 6.4 Cosplay profile training

```bash
# Trainer utility (ต้องการ compile ก่อน)
gcc -O2 -std=c11 -I. -o cosplay_train.exe cosplay_train.c -lstdc++
.\cosplay_train.exe --input ses_profiles/ --output my_profile.cpl
```

---

## 7. KV State Perturbation

### 7.1 Basic KV swap

```bash
# Perturb N bytes in KV state ทุก decode
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --kv-swap 1024
```

### 7.2 Target specific layer

```bash
# -1 = ทุก layer, หรือ 0..N = layer ที่ระบุ
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --kv-swap 512 --kv-layer 5
```

---

## 8. KV SID Eviction (Zero-Copy)

```bash
# เปิด KV SID eviction
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --kv-evict 4
```

Chat commands (ต้อง compile ด้วย `-DKV_ARCHIVE`):
| Command | Action |
|---------|--------|
| `/evict N` | Swap N oldest layers → backup, poison original (model "ลืม") |
| `/restore` | คืน K/V กลับจาก backup ทั้งหมด |

การทำงาน: pointer swap — ไม่มีการ copy data, original ถูก poison ด้วย 0xDE/0xAD

---

## 9. KV Page Store

```bash
# เปิด KV Page Store (token-position-granular paging)
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --kv-page

# Evict 8 หน้าที่เก่าที่สุดตั้งแต่เริ่ม
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --kv-page --kv-page-evict 8
```

Chat commands:
| Command | Action |
|---------|--------|
| `/pevict N` | Evict N หน้าที่เก่าที่สุด |
| `/prestore PAGE_ID` | คืนค่าหน้าที่ระบุ |
| `/prestore all` | คืนค่าทุกหน้า |
| `/pstatus` | แสดงสถานะ page store |

---

## 10. KV Remap (Adaptive Skeleton+Delta)

```bash
# เปิด KV remap + rail background scan
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --remap
```

Chat commands:
| Command | Action |
|---------|--------|
| `/rstatus` | แสดงสถานะ skeleton + delta + rail |
| `/rscan` | บังคับ rescan rail ใหม่ |
| `/rrestore` | restore KV กลับไปที่ skeleton + delta |

ทำงานอัตโนมัติ:
1. **Rail scan** หลัง generation → ตรวจ % change ของ KV
2. **Classify**: 0-15%→ENTROPY, 15-85%→GEO, 85%+→REBUILD
3. **Store delta** (ถ้าไม่ rebuild) → compressed XOR/RLE
4. **Compress skeleton** ด้วย zstd

### ต้องการไฟล์
```
zstd.dll         — บีบอัด skeleton
kv_remap.h       — adaptive 3-tier system
kv_remap_rail.h  — 3-lane background scan
```

### Test suite

```bash
runner\test_kv_remap.exe
```

7 tests:
1. Classify at different change levels (0-100%)
2. Full cycle: store delta → restore → verify
3. Geo delta (50% change)
4. Rebuild at 90% change
5. Rail background scan
6. Rail freeze/resume
7. Multi-turn streaming simulation

---

## 11. DRamTile (Zero-Copy Tensor Store)

```bash
runner\llama_pogls_runner_sid_v2.exe model.gguf --dramtile
```

เปิด DRamTile — zero-copy tensor store backend

---

## 12. Session Profiles

### 12.1 Batch generate profiles

```bash
# สร้าง session profiles (.ses) จำนวนมาก
runner\llama_pogls_runner_sid_v2.exe model.gguf --profile-batch bake_out/prompts.txt
```

### 12.2 Profile utilities

```bash
# ต้องการ compile ก่อน:
gcc -O2 -std=c11 -I. -o ses_cmp.exe ses_cmp.c -lstdc++
gcc -O2 -std=c11 -I. -o ses_cluster.exe ses_cluster.c -lstdc++
gcc -O2 -std=c11 -I. -o ses_featurize.exe ses_featurize.c -lstdc++
gcc -O2 -std=c11 -I. -o ses_merge.exe ses_merge.c -lstdc++

# เปรียบเทียบ profiles
ses_cmp.exe profile1.ses profile2.ses

# จัดกลุ่ม profiles (k=3)
ses_cluster.exe --input ses_profiles/ --k 3

# Extract features
ses_featurize.exe --input ses_profiles/ --output features.bin

# Merge profiles
ses_merge.exe --input ses_profiles/ --output merged.ses
```

### 12.3 Profile-aware cosplay trainer

```bash
# Train cosplay profile จาก session profiles
gcc -O2 -std=c11 -I. -o cosplay_profile_train.exe cosplay_profile_train.c -lstdc++
.\cosplay_profile_train.exe --profiles ses_profiles/ --output profile.cpl
```

### 12.4 Existing profiles

`.ses` files (ใน `bake_out/`):
| File | Description |
|------|-------------|
| `hello.ses` | Simple hello prompt |
| `hello_all.ses` | Hello + all variants |
| `hello_ph.ses` | Hello phase-based |
| `hello_prompt.ses` | Hello structured |
| `prompt_hello.ses` | Hello prompt variant |
| `prompt_ml.ses` | ML prompt |
| `prompt_poem.ses` | Poem prompt |
| `ml.ses` | ML conversation |
| `multi_turn.ses` | Multi-turn conversation |
| `poem_ph.ses` | Poem phase-based |

---

## 13. Memory Timeline / Logging

```bash
# บันทึก SID tensor memory timeline
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --sid-face 1 --mem-store timeline.log
```

---

## 14. Utility Flags

```bash
# นับเฉพาะจำนวน tensors ใน model แล้วจบ
runner\llama_pogls_runner_sid_v2.exe model.gguf --count-only

# Simulation mode (ไม่ต้องใช้ model จริง)
runner\llama_pogls_runner_sid_v2.exe model.gguf --simulate --chat

# Context size (default: 2048)
runner\llama_pogls_runner_sid_v2.exe model.gguf --chat --ctx 4096
```

---

## 15. Combined Examples

### Full pipeline: SID + Cosplay + KV Remap + KV Page

```bash
runner\llama_pogls_runner_sid_v2.exe model.gguf ^
  --chat ^
  --sid-face 1 --sid-spoke all ^
  --cosplay cosplay.qwen.cpl ^
  --remap ^
  --kv-page --kv-page-evict 4 ^
  --ctx 4096 --max-new 128 --temp 0.7
```

### Experiment: SID + Twin GPU + Gear Lock

```bash
runner\llama_pogls_runner_sid_v2.exe model.gguf ^
  --chat ^
  --sid-face 1 --sid-adaptive --sid-corrupt 512 ^
  --twin-gpu --gear-lock --gear-lock-threshold 0.25 ^
  --mem-store gear_timeline.log
```

### Geometry research: Bond + Hex + TriHex + Goldberg

```bash
runner\llama_pogls_runner_sid_v2.exe model.gguf ^
  --hex --hex-level 3 ^
  --trihex --trihex-level 3 ^
  --goldberg --goldberg-threshold 1.0
```

---

## 16. DLL Dependencies (ใน `runner/`)

```
llama_b9733.dll          — main llama.cpp v2 (b9733)
ggml_b9733.dll           — GGML core (b9733)
ggml.dll                 — GGML (current)
ggml-base.dll            — Base GGML
ggml-cpu.dll             — CPU backend (auto-detect)
ggml-cpu-x64.dll         — x84-64 CPU
ggml-vulkan.dll          — Vulkan GPU backend
zstd.dll                 — Zstd compression (KV Remap)
libllama.dll             — llama library
mtmd.dll                 — Multi-token/multi-decode
```

เลือก CPU backend ตาม architecture:
- `ggml-cpu-haswell.dll` (Haswell)
- `ggml-cpu-icelake.dll` (Ice Lake)
- `ggml-cpu-cascadelake.dll` (Cascade Lake)
- `ggml-cpu-zen4.dll` (AMD Zen 4)
- etc.
