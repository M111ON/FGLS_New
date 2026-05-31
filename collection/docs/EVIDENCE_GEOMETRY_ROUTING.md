# Geometry Routing Proof Report

This document records the working proof that the repo can route a request by geometry, resolve the target model/store pair, and execute the model end-to-end.

## Goal

Prove that `zone + shape + ns` can be turned into a real runtime decision, then used to launch a model against the matching geometry store.

## What We Built Before The Proof

### 1. Coordinate resolution

The runner gained coord-based launch paths:

- `--coord-resolve` for registry lookup only
- `--coord` for lookup + store open + model run
- `--ask` for prompt-first coord launch

Implementation entry point: `core/llama_pogls_runner.c` (`main()`, coord handlers, and `run_coord_mode()`).

### 2. Geometry-backed backend

`core/llama_pogls_backend.c` parses GGUF tensor metadata into `ModelIndex`, then streams exact layer windows from the file. That gives a real file-backed path for routed layer execution.

### 3. Warm-up reduction

Before the final proof, the runner warm-up cost was reduced in two places:

- model load uses mmap-backed GGUF loading
- chat context was reduced from `n_ctx=2048` to `n_ctx=1024`
- batch settings were reduced to `n_batch=512` and `n_ubatch=128`

That cut the observed scheduler reserve time in the chat path from about `263 ms` to about `94 ms` on the same model class.

### 4. Windows compatibility cleanup

Local core files used by the runner were cleaned so the path compiles and runs on this machine:

- `core/core/angular_mapper_v36.c`
- `core/core/pogls_qrpn_phaseE.h`
- `core/core/pogls_fold.c`
- `core/core/entangle_stub2.c`

## Proof Evidence

### A. Coord resolution works

Command:

```powershell
.\pogls_runner.exe --coord-resolve ..\coord_real_registry.json 2 S
```

Observed output:

```text
model_key=qwen
gguf_path=I:/Vault/models/Qwen3-0.6B-Q8_0.gguf
store_path=I:/FGLS_new/collection/build/qwen_geom_v3
force_cpu=1
```

This proves the coord registry maps a geometry key to a real model and store path.

### B. Geometry-backed backend run works

Command:

```powershell
.\pogls_runner.exe --coord ..\coord_real_registry.json 2 S 0
```

Observed output:

```text
[coord] store_ready=1 entries=84
[runner] 310 layers, max layer = 161432 KB
[runner] stream done: 2141 ms
[backend] layers_read=310 skipped=0 bytes_read=633495520 peak_layer=165306368
```

This proves the runner can:

- resolve coord to a real model/store pair
- open the geometry store
- stream the model layers
- complete execution successfully

### C. Interactive chat works

Command:

```powershell
@'
hello
exit
'@ | .\pogls_runner.exe I:\Vault\models\Qwen3-0.6B-Q8_0.gguf --chat
```

Observed result:

```text
[chat] ready. Type 'exit' or 'quit' to stop.
[stream] ... response text ...
```

This proves the runner is not just resolving paths. It creates a llama context, streams tokens, and responds interactively.

## Warm-Up Benchmark

### Before tuning

- `n_ctx=2048`
- observed scheduler reserve: about `263.38 ms`

### After tuning

- `n_ctx=1024`
- `n_batch=512`
- `n_ubatch=128`
- observed scheduler reserve: about `94.29 ms`

### Runner-side impact

- smaller context footprint
- lower reserve cost
- faster first-response warm-up
- still stable enough for chat and coord launch

## File References

- `core/llama_pogls_runner.c`
  - coord routing, chat loop, warm-up settings
- `core/llama_pogls_backend.c`
  - GGUF tensor table parsing and layer streaming
- `core/core/angular_mapper_v36.c`
  - geometry runtime compatibility fixes
- `core/core/pogls_qrpn_phaseE.h`
  - stats formatting and witness cleanup
- `core/Makefile`
  - runner build recipe

## What This Means

This is the proof that the repo can already do the important part:

- structure a system around geometry keys
- match those keys to weights and stores
- launch and run a real model from that structure
- keep the path working on Windows

It does **not** prove a llama-free inference backend. That is a separate future project.

## Conclusion

The requested milestone is achieved: the system can be structured to match geometry keys to real weights, resolve them deterministically, and execute interactively. The warm-up path was also improved, and the proof is backed by observed runs and timings, not just design intent.
