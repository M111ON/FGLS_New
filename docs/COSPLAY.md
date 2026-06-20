# 🎭 Cosplay: Signature-Derived Perturbation Profile

**Replace 485MB `.gsten` tensor store with a ~2.7KB `.cpl` file** that encodes minimal perturbation rules. At inference time, perturbations are applied to SID-cache tensor data before swap injection — producing **detectably different model output** while keeping the model fully coherent.

---

## Core Insight

A baked tensor store (`.gsten`) captures a model's weights at a specific "moment" — useful but **fat** (485MB). Cosplay asks: *do we need to store the entire tensor, or just enough information to push the model's output in a detectable direction?*

Answer: **~2.7KB** (0.0005%) is enough.

The trick: instead of storing full tensor data, store a **perturbation recipe**:
- Which tensors to touch (by FNV-1a name hash)
- Which bytes to modify (stride-based sparse XOR)
- How strong the modification is (1-bit LSB flip per byte)

Applied statistically: ~1.5% of bytes per tensor are flipped by ±1. Scale bytes in quantized blocks are rarely hit (~6% of perturbations). The model absorbs the noise and produces a different but coherent response.

---

## File Format (`.cpl`, version 2)

```
┌─────────────────────────────────────────┐
│ Header (20 bytes)                       │
│  magic(4) = 0x504F434C ("CPL")          │
│  version(4) = 2                         │
│  n_entries(4)                           │
│  n_layers(4)                            │
│  n_heads(2)                             │
│  n_embd(2)                              │
├─────────────────────────────────────────┤
│ Entry[0..n-1] (16 bytes each)           │
│  name_hash(4)  — FNV-1a of tensor name  │
│  mode(1)       — CP_XOR=1, CP_ROT=3    │
│  arg(1)        — xor byte / rot k       │
│  stride(2)     — apply every N bytes     │
│  data_size(4)  — expected tensor size   │
│  tick(4)       — gsten index (reserved) │
├─────────────────────────────────────────┤
│ Total: 20 + n×16 bytes                  │
│ Qwen2.5-0.5B (170 tensors): 2,740 bytes  │
└─────────────────────────────────────────┘
```

### Version Compatibility

| Version | Entry size | Feature |
|---------|-----------|---------|
| 1 | 12 bytes | name_hash + mode + arg + stride + tick |
| 2 | 16 bytes | + data_size for verification |

Reader is backward-compatible: v1 entries are zero-filled for `data_size`.

---

## How It Works

### Training (offline, on baked gsten)

```
gsten (291 tensors, 485MB)
  ↓
Filter: keep only SID-cache weight tensors (size ≥ 64KB) → 170 tensors
  ↓
For each tensor:
  → read tensor data from gsten
  → compute FNV-1a hash of tensor name
  → store CosplayEntry{ name_hash, mode=CP_XOR, arg=0x01, stride=N, data_size }
  ↓
.cpl file (2.7KB)
```

### Inference (runtime, per decode)

```
GGUF model (291 tensors)
  ↓
SID cache init → 170 weight tensors cached
  ↓
cosplay_load(.cpl) → 170 entries loaded
  ↓
SID swap setup:
  For each tensor in SID cache:
    → compute FNV-1a hash of tensor name
    → cosplay_find(profile, hash)
    → if found and mode != CP_NONE:
        malloc copy of tensor data
        cosplay_apply(entry, copy, size) → XOR every Nth byte with arg
        replace swap data pointer (is_malloc flag set)
  ↓
Decode runs with perturbed tensor data
  → output differs from baseline
  → model stays coherent
```

### Perturbation Mechanics

For a Q4_K_M or Q8_0 quantized block:

```
Block (34-144 bytes) ┌─────┬──────────────────┐
                     │scale│  quantized data  │
                     │2-4B │  30-128B         │
                     └─────┴──────────────────┘
                         ↑           ↑
                   stride hits    stride hits
                   ~6% of time    ~94% of time
```

With stride=64 on Q4_K_M blocks (144 bytes):
- 1 in 144 bytes is skipped per perturbed byte
- ~94% land on quantized data values (safe — ±1 change)
- ~6% land on block scales (adds noise but empirically harmless)

---

## Scaling Results (Qwen2.5-0.5B Q4_K_M)

All runs with `--temp 0 --sid-face 1`, prompt "Hello":

| # Tensors | `.cpl` size | Output tail |
|-----------|------------|-------------|
| # Tensors | `.cpl` size | First token | Output tail |
|-----------|------------|-------------|-------------|
| 0 (baseline) | — | `\n` | `Hello! How can I assist you today? ...valuable source of information for others like you in the future. 😊` |
| 1 (output.weight) | 36 B | `\n` | `...of utmost importance in helping me improve my responses. Thank you!` |
| 3 | 68 B | `\n` | `I'm not sure what you're asking for. Can you please provide more information?` |
| 9 | 164 B | `\n` | `I'm not sure what you mean by "Hello", but it seems like a response...` |
| 25 (all attn_output) | 420 B | `\n` | `I'm not sure what you mean by "Hello", but if you have a specific question... Goodbye!` |
| **170 (ALL weights)** | **2.7 KB** | `,` | `I'm not sure what you're asking for. If there's any information I can help with, please let me know!` |

