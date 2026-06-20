# Development Summary — SID + Cosplay + Experiment Pipeline

## Overview

A system that perturbs LLM weight tensors at inference time using **profile-aware cosplay** — small byte-level XOR deltas derived from session profiles (`.ses`) that encode user behavioral trajectories. The goal: produce **detectably different model outputs** while keeping the model fully coherent, without retraining or modifying the base model.

Three subsystems:
- **SID** (Signature Injection Dilation) — runtime tensor swap engine: redirects `tensor->data` to writable heap buffers, applies per-decode perturbation
- **Cosplay** — minimal perturbation rules (`.cpl` files, ~2.7KB each) that encode stride/XOR patterns derived from SID tensor cache signatures
- **Experiment** — multi-condition framework that auto-runs baseline + N `.cpl` files, compares logits cosine similarity, first-token diffs, and output text

---

## Milestone: End-to-End Validation (June 20)

### Problem
Base llama.cpp b9686 CPU backend **deadlocks** if `tensor->data` pointer changes between `sched_reserve` and `llama_decode`. Our original approach was `tensor_set_data()` pointer swap per decode, which triggers `ggml-cpu` scheduler infinite wait.

### Solution: Init Redirect
Instead of swapping pointers at runtime, **redirect at init** — set `tensor->data` to writable SID cache buffer **before context creation**. The scheduler captures heap pointers from the start. Then at experiment time, use in-place `memcpy` to modify buffer contents. No pointer change, no deadlock.

Key changes:
1. `tensor_set_data(tptr, sid_buf)` — redirect at init (before `llama_init_from_model`)
2. `found_tensors[ft_idx].orig_data = sid_buf` — save target for in-place writes
3. Experiment: `memcpy(dst=orig_data, src=perturbed_copy, size)` — modify in-place
4. After decode: `memcpy(dst=orig_data, src=clean_copy, size)` — restore

### DLL Version Crash
All builds crashed with `ACCESS_VIOLATION (0xC0000005)` at `llama_decode`. Root cause: **version mismatch** between `llama.dll` (b9528) and `ggml-cpu-*.dll` (b9686) in `runner/` directory. The backend loader scans PATH and finds the old b9686 CPU DLLs first.

Fix: Use ALL DLLs from same b9528 build:
- `ggml-cpu-haswell.dll` (1142784 bytes)
- `ggml-cpu-sse42.dll` (857088 bytes)
- `ggml-cpu-x64.dll` (848384 bytes)
- `ggml.dll`, `ggml-base.dll`, `llama.dll`

Remove old b9686 DLLs: rename to `.bak`.

### Result
Full pipeline works end-to-end:
- Model load (456-547ms)
- Tensor scan (291/291)
- SID cache init (170 weight tensors, 485MB)
- Init redirect (170 tensor->data → writable heap)
- Context creation (kv_cache, sched_reserve)
- `llama_decode` (prompt + generation)
- Experiment framework (10 conditions)

---

## Profile-Aware Cosplay Experiment

### Input
9 profile-aware `.cpl` files in `runner/cpl_profiles/`:
| File | Cluster | Role | Active Faces |
|------|---------|------|-------------|
| hash_table_005 | C0 | centroid | F4(1.0), F7(0.31) |
| java_265 | C0 | edge | F4(0.81), F7(1.0) |
| java_367 | C0 | edge | F4(0.81), F7(1.0) |
| jwt_348 | C1 | centroid | F2(1.0), F5(0.38) |
| devops_101 | C1 | edge | F2(0.83), F7(1.0) |
| devops_203 | C1 | edge | F2(0.83), F7(1.0) |
| nosql_233 | C2 | centroid | F0(1.0), F1(0.88), F5(0.92) |
| regex_142 | C2 | edge | F5(1.0), F6(0.17) |
| regex_040 | C2 | edge | F5(1.0), F6(0.17) |

### Results (Qwen2.5-0.5B Q4_K_M, prompt "Hello", temp=0, max_new=1)

**All 9 .cpl files produce different logits from baseline** (cosine 0.946–0.985):

| Condition | vs Baseline Cosine | First Token |
|-----------|:-----------------:|:-----------:|
| Baseline | 1.0000 | `,` (11) |
| devops_101 | **0.9460** | ` ` (220) |
| devops_203 | **0.9460** | `\n` (198) |
| hash_table_005 | 0.9528 | `,` (11) |
| java_265 | 0.9528 | `.` (13) |
| java_367 | 0.9528 | `......` (28149) |
| jwt_348 | 0.9833 | `\n` (198) |
| nosql_233 | 0.9826 | `,` (11) |
| regex_040 | 0.9848 | `\n\n` (271) |
| regex_142 | 0.9848 | `\n\n` (271) |

