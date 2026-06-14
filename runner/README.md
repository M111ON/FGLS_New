# SID Inference Runner

Coordinate-routed LLM inference using SID (Single Integrated Dimension).

## Architecture

```
gguf_index.h ──→ sid_loader.h ──→ llama_pogls_runner_sid.c
                    ↑                    │
               sid_cache.h          llama.cpp (b9528 DLLs)
```

| Module | File | Responsibility |
|--------|------|---------------|
| A | `gguf_index.h` | GGUF v3 parser: read tensor metadata (name, offset, size, dtype) |
| B | `sid_loader.h` | Cache-aware tensor loader: seek+read from GGUF, SIDCache integration |
| C | `sid_cache.h` | TWFaceRewind-backed weight cache: O(1) lookup by TRing position |
| D | `llama_pogls_runner_sid.c` | Main entry: CLI, llama init, chat loop, SID tensor callback |
| E | `Makefile` | Build with b9528 DLLs |
| F | `tests/` | Unit tests + integration test |

## Build

```bash
# Prerequisites: copy b9528 DLLs
copy I:\llama\llama-b9528-bin-win-vulkan-x64\*.dll C:\TPOGLS\

# Build all
make

# Build just runner
make runner
```

## Usage

```bash
# Chat mode
llama_pogls_runner_sid.exe model.gguf --chat

# With SID routing
llama_pogls_runner_sid.exe model.gguf --twidx model.twidx --chat

# Single prompt
llama_pogls_runner_sid.exe model.gguf --prompt "What is 2+2?" --max-new 20
```

## Options

| Flag | Default | Description |
|------|---------|-------------|
| `--twidx <path>` | none | SID `.twidx` coordinate index |
| `--ngl <N>` | 0 | GPU layers |
| `--temp <T>` | 0.7 | Sampling temperature |
| `--top-p <P>` | 0.9 | Top-p sampling |
| `--top-k <K>` | 40 | Top-k sampling (0=off) |
| `--repeat-penalty <P>` | 1.1 | Repeat penalty |
| `--max-new <N>` | 256 | Max generated tokens |
| `--cache <MB>` | 256 | SID weight cache pool size |
| `--prompt <str>` | none | Single prompt mode |
| `--chat` | false | Interactive chat mode |

## Tests

```bash
make tests
./test_sid_cache.exe     # cache hit/miss/evict
./test_sid_loader.exe    # GGUF tensor load verify
./test_integration.exe   # compare output vs official llama-cli
```

## Distributed Development

Each module is independent with clearly bounded interfaces:

1. **Module A** (`gguf_index.h`): standalone, no dependencies beyond std C
2. **Module B** (`sid_loader.h`): depends on A + C
3. **Module C** (`sid_cache.h`): standalone, simple buffer pool
4. **Module D** (`llama_pogls_runner_sid.c`): integrates A+B+C + llama.cpp
5. **Module F** (`tests/`): test each module independently

Work on modules A, C, and F can proceed in parallel.
