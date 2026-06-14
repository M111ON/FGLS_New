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