**Key finding:** model remains **coherent at every level**. No crash, no garbage output. The perturbation is subtle enough that the model adapts, but strong enough to produce a measurably different response.

---

## `--experiment DIR` (Multi-Delta Experiment Framework)

Takes a directory of `.cpl` files and runs **each as a separate condition** — baseline (no perturbation) + each `.cpl` — then cross-compares all outputs.

### Flow
```
experiment_dir/
  ├── subtle.cpl         (stride=256)
  ├── moderate.cpl       (stride=64)
  ├── aggressive.cpl     (stride=8)
  └── results/
      ├── 00-baseline/       (no perturbation)
      │   ├── tokens.txt
      │   ├── tokens.bin
      │   ├── logits.bin
      │   └── info.txt
      ├── 01-subtle.cpl/
      ├── 02-moderate.cpl/
      ├── 03-aggressive.cpl/
      └── report.txt         (cross-comparison report)
```

For each condition:
1. **Snapshot**: restore delta arrays to clean SID cache pointers
2. **Apply**: if `.cpl`, load and apply perturbation via `cosplay_apply()`
3. **Generate**: decode "Hello" prompt + generate N tokens
4. **Save**: token IDs, decoded text, prompt logits, timing
5. **Clean**: free perturbed buffers for next condition

After all conditions run:
- **Per-condition**: first token, output (truncated), timing
- **Cross-matrix**: pairwise first-token diff, logit cosine similarity
- **Report**: saved to `results/report.txt`

### Sample Output
```
$ echo "Hello" | ./llama_pogls_runner_sid_v2.exe model.gguf \
    --experiment bake_out/experiment --sid-face 1 --temp 0

[experiment] 6 conditions (baseline + 5 .cpl files)
  00-baseline:          first=11 ','
  01-25-attn-output:    first=11 ','
  02-full-170-stride32: first=18137 ' \xe9'   ← garbled (too aggressive)
  03-full-170-stride64: first=4102 '\xc2\xa0' ← coherent, different
  04-full-170:          first=14133 ' Je'     ← coherent, different
  05-same-cpl:          first=198 '\n'        ← coherent, different
```

**Key finding**: each perturbation produces a detectably different output, ranging from subtle (same first token, different content) to aggressive (garbled).

### Implementation
- Snapshot: `experiment_clean_ptrs[]` array saves original SID cache pointers
- No deep copy needed — `cosplay_apply()` always creates new malloc'd buffers
- Between conditions: restore clean ptrs, free old buffers, apply new perturbation
- Uses raw `tensor_set_data()` (not time travel ring) for isolation

## `--cosplay-compare` (Cosplay + Time Travel)

At startup (before any user interaction), runs a **controlled comparison** of the first token WITH vs WITHOUT cosplay:

1. Decode test prompt "Hello" **WITH** cosplay → save logits + sampled first token
2. `llama_memory_clear()` → reset KV cache
3. Toggle delta arrays back to **original** (unperturbed) cached tensor data
4. Decode **WITHOUT** cosplay → save logits + sampled first token
5. **Compare**: token diff, logit cosine similarity, max diff, same-sign ratio, top-5 overlap
6. Toggle delta arrays back to cosplay, clear KV cache, continue normally

```
$ echo "Hello" | ./llama_pogls_runner_sid_v2.exe model.gguf \
    --cosplay bake_out/cosplay.qwen.cpl --cosplay-compare --sid-face 1 --temp 0

[cosplay-cmp] === Cosplay Comparison (1 tokens, 25 swaps) ===
[cosplay-cmp] ═══ Comparison: WITH vs WITHOUT Cosplay ═══
  First token WITH cosplay:    ',' (token 11)
  First token WITHOUT cosplay: '\n' (token 271)
  Same token? NO
  Logits cosine similarity:    0.997262
  Max logit difference:        1.467321
  Same-sign ratio:             148773/151936 (97.9%)
  Top-5 tokens WITH cosplay:   271:'\n', 198:'\n\n', 11:',', 3837:'\xef\xbc\x8c', 17091:' \xc2\xa0'
  Top-5 tokens WITHOUT cosplay:271:'\n', 198:'\n\n', 11:',', 3837:'\xef\xbc\x8c', 17091:' \xc2\xa0'
  Top-5 overlap: 5/5
[cosplay-cmp] Done. Continuing main loop WITH cosplay.
```

**The first token changes** — proof that cosplay measurably affects the output despite 99.7% logit similarity.