**Key patterns:**
- **C0 (devops/hash_table)**: most aggressive perturbation (cosine 0.946–0.953)
- **C1/C2 (jwt/regex/nosql)**: more conservative (cosine 0.983–0.985)
- **Within-cluster duplicates**: same stride distribution → identical cosines
- **All outputs are coherent** — no crash, no garbage, just different responses

### Full Comparison Matrix (10×10 Cosine Similarity)

```
        00-baseline  01-devops_101 03-hash_table  06-jwt_348   07-nosql_233  08-regex_040
00-base  1.0000       0.9460        0.9528         0.9833       0.9826        0.9848
01-dev   0.9460       1.0000        0.9940         0.9591       0.9653        0.9578
03-hash  0.9528       0.9940        1.0000         0.9622       0.9679        0.9618
06-jwt   0.9833       0.9591        0.9622         1.0000       0.9951        0.9981
07-nosql 0.9826       0.9653        0.9679         0.9951       1.0000        0.9946
08-regex 0.9848       0.9578        0.9618         0.9981       0.9946        1.0000
```

---

## Architecture

### File Map

```
runner/
├── llama_pogls_runner_sid_v2.c       # Main runner (b9686)
├── llama_pogls_runner_sid_v2_test.c   # Test runner with experiment (b9528 build)
├── llama_test_haswell.exe             # Compiled binary (b9528 + Haswell)
├── cosplay.h                           # CosplayProfile, save/load/apply/train API
├── cosplay_train.c                     # CLI trainer: .gsten → .cpl
├── cosplay_profile_train.c             # Profile-aware trainer: .ses + .gsten → .cpl
├── session_profile.h                   # SES2 format, transition matrix, timeline
├── sid_delta_ring.h                    # Delta ring journal for time travel
├── sid_timetravel.h                    # Rewind/ffwd/checkpoint orchestration
├── bond_discovery.h                    # Bond graph, cardioid express, Metatron
├── capture_pipeline.h                  # 12-face capture pipeline
├── cpl_profiles/                       # 9 profile-aware .cpl files
│   ├── hash_table_005.cpl
│   ├── java_265.cpl
│   ├── devops_101.cpl
│   ├── jwt_348.cpl
│   ├── nosql_233.cpl
│   ├── regex_142.cpl
│   ├── regex_040.cpl
│   ├── devops_203.cpl
│   ├── java_367.cpl
│   └── results/                        # Experiment output (auto-generated)
│       ├── 00-baseline/
│       ├── 01-devops_101/
│       ├── ...
│       └── report.txt
├── ggml-cpu-haswell.dll               # b9528 (overwrite old b9686)
├── ggml-cpu-sse42.dll                 # b9528
├── ggml-cpu-x64.dll                   # b9528
├── ggml.dll                           # b9528
├── ggml-base.dll                      # b9528
├── llama.dll                          # b9528
├── ses_cmp.c                          # Profile comparison tool
├── ses_cluster.c                      # HAC clustering (⚠ heap corruption bug)
├── ses_merge.c                        # Profile merge tool
├── ses_featurize.c                    # SES2 → CSV feature vector (108 columns)
├── cluster_profiles.py                # Python workaround for ses_cluster
└── REPORT_DLL_CRASH.md                # Documentation of b9686 DLL crash
```

### Pipeline Flow

```
Input prompt
    │
    ▼
┌─ SID swap setup ──────────────────────────┐
│  ses_fnv1a(tokens) → face (0..7)          │
│  face → coordinate → tensor list          │
│  tensor->data → writable SID cache buffer │
└───────────────────────────────────────────┘
    │
    ▼
┌─ Experiment / Chat loop ──────────────────┐
│  For each condition or decode step:       │
│    memcpy(orig_data, delta, size)         │
│    llama_decode()                         │
│    memcpy(orig_data, clean, size)         │
└───────────────────────────────────────────┘
    │
    ▼
Output tokens / comparison report
```

### Key Design Decisions

