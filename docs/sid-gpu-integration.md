# SID + GPU Integration

## ปัญหา

SID (Session ID swap) กับ GPU offload (`--ngl N`) ไม่สามารถทำงานร่วมกันได้ เพราะ:

### 1. `--sid` flag หาย

มีแต่ `--sid-face N` ไม่มี `--sid` plain flag พอ user ส่ง `--sid` args parser ไม่ match เงื่อนไขไหนเลย (ไม่มี `else if(!strcmp(...))`) ทำให้ `sid_face = 0` ตลอด — SID ไม่ทำงาน

**Fix**: `llama_pogls_runner_sid_v2.c:688` — เพิ่ม
```c
else if(!strcmp(argv[i],"--sid"))sid_face=1;
```

### 2. GPU tensor `delta_orig_data` → ACCESS_VIOLATION

`found_tensors[i].orig_data` ชี้ไปที่ **GPU device memory** เมื่อ `--ngl > 0` (Vulkan buffer)

```
sid_swap_restore_ex()
  → ggml_backend_tensor_set(t, delta_orig_data[i], ...)
  → delta_orig_data[i] = found_tensors[fi].orig_data  ← GPU pointer!
  → memcpy จาก GPU address → ACCESS_VIOLATION
```

**Fix**: `llama_pogls_runner_sid_v2.c:1378` — ตรวจสอบว่า tensor อยู่บน GPU หรือไม่:
```c
struct ggml_tensor *_t = found_tensors[i].ptr;
int _gpu = _t && _t->buffer && !ggml_backend_buffer_is_host(_t->buffer);
```

ถ้า GPU tensor → ใช้ `cached` (CPU-readable, loaded from GGUF or preloaded) แทน `orig_data`
ถ้า CPU tensor → ใช้ `orig_data` เหมือนเดิม (zero-copy mmap)

### 3. DRamTile init failed เมื่อมี `--ngl`

VirtualAlloc ขนาด 5.1 GB ล้มเหลวหลังจาก GPU buffer allocation (น่าจะเพราะ memory fragmentation หรือ commit limit) → DRamTile fallback เป็น heap

```
[dramtile] init failed — falling back to heap
```

**Status**: **ยังไม่ fix** (pre-existing) — lazy GGUF fallback ใช้ได้ แต่เสีย benefit ของ DRamTile (CPU: -36%, warm cache: -23%)

## Architecture

```
llama_load_model_from_file()
  └─ GPU tensor → t->buffer = Vulkan buffer
  └─ t->data    → GPU device address
  └─ t->extra   = NULL (no cpu-readable pointer saved by llama.cpp)

SID init:
  └─ dt_store_init() → [fails with --ngl] → g_opt_dramtile = 0
  └─ sid_loader_open() → opens GGUF file handle
  └─ sid_cache_init() → allocates ~5.1 GB pool
  └─ lazy mode → ไม่ preload (opt_ngl > 0)

SID swap setup loop (for each tensor):
  └─ dt_get() → miss (DRamTile ไม่มี)
  └─ sid_cache_get() → miss (lazy mode ไม่มี)
  └─ GPU tensor → sid_loader_load() → reads from GGUF file
                  → puts into SID cache (compressed)
                  → sid_cache_get() → decompressed persistent copy
  └─ CPU tensor → zero-copy: orig_data (mmap)

SID swap apply:
  └─ tensor_update_data(t, sid_data, sz)
      └─ ถ้า GPU tensor → ggml_backend_tensor_set(t, data, 0, sz)
      └─ ถ้า CPU tensor → memcpy(t->data, data, sz)

SID swap restore:
  └─ tensor_update_data(t, delta_orig_data, sz)
      └─ delta_orig_data ต้องเป็น CPU-readable pointer เสมอ!
```

## Files ที่เกี่ยวข้อง

| File | Role |
|------|------|
| `runner/llama_pogls_runner_sid_v2.c` | Main runner — arg parsing, SID init, swap loop |
| `runner/sid_loader.h` | GGUF tensor loader — `sid_loader_open()` + `sid_loader_load()` |
| `runner/dramtile_store.h` | DRamTile zero-copy store — fallback ปัญหาเมื่อมี --ngl |
| `runner/sid_cache.h` | SID cache with compression — transparent decompress |

## Known Issues

1. **DRamTile + `--ngl`**: VirtualAlloc 5.1 GB fails after GPU alloc. **Workaround**: SID falls back to lazy GGUF load (ช้ากว่าแต่ทำงานได้). **Ideal fix**: ให้ DRamTile ใช้ GGUF offset-based reader แทนการ memcpy จาก tensor pointer (เหมือนที่ทำใน June 28 fix สำหรับ tensor load)

2. **`delta_orig_data` กับ `sid_data` ชี้ buffer เดียวกัน**: เมื่อไม่มี cosplay/corrupt ไม่มีปัญหา แต่ถ้ามี cosplay + `--ngl` ต้องแยก copy เพราะ `sid_data` อาจถูก modify

3. **Progressive swap ทุก decode**: ปัจจุบัน progressive cap (`g_sid_progress`) เพิ่มเฉพาะใน restore path — แต่ละ decode จะ restore แล้ว apply ใหม่พร้อม expand เป็น loop ตามธรรมชาติ

## วิธีทดสอบ

```powershell
# 8B + GPU (14 layers พอดีกับ VRAM 4GB)
.\llama_pogls_runner_sid_v2.exe model.gguf --ngl 14 --sid

# 8B + CPU
.\llama_pogls_runner_sid_v2.exe model.gguf --sid --dramtile

# 8B + CPU + DRamTile
.\llama_pogls_runner_sid_v2.exe model.gguf --sid --dramtile
```

Output ที่ควรเห็น:
```
[sid] weight tensors: 173, total data: 5146918912 bytes
[sid] GGUF open, lazy load for GPU tensors
[sid] 251 / 251 tensors will be swapped per decode
[sid] lazy progressive: start 50/251, +50 per decode
```

ไม่มี crash exit code 0