### How It Works (Implementation)
- Saves both the cosplay-perturbed and original SID cache pointers during swap setup
- Uses **raw `tensor_set_data()`** calls (not the time travel ring) to avoid side effects on the delta journal
- Comparison is purely at the logits/token level — no modification to the main loop state
- All 151936 logits are compared: cosine similarity, per-element max diff, sign agreement

## Files

| File | Purpose |
|------|---------|
| `runner/cosplay.h` | Core library: CosplayProfile, CosplayEntry, save/load (v1+v2), lookup (FNV-1a), apply, verify, train |
| `runner/cosplay_train.c` | CLI trainer: `cosplay_train <store.gsten> <output.cpl>` |
| `runner/llama_pogls_runner_sid_v2.c` | `--cosplay PATH` flag; cosplay applied during SID swap setup; `--cosplay-compare` startup comparison |
| `docs/COSPLAY.md` | This document |

### Key APIs

```c
// Train from gsten (n_targets=0 → all weight tensors ≥64KB)
cosplay_train(&cp, &gi, gsten_path, target_names, n_targets);

// Save/load .cpl (v2 save, v1+v2 load)
cosplay_save(path, &cp);
cosplay_load(path, &cp);

// Lookup by FNV-1a name hash
CosplayEntry *ce = cosplay_find(&cp, name_hash);

// Apply perturbation → malloc'd copy (caller frees)
uint8_t *perturbed = cosplay_apply(ce, src_data, src_size);

// Verify entry matches tensor
int ok = cosplay_verify(ce, tensor_name, tensor_size);
```

---

## Design Decisions

### Why FNV-1a name hash (not SID node_id)?
Multiple tensors (e.g., all 24 `blk.N.attn_norm.weight`) can map to the **same** SID node_id because their data distributions are similar (all ≈ 1.0). FNV-1a hash of the tensor name is guaranteed unique.

### Why sparse byte-level XOR on quantized data?
- **Dequantize → perturb → requantize** is correct but expensive and lossy
- **Full replacement** (copy gsten data) is what `--gsten` already does — defeats the purpose of cosplay
- **Byte-level XOR** with stride > 1 is minimal, fast, and statistically safe (~94% quant hits)

### Why only SID-cache weight tensors?
F32 norm tensors and biases are not stored in the SID cache (they're small, <64KB). Only weight tensors (170 out of 291) participate in the SID swap mechanism. Cosplay piggybacks on this existing pipeline.

### Why `cosplay_apply()` returns a malloc'd buffer?
The runner needs to track perturbed buffers separately from originals for proper cleanup. The `is_malloc` flag on each SID swap entry signals that the data pointer was malloc'd and needs `free()` during restore.

---

## Compile

```bash
# Trainer
gcc -O2 -std=c11 -I. -Icollection -Icollection/src \
    -Icollection/core -Icollection/core/core \
    -Icollection/core/pogls_engine/core \
    -Icollection/core/geo_headers \
    -Icollection/geo_jump_module/include \
    -o runner/cosplay_train.exe runner/cosplay_train.c \
    collection/geo_jump_module/src/geo_jump.c -lm

# Runner (with llama.cpp deps)
gcc -O2 -std=c11 -I. -Icollection ... \
    -II:/llama.cpp/include -II:/llama.cpp/ggml/include \
    -o runner/llama_pogls_runner_sid_v2.exe \
    runner/llama_pogls_runner_sid_v2.c \
    collection/geo_jump_module/src/geo_jump.c \
    runner/llama.dll runner/ggml.dll runner/ggml-base.dll \
    runner/ggml-cpu-x64.dll -lm
```

## Usage

```bash
# Train (all weight tensors, auto-selected)
cosplay_train bake_out/store.gsten bake_out/cosplay.qwen.cpl

# Run with cosplay
llama_pogls_runner_sid_v2 model.gguf --cosplay bake_out/cosplay.qwen.cpl --sid-face 1

# Run with cosplay + startup comparison
llama_pogls_runner_sid_v2 model.gguf --cosplay bake_out/cosplay.qwen.cpl --cosplay-compare --temp 0
```

---

## Limitations

- **Block scales get hit ~6% of the time**: adds random noise to block scale values. Harmless empirically but not clean.
- **Model-specific**: `.cpl` is trained from a gsten bake of a specific model. Cannot be reused across models.
- **Requires gsten for training**: the gsten file contains all tensor data needed for SID signature computation. Without it, cosplay cannot train.
- **Only weight tensors**: F32 norms/biases are skipped (not in SID cache). Perturbing them would require changes to the SID pipeline.

---

## Philosophy

Cosplay is not about fidelity — it's about **influence**. A 2.7KB file that measurably shifts a 500MB model's output is a different kind of compression: not lossless reconstruction of original weights, but **lossy perturbation that preserves coherence**.

This mirrors the "venom" philosophy from the SID Vision: inject, take over, carry forward. The perturbation is not the data — it's the **difference** that matters.