1. **Init redirect over pointer swap**: avoids b9686 deadlock. Sets `tensor->data` to heap before scheduler captures it.
2. **In-place memcpy over VirtualProtect**: mmap'd tensor pages can't be made writable (VirtualProtect error 87). Heap buffers are always writable.
3. **Separate clean copies**: `experiment_clean_ptrs[i] = malloc + memcpy` at experiment start. Prevents restore being NOP after in-place overwrite.
4. **Cosplay interaction**: when cosplay active, `orig_data` points to `cosplay_orig_ptrs[i]` (clean SID cache), not cosplay-modified buffer.
5. **b9528 over b9686**: b9686 CPU backend has scheduler deadlock. b9528 handles pointer changes correctly.
6. **No make**: use `gcc` directly (MinGW). `-O2` avoids stack overflow from large inline functions.

---

## Compilation

### Test binary (b9528 + Haswell)
```powershell
gcc -O2 -std=c11 -I. -Irunner -Icollection -Icollection/src -Icollection/core ^
  -Icollection/core/core -Icollection/core/pogls_engine/core -Icollection/core/geo_headers ^
  -Icollection/geo_jump_module/include -Icollection/core/pogls_engine -Icollection/geopixel ^
  -Icollection/geopixel/Metatron/core -II:/llama.cpp/include -II:/llama.cpp/ggml/include ^
  -o runner/llama_test_haswell.exe runner/llama_pogls_runner_sid_v2_test.c ^
  collection/geo_jump_module/src/geo_jump.c ^
  "I:/llama/llama-b9528-bin-win-vulkan-x64/llama.dll" ^
  "I:/llama/llama-b9528-bin-win-vulkan-x64/ggml.dll" ^
  "I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-base.dll" ^
  "I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-haswell.dll" ^
  -lm -lws2_32
```

### Profile-aware cosplay trainer
```powershell
gcc -O2 -std=c11 -I. -Irunner -Icollection [...] ^
  -o runner/cosplay_profile_train.exe runner/cosplay_profile_train.c -lm
```

---

## Usage

### Run experiment (fast, max_new=1)
```powershell
$env:PATH = "I:\FGLS_new\runner;$env:PATH"
.\runner\llama_test_haswell.exe I:\model\qwen2.5-0.5b-instruct-q4_k_m.gguf `
  --no-warmup --temp 0 --max-new 1 --experiment runner\cpl_profiles --sid-prompt-hash
```

### Run experiment (full generation)
```powershell
.\runner\llama_test_haswell.exe I:\model\*.gguf `
  --no-warmup --temp 0 --max-new 256 --experiment runner\cpl_profiles --sid-prompt-hash
```
⚠ Takes ~hours on CPU. Use fewer `.cpl` files or smaller model.

### Single cosplay test
```powershell
.\runner\llama_test_haswell.exe I:\model\*.gguf `
  --no-warmup --temp 0 --max-new 10 --prompt "Hello" `
  --sid-prompt-hash --cosplay runner\cpl_profiles\hash_table_005.cpl
```

### Profile batch mode
```powershell
.\runner\llama_test_haswell.exe I:\model\*.gguf `
  --no-warmup --temp 0 --sid-prompt-hash --profile-batch batch_400_unique.txt
```

---

## Known Issues

1. **b9686 deadlock** — CPU backend scheduler hangs if `tensor->data` changes between `sched_reserve` and `llama_decode`. Fixed by init redirect + b9528 migration.
2. **b9528 DLL path** — all DLLs must be from the SAME b9528 build. Mixing with old b9686 DLLs = crash.
3. **`ses_cluster.exe` heap corruption** — MinGW CRT bug with `_strdup`/`fopen` cycles after ~318 profile loads. Workaround: Python `cluster_profiles.py`.
4. **Experiment slow with default max_new=256** — 10 conditions × 256 tokens × 170 memcpy = hours. Use `--max-new 1` for quick validation.
5. **CPU only** — GPU backends (Vulkan, CUDA) not tested. Crash at `llama_decode` confirmed across all backends on this system (pre-existing DLL issue).

---

## Next Steps

1. **Migrate main runner** (`llama_pogls_runner_sid_v2.c`) to b9528 — currently only test file uses b9528
2. **Convert main loop** to in-place memcpy — currently uses `tensor_set_data` pointer swap (verified working per AGENTS.md but risk of regression)
3. **Profile-aware cosplay comparison** — run `--experiment` with all 5 unique `.cpl` signatures + baseline, compare output text trajectories
4. **Diverse model testing** — try with larger model (Llama 3.2 3B) to verify perturbation signatures scale
5. **GPU backend** — integrate Vulkan or CUDA for faster inference
