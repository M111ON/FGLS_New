# SID Runner — 6-Module Distributed Structure

## Modules
| Module | File | Responsibility | Depends On |
|--------|------|---------------|------------|
| A | `gguf_index.h` | Standalone GGUF v3 tensor index reader | — |
| B | `sid_loader.h` | Cache-aware tensor loader, norm bypass | A, C |
| C | `sid_cache.h` | TWFaceRewind-backed weight cache (256 entry, round-robin evict) | — |
| D | `llama_pogls_runner_sid.c` | Main runner: CLI, llama init, chat loop, load callback | A, B, C |
| E | `Makefile` | Build system for b9528 DLLs | — |
| F | `tests/*.c` | Tests: sid_cache, sid_loader, integration, benchmark | A, B, C |

## Dependencies
- llama-b9528-bin-win-vulkan-x64 DLLs
- collection/ headers (sid.h, tw_face_bridge.h)

## Build
```bash
make
./llama_pogls_runner_sid.exe model.gguf --twidx model.twidx --chat
```

## ⚠️ Operational Notes (July 2026)

### Build: `_fseeki64` on MinGW
MinGW ไม่ export `__imp__fseeki64` — linker error 53 ถ้าใช้ `_fseeki64`.
- `sid_loader.h` → ใช้ `fseeko64()` แทน
- `pogls_store.h` → ใช้ `pogls_fseek64` macro (`__MINGW32__` → `fseeko64`, else `_fseeki64`)

### Build: Always set stack size
ต้องใส่ `-Wl,--stack,16777216` ทุก rebuild
- ลืม → STATUS_STACK_OVERFLOW (0xC00000FD, exit -1073741571)
- single-step compile+link (`gcc -o exe source.c ...`) หลีกเลี่ยง MSYS2 gcc I/O bug (exit 53)

### Build: zstd.h include path
`kv_sid_evict.h` → `binary_shell_codec.h` → `#include <zstd.h>`
ต้องมี `-Icollection/Hfolder` ใน compiler flags

### Correct argument format (NOT -m, -p, -n)
| Wrong | Correct |
|-------|---------|
| `-m model.gguf` | `model.gguf` (positional) |
| `-p "Hello"` | `--prompt "Hello"` |
| `-n 16` | `--max-new 16` |
| `-m model --sid-face 1` | `model --sid-face 1 --prompt ...` |

ตัวอย่าง: `runner/llama_pogls_runner_sid_v2.exe I:\model\SmolLM2-360M-Instruct.Q8_0.gguf --prompt "Hello" --max-new 16 --ngl 0`

### --pogls-store: POGLS file ต้อง match model tensor count
`.pogls` มี tensor index ตามตอนสร้าง ถ้า model ต่างกัน (เช่น Qwen3-4B .pogls 394 tensors กับ SmolLM2-360M 290 tensors) → fallback to GGUF
สร้าง `.pogls` ใหม่ด้วย `runner/gguf_to_pogls.py <model.gguf> <out.pogls>`

### libllama.dll.a import lib (ถ้าต้อง rebuild)
```bash
gendef I:\FGLS_new\runner\libllama.dll
dlltool -d libllama.def -D libllama.dll -l libllama.dll.a
```

### Build command (full, verified July 1 2026)
```powershell
gcc -m64 -O2 -std=c11 -I. -Icollection -Icollection/src -Icollection/core -Icollection/core/core -Icollection/core/pogls_engine/core -Icollection/geopixel -Icollection/geopixel/Metatron/core -Icollection/pogls_engine -Icollection/geo_jump_module/include -Icollection/geopixel/hbv_bundle/Diamond_shell_encoder -Icollection/geopixel/hbv_bundle/Diamond_decode_hamburger -Icollection/geopixel/hbv_bundle/core -Icollection/Hfolder -Icollection/dgls/diamond/include -II:/llama.cpp/include -II:/llama.cpp/ggml/include -II:/llama.cpp/src -o runner/llama_pogls_runner_sid_v2.exe runner/llama_pogls_runner_sid_v2.c collection/geo_jump_module/src/geo_jump.c runner/kv_tensor_access.cpp -Lrunner -llibllama -lggml -lggml-base -lggml-cpu-x64 -lggml-vulkan -lstdc++ -lm runner/zstd.dll '-Wl,--stack,16777216'
```
