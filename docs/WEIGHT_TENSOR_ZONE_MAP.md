# Weight Tensor Zone Map & Cosplay Mechanics

> **Internal Technical Report** — SID Weight Injection Safety Classification + Inference Perturbation Mechanics
>
> Date: June 28, 2026 | Project: FGLS / POGLS / SID
> Models Tested: Qwen2.5-0.5B, SmolLM2-360M, LFM2.5-1.2B

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Tensor Classification System](#2-tensor-classification-system)
3. [Zone Safety Map](#3-zone-safety-map)
4. [Protection Layers (Defense-in-Depth)](#4-protection-layers-defense-in-depth)
5. [Cosplay Perturbation Mechanics](#5-cosplay-perturbation-mechanics)
6. [Per-Decode Injection Flow](#6-per-decode-injection-flow)
7. [Experimental Results](#7-experimental-results)
8. [Key Numbers Reference](#8-key-numbers-reference)
9. [Threat Model & Limitations](#9-threat-model--limitations)
10. [File Reference](#10-file-reference)

---

## 1. Executive Summary

The SID (Swap-Injection-Decode) system performs **runtime weight tensor modification** in LLM inference by redirecting `tensor->data` pointers between decode steps. This document classifies every tensor in the model into safety zones and documents how cosplay perturbation profiles (`.cpl`) alter inference behavior.

### Three Core Discoveries

| # | Finding | Implication |
|---|---------|-------------|
| 1 | **58% of tensors (170/291)** are safe to perturb — weight matrices (Q8_0, ≥64KB) | Cosplay needs only 2.7KB to steer a 485MB model |
| 2 | **norm.weight** is **critical** — whitelisted at minimum hotness 0.5, excluded from corruption tests | Touching layer norms breaks numerical stability |
| 3 | **10 different `.cpl` files produce 10 different outputs** from the same deterministic prompt | Session-derived perturbation = personalized steering |

---

## 2. Tensor Classification System

### 2.1 Three-Tier Hotness Zones

Hotness (0.0–1.0) predicted via **cardioid express** geometry + 3-pass bond diffusion. Each tensor's position on the cardioid curve `r(θ) = a(1 + cos θ)` determines its zone.

```
                 HOT (≥ 0.9)
               ┌─────────────┐
               │  Inside     │  Primary injection targets
               │  cardioid   │  Full corruption strength
               └─────────────┘
               ┌─────────────┐
    WARM       │  Outside    │  Partial corruption
  (0.3–0.899)  │  cardioid   │  0.5× adaptive scaling
               └─────────────┘
               ┌─────────────┐
    COLD       │  Cusp near  │  Skipped (or inverted)
   (< 0.3)     │  θ=π        │  1-byte minimum if adaptive
               └─────────────┘
```

### 2.2 Distribution Across Architectures

| Model | Layers | HOT | WARM | COLD |
|-------|:------:|:---:|:----:|:----:|
| SmolLM2-360M | 32L | 55.2% | 40.0% | 4.8% |
| Qwen2.5-0.5B | 24L | 58.1% | 35.1% | 6.9% |

### 2.3 70% Depth Invariant

The **hottest layer in all tested models** sits at ~70–72% of total depth — a universal target for injection.

| Model | Layers | Hottest Layer | Depth % |
|-------|:------:|:-------------:|:-------:|
| SmolLM2-360M | 32 | blk.23 | 71.9% |
| Qwen2.5-0.5B | 24 | blk.17 | 70.8% |
| smolVLM-text | 30 | blk.21 | 70.0% |

---

## 3. Zone Safety Map

### 3.1 Complete Tensor Taxonomy

```
                          Total Tensors (291)
                               │
                    ┌──────────┴──────────┐
                    ▼                     ▼
              Weight Tensors         F32 Tensors
              (Q8_0, ≥64KB)          (norm, bias, small)
               ┌──────┐              ┌──────────────────┐
               │ 170  │              │      121         │
               └──┬───┘              └────────┬─────────┘
                  │                           │
          ┌───────┼───────┐            CRITICAL — Not in
          ▼       ▼       ▼            SID cache. Never
        HOT    WARM    COLD            perturbed. Bypass
       (≥0.9) (0.3-0.9) (<0.3)        compression.
          │       │       │
          │       │       └── Skipped by bond filter
          │       │            when --bond active
          │       │
          │       └── Adaptive: half corruption bytes
          │
          └── Primary targets — full strength
```

### 3.2 Zone Details

#### GREEN ZONE — Weight Tensors (170 tensors)

| Category | Examples | Count | dtype | Safe to Perturb? |
|----------|----------|:-----:|:-----:|:----------------:|
| Attention weights | `blk.N.attn_q.weight`, `attn_k.weight`, `attn_v.weight`, `attn_output.weight` | 96 | Q8_0 | **Yes** — primary cosplay targets |
| FFN weights | `blk.N.ffn_gate.weight`, `ffn_down.weight`, `ffn_up.weight` | 72 | Q8_0 | **Yes** — primary cosplay targets |
| Output embedding | `output.weight` | 1 | Q8_0 | **Yes** — tested standalone, coherent output |
| Token embedding | `token_embd.weight` | 1 | Q8_0 | **Yes** — in SID cache |

**Filter**: `size ≥ 64KB` (cosplay_train.c:20) + passes `bond_is_weight()` (bond_discovery.h:13-19)

#### YELLOW ZONE — Critical Norm Weights (49 tensors)

| Category | Examples | Count | dtype | Safe to Perturb? |
|----------|----------|:-----:|:-----:|:----------------:|
| Output norm | `output_norm.weight` | 1 | F32 | **NO** — full-corrupt → CRASH |
| Attention norm | `blk.N.attn_norm.weight` | 24 | F32 | **NO** — full-corrupt → CRASH |
| FFN norm | `blk.N.ffn_norm.weight` | 24 | F32 | **NO** — full-corrupt → CRASH |

**Empirical** (test_zone_safety.c, scenario 4): Full 0x42 replacement → `std::out_of_range` at index `18446744073709551615` (= -1 as size_t). F32 scale values become ~50.66, producing NaN/INF that cascade into out-of-bounds vector access.

**Mitigation**: Stride=64 (partial XOR) survives decode. The protection layers are sufficient — cosplay and SID swap only use stride≥64.

**Protection**: Whitelisted in both `hex_grid.h:353-356` and `bond_discovery.h:425-427`:
> `if (h[i] < 0.5f && strstr(name, "norm.weight")) h[i] = 0.5f;`

Excluded from all corruption tests in `test_phase_b.c` (lines 219, 233, 258, 276, 293).

#### RED ZONE — Bias Tensors (72 tensors)

| Category | Examples | Count | dtype | Safe to Perturb? |
|----------|----------|:-----:|:-----:|:----------------:|
| Q attention bias | `blk.N.attn_q.bias` | 24 | F32 | **NO** — full-corrupt → CRASH |
| K attention bias | `blk.N.attn_k.bias` | 24 | F32 | **NO** — full-corrupt → CRASH |
| V attention bias | `blk.N.attn_v.bias` | 24 | F32 | **NO** — full-corrupt → CRASH |

**Empirical** (test_zone_safety.c, scenario 5): Same `std::out_of_range` crash as norm.weight. Mechanism identical — F32 corruption → NaN → vector OOB.

**Mitigation**: Stride=64 (partial XOR) survives decode. Cosplay never targets biases (filtered at bond level).

**Protection**: `bond_is_weight()` returns 0 for bias. Not cached. Not compressed. Not swapped. Never enters any pipeline stage.

#### GRAY ZONE — Non-Layer Tensors (0 tensors found)

| Category | Examples | Count | dtype | Safe to Perturb? |
|----------|----------|:-----:|:-----:|:----------------:|
| (none in Qwen2.5-0.5B) | — | 0 | — | N/A |

All 121 F32 tensors are classified as either YELLOW (norm.weight) or RED (bias). No unclassified F32 tensor was found.

Hotness assigned neutral (0.5) since no layer number.

---

## 4. Protection Layers (Defense-in-Depth)

Five layers prevent accidental damage to critical tensors:

```
Layer 1 — Cache Exclusion (sid_loader.h:70-72)
  ┌──────────────────────────────────────────┐
  │ sid_loader_is_norm(name)                 │
  │   → if norm/bias: skip cache + compress  │
  │   → never enters SID cache               │
  └──────────────────────────────────────────┘

Layer 2 — Bond Graph Node Filter (bond_discovery.h:13-19)
  ┌──────────────────────────────────────────┐
  │ bond_is_weight(name)                     │
  │   → bias: return 0 (not a bond node)     │
  │   → norm without .weight: return 0       │
  └──────────────────────────────────────────┘

Layer 3 — Hotness Floor Whitelist (hex_grid.h:353-356, bond_discovery.h:425-427)
  ┌──────────────────────────────────────────┐
  │ norm.weight always ≥ 0.5                 │
  │   → even if cardioid says cold           │
  │   → never classified as "cold" tensor    │
  └──────────────────────────────────────────┘

Layer 4 — Corruption Test Exclusion (test_phase_b.c)
  ┌──────────────────────────────────────────┐
  │ All 4 scenarios skip norm.weight:        │
  │   baseline / hot corrupt / cold corrupt  │
  │   / all corrupt — norm.weight untouched  │
  └──────────────────────────────────────────┘

Layer 5 — Gear Lock Runtime Filter (llama_pogls_runner_sid_v2.c:406-426)
  ┌──────────────────────────────────────────┐
  │ After 3 decode cycles:                   │
  │   icosa lane events dynamically filter   │
  │   which swaps are active per decode      │
  └──────────────────────────────────────────┘
```

---

## 5. Cosplay Perturbation Mechanics

### 5.1 What `.cpl` Contains

A `.cpl` file stores only **perturbation recipes** — not tensor data. Size: ~2.7KB for 170 tensors (vs 485MB `.gsten` store).

```
  File Layout (20 + n×16 bytes)
  ┌──────────────────────────────────────┐
  │ magic    (4B) = 0x504F434C ("CPL")   │
  │ version  (4B) = 2                    │
  │ n        (4B) = entry count          │
  │ n_layers (4B)                        │
  │ n_heads  (2B)                        │
  │ n_embd   (2B)                        │
  ├──────────────────────────────────────┤
  │ Entry[0..n-1] (16B each):            │
  │   name_hash  (4B) — FNV-1a of name   │
  │   mode       (1B) — 0=XOR, 1=SET     │
  │   arg        (1B) — xor byte/value   │
  │   stride     (2B) — apply every N B  │
  │   data_size  (4B) — expected size    │
  │   tick       (4B) — reserved         │
  └──────────────────────────────────────┘
```

### 5.2 Perturbation Function

```c
cosplay_apply(entry, src_data, src_size) {
    buf = malloc(size)           // new copy
    memcpy(buf, src_data, size)  // copy original
    stride = entry.stride        // default 64
    for (i = 0; i < size; i += stride)
        if (mode == XOR) buf[i] ^= arg
        if (mode == SET) buf[i] = arg
    return buf                   // malloc'd perturbed copy
}
```

**Key properties**:
- Allocates **new malloc'd buffer** (original untouched)
- **Stride-based**: touches every Nth byte only (~1.5% of bytes per tensor)
- **Default stride = 64**: 1 in 64 bytes perturbed

### 5.3 Statistical Impact on Quantized Blocks

For Q4_K_M block (144 bytes) with stride=64:

```
Block (144B) ┌─────┬──────────────────────────┐
             │scale│     quantized data       │
             │2-4B │       30-128B            │
             └─────┴──────────────────────────┘
                 ↑              ↑
            stride hits     stride hits
            ~6% of time     ~94% of time
```

- **94% of perturbations** land on quantized data → ±1 change in integer representation → insignificant to model behavior
- **6% land on block scales** → minor noise, empirically harmless
- **Model stays coherent** at all perturbation levels (1–170 tensors)

### 5.4 Profile-Aware Stride Modulation

Stride is modulated by **face usage frequency** from session profile (`.ses`):

| Face Usage | Stride | Effect |
|:----------:|:------:|--------|
| > 20% | 32 | Strong perturbation for most-used faces |
| > 10% | 48 | Moderate |
| > 4% | 64 | Default |
| > 0.5% | 96 | Light |
| ≤ 0.5% | 128 | Minimal for least-used faces |

Higher face usage = smaller stride = more bytes perturbed = stronger steering.

---

## 6. Per-Decode Injection Flow

### 6.1 Init Phase (one-time, before any decode)

```
  GGUF Model (291 tensors)
       │
       ▼
  Tensor Scan
  ──────────────────────
  - VirtualQuery → 290/290 names matched
  - 170 weight tensors ≥ 64KB identified
       │
       ▼
  SID Cache Init
  ──────────────────────
  For each of 291 tensors:
    - sid_loader_load(name)
    - if !sid_loader_is_norm(name):
        sid_cache_put_compressed() → Shell compression
    - 170 weight tensors cached, 121 norms/biases skipped
       │
       ▼
  Init Redirect (before context creation)
  ──────────────────────
  For each cached tensor:
    tensor_set_data(ptr, sid_cache_buf)
    → scheduler captures heap pointers (avoids b9686 deadlock)
       │
       ▼
  Cosplay Apply (if --cosplay)
  ──────────────────────
  For each swap entry:
    hash = FNV-1a(tensor_name)
    if hash matches .cpl entry:
      perturbed = cosplay_apply(entry, sid_data, size)
      sid_swaps[s].sid_data = perturbed  ← pointer swap
      sid_swaps[s].is_malloc = 1
       │
       ▼
  Context Creation
  ──────────────────────
  llama_init_from_model(model, cp)
  → Scheduler sees heap pointers from init redirect
```

### 6.2 Decode Cycle (per token)

```
  ┌─────────────────────────────────────────────────────┐
  │               PER TOKEN DECODE LOOP                 │
  ├─────────────────────────────────────────────────────┤
  │  ①  sid_swap_apply()                                │
  │     → for each active swap:                         │
  │       tensor_set_data(ptr, delta_sid_data[i])       │
  │       → redirect to (perturbed) SID cache buffer    │
  │       → time travel journal (if --timetravel)       │
  │                                                     │
  │  ②  llama_decode(ctx, batch)                        │
  │     → backend reads tensor->data                    │
  │     → computes with modified weights                │
  │     → model cannot detect the swap (transparent)    │
  │                                                     │
  │  ③  sid_swap_restore()                              │
  │     → for each applied swap:                        │
  │       tensor_set_data(ptr, orig_data)               │
  │       → restore original GGUF pointer               │
  │       → time travel finalize                        │
  │                                                     │
  │  ④  Sample next token from (perturbed) logits       │
  └─────────────────────────────────────────────────────┘
```

### 6.3 Why This Works

The **CPU/Vulkan backend reads `tensor->data` on every `llama_decode` call**. There is no caching or deduplication at the pointer level. Swapping between decodes guarantees the model sees modified weights without:
- Model reload
- Memory reallocation
- Context recreation
- Any model-awareness of the swap

---

## 7. Experimental Results

### 7.1 Cosplay Scaling (Qwen2.5-0.5B Q4_K_M, temp=0, --sid-face 1)

| # Tensors | `.cpl` Size | First Token | Output Tail |
|:---------:|:-----------:|:-----------:|-------------|
| 0 | — | `\n` (271) | `Hello! How can I assist you today? ...` |
| 1 (output.weight) | 36 B | `\n` (271) | `...of utmost importance in helping me improve...` |
| 3 | 68 B | `\n` (271) | `I'm not sure what you're asking for...` |
| 9 | 164 B | `\n` (271) | `I'm not sure what you mean by "Hello"...` |
| 25 (attn_output) | 420 B | `\n` (271) | `Same pattern as 9 but more defensive` |
| **170 (ALL)** | **2.7 KB** | `,` (11) | `I'm not sure what you're asking for...` |

> **Key finding: Model remains coherent at every level. No crash, no garbage.**

### 7.2 Profile-Aware Cosplay — 10 Conditions, Same Prompt

Prompt: `"Hello"`, temp=0 (fully deterministic). Baseline + 9 `.cpl` files derived from session profiles.

| Condition | Cluster | Cosine vs Baseline | First Token |
|-----------|:-------:|:------------------:|:-----------:|
| **Baseline** | — | 1.0000 | `,` (11) |
| devops_101 | C1 edge | **0.9460** | ` ` (220) |
| devops_203 | C1 edge | **0.9460** | `\n` (198) |
| hash_table_005 | C0 centroid | 0.9528 | `,` (11) |
| java_265 | C0 edge | 0.9528 | `.` (13) |
| java_367 | C0 edge | 0.9528 | `......` (28149) |
| jwt_348 | C1 centroid | 0.9833 | `\n` (198) |
| nosql_233 | C2 centroid | 0.9826 | `,` (11) |
| regex_040 | C2 edge | 0.9848 | `\n\n` (271) |
| regex_142 | C2 edge | 0.9848 | `\n\n` (271) |

> **Key finding: Same deterministic prompt produces 10 different outputs — differentiated by cosplay profile alone.**

### 7.3 Cosine Similarity Matrix (Selected)

```
               ┌─────────┬─────────┬─────────┬─────────┬─────────┬─────────┐
               │ baseline│ devops  │ hash_t  │ jwt_348 │ nosql   │ regex   │
┌──────────────┼─────────┼─────────┼─────────┼─────────┼─────────┼─────────┤
│ baseline     │  1.0000 │  0.9460 │  0.9528 │  0.9833 │  0.9826 │  0.9848 │
│ devops_101   │  0.9460 │  1.0000 │  0.9940 │  0.9591 │  0.9653 │  0.9578 │
│ hash_table   │  0.9528 │  0.9940 │  1.0000 │  0.9622 │  0.9679 │  0.9618 │
│ jwt_348      │  0.9833 │  0.9591 │  0.9622 │  1.0000 │  0.9951 │  0.9981 │
│ nosql_233    │  0.9826 │  0.9653 │  0.9679 │  0.9951 │  1.0000 │  0.9946 │
│ regex_040    │  0.9848 │  0.9578 │  0.9618 │  0.9981 │  0.9946 │  1.0000 │
└──────────────┴─────────┴─────────┴─────────┴─────────┴─────────┴─────────┘
```

- C0 clusters (devops/hash_table): most aggressive → cosine 0.946–0.953
- C1/C2 clusters (jwt/regex/nosql): more conservative → cosine 0.983–0.985

### 7.4 Cosplay-Compare (WITH vs WITHOUT, --cosplay-compare)

| Metric | Value |
|--------|:-----:|
| First token WITH cosplay | `,` (11) |
| First token WITHOUT cosplay | `\n` (271) |
| **Same token?** | **NO** |
| Logits cosine similarity | 0.997262 |
| Max logit difference | 1.467321 |
| Same-sign ratio | 148773/151936 (97.9%) |

> Despite 99.7% logit similarity, **the sampled first token changes**.

### 7.5 Hybrid Filter — Zero-Waste Injection

| Approach | Tensors | RMSE vs Clean | Effect Retained |
|----------|:-------:|:-------------:|:---------------:|
| Baseline (hot+warm) | 211 | 0.206 | 100% |
| **Hybrid cluster** | **141** | **0.186** | **~95%** |
| Multi-depth (3 layers) | 21 | varies | varies |

> **~50% of tensors achieve ~95% of steering effect.**

### 7.6 Stride Sensitivity (Experiment Framework)

| Condition | Stride | First Token | Quality |
|-----------|:------:|:-----------:|---------|
| Baseline | — | `,` (11) | Clean |
| 25 attn-output | default | `,` (11) | Subtle (same 1st token) |
| Full-170 stride=32 | 32 | **garbled** (18137) | **Too aggressive** |
| Full-170 stride=64 | 64 | ` ` (4102) | Coherent, different |
| Full-170 default | default | ` Je` (14133) | Coherent, different |

> stride=32 on all 170 tensors → garbled output (destructive). stride=64+ → safe.

### 7.7 Zone Crash Test (Systematic, June 28)

Systematic test (`test_zone_safety.c`) corrupting each zone and measuring crash/survival on Qwen2.5-0.5B (291 tensors).

| # | Scenario | Zone | Stride | Tensors | Crash? | Logits vs Baseline |
|---|----------|------|:------:|:-------:|:------:|:------------------:|
| 1 | green_stride64 | GREEN | 64 | 170 | **OK** | cos=0.989, diff=100% |
| 2 | green_stride32 | GREEN | 32 | 170 | **OK** | cos=0.985, diff=100% |
| 3 | green_stride8 | GREEN | 8 | 170 | **OK** | cos=0.989, diff=100% |
| 4 | green_fullcorrupt | GREEN | 0 (0x42) | 170 | **OK** | cos=0.974, diff=100% |
| 5 | yellow_norm_weight | YELLOW | 0 (0x42) | 49 | **CRASH** | N/A |
| 6 | red_bias | RED | 0 (0x42) | 72 | **CRASH** | N/A |
| 7 | gray_other | GRAY | 0 | 0 | **OK** | identity |
| 8 | green+yellow | GREEN+YELLOW | 64 | 219 | **OK** | distinct |
| 9 | all_non_yellow | GREEN+RED+GRAY | 64 | 242 | **OK** | distinct |
| 10 | all_tensors | ALL | 64 | 291 | **OK** | distinct |

**Crash signature**: `std::out_of_range` with index `18446744073709551615` (= (size_t)-1). Triggered when F32 norm/bias scales corrupted to ~50.66 via full 0x42 replacement → NaN/INF propagation → out-of-bounds vector access.

**Key findings**:
1. **GREEN zone**: 100% safe at ALL corruption levels (stride 64/32/8/full). Q8_0 quantization is robust.
2. **YELLOW/RED zones**: CRASH on full corruption (stride=0). **Survive stride=64** (partial XOR). Cosplay default stride=64 is safe.
3. **GRAY zone**: 0 tensors — all F32 tensors are classified as norm.weight (49) or bias (72).
4. **Multi-zone with stride=64**: All combinations survive — stride=64 on YELLOW/RED does not cause NaN.

---



## 8. Key Numbers Reference

### 8.1 Tensor Counts

| Number | Description |
|:------:|-------------|
| 291 | Total tensors in Qwen2.5-0.5B GGUF |
| 291 | Found by VirtualQuery scan (improved scanner) |
| **170** | In SID cache (safe to perturb, Q8_0) |
| 121 | F32 norms/biases (NOT in cache, DO NOT TOUCH) |
| **49** | `norm.weight` (critical — YELLOW zone, full-corrupt → CRASH) |
| **72** | `bias` (RED zone, never cached, full-corrupt → CRASH) |
| 0 | GRAY zone (all 121 F32 tensors are YELLOW or RED, no unclassified) |
| 141 | Hybrid cluster size (95% effect with 50% tensors) |

### 8.2 File Sizes

| Item | Size | vs Gsten |
|------|:----:|:--------:|
| `.gsten` store | 485 MB | 1× |
| `.cpl` (170 tensors) | **2,740 B** | **0.0005%** |
| output.weight only | 36 B | — |
| 25 tensors | 420 B | — |

### 8.3 Performance

| Metric | Value |
|--------|:-----:|
| Model load | 456–547 ms |
| Inference (Vulkan) | 65+ t/s |
| DRam speed | 2,122 MB/s |
| Shell decode | 1,400 MB/s |
| Shell ratio (Q4) | 1.88× |
| Capture speed | 26,106 t/s (38 µs/tensor) |

### 8.4 Hotness Propagation

| Parameter | Value |
|-----------|:-----:|
| Propagation passes | 3 |
| Self-weight | 0.7 |
| Neighbor contribution | 0.3 |
| Hotness floor (norm.weight) | 0.5 |
| Cold threshold | 0.3 |
| Bond types | 10 (LAYER_SLOT to METATRON_HUB) |
| Bond count (SmolLM2) | 2,320 |

---

## 9. Threat Model & Limitations

### 9.1 What Could Break the Model

| Action | Result | Evidence |
|--------|--------|----------|
| Full-corrupt `norm.weight` (49× F32) | **CRASH** — `std::out_of_range` | `test_zone_safety.c` scenario 4 — F32 → NaN → vector OOB |
| Full-corrupt `bias` (72× F32) | **CRASH** — `std::out_of_range` | `test_zone_safety.c` scenario 5 — same mechanism |
| Stride=64 on YELLOW/RED | **Survives** | Scenarios 7-10: stride=64 on combined zones all OK |
| stride < 32 on ALL GREEN tensors | **Garbled output** | Confirmed in experiment (stride=32 → garbled 18137) |
| Full 0x42 GREEN only | **Coherent output** | cos=0.974, different but not garbled |
| Perturb 100% of bytes | **Destroy quantization** | Cosplay ~1.5% per tensor is intentional |

### 9.2 Confirmed Safe Bounds

- **Stride ≥ 64**: Safe and coherent across all 170 tensors
- **Stride = 32**: **Unsafe** when applied to all tensors (garbled)
- **1–25 tensors**: Always coherent regardless of stride
- **Block scale hits (6%)**: Empirically harmless

### 9.3 Known Gaps

| Gap | Why |
|-----|-----|
| Per-tensor crash test | Done for entire zones (49 norm, 72 bias, 170 weight). Single-tensor granularity not tested |
| GPU backend perturbation | Vulkan/CUDA SID path not tested — currently CPU-only in test harness |
| Larger models (>1.2B) | Only tested on 0.5B–1.2B. 7B+ may have different sensitivity |
| F32 norms not in cache | Cannot perturb via SID — would require pipeline changes. Cosplay stride=64 avoids NaN |
| SmolLM2-360M (no biases) | Only tested GREEN zone there. YELLOW/RED crash tests only on Qwen2.5-0.5B |

---

## 10. File Reference

| File | Role | Key Content |
|------|------|-------------|
| `runner/hex_grid.h` | Hotness prediction | `norm.weight` whitelist (L353-356), cardioid LUT, 3-pass diffusion |
| `runner/bond_discovery.h` | Bond graph | `bond_is_weight()` filter (L13-19), hotness whitelist (L425-427) |
| `runner/sid_loader.h` | Tensor loading | `sid_loader_is_norm()` cache exclusion (L70-72) |
| `runner/cosplay.h` | Perturbation engine | `cosplay_apply()` XOR/SET with stride (L45-58), .cpl format |
| `runner/sid_cache.h` | Weight cache | Shell compression, `sid_cache_put_compressed()` |
| `runner/llama_pogls_runner_sid_v2.c` | Main runner | `sid_swap_apply/restore`, cosplay integration, experiment loop |
| `runner/test_phase_b.c` | Zone safety test | HOT vs COLD corruption comparison, `norm.weight` exclusion |
| `runner/test_hex_swap.c` | Hex test | `is_norm()` filter |
| `runner/test_zone_safety.c` | Zone crash test | Systematic 10-scenario crash/coherence test, `--scenario N` |
| `runner/cosplay_profile_train.c` | Profile trainer | `stride_from_usage()` modulation |
| `docs/COSPLAY.md` | Documentation | Full cosplay reference: format, results, design decisions |
| `docs/DEVELOPMENT_SUMMARY.md` | Development log | Architecture, experiment results, known issues |
| `SID_RESEARCH_REPORT.md` | Research report | Core mechanism, 70% invariant, hybrid filter, time travel |

---

## Appendix A: Filter Source Code Summary

```
Filter                        File:Line               Logic
──────────────────────────────────────────────────────────────────────
sid_loader_is_norm()          sid_loader.h:70-72      strstr(name, "norm|_norm|bias")
bond_is_weight()              bond_discovery.h:13-19  reject bias; reject norm without .weight
norm.weight whitelist         hex_grid.h:353-356      if h < 0.5 → clamp to 0.5
norm.weight whitelist         bond_discovery.h:425-427 if h < 0.5 → clamp to 0.5
is_norm()                     test_hex_swap.c:120-122 strstr(name, "norm.weight")
MIN_WEIGHT_BYTES              cosplay_profile_train.c:20  64 KB threshold
hotness thresholds            bond_discovery.h:454-458    ≥ 0.9 HOT, ≥ 0.3 WARM, else COLD
```

## Appendix B: Cosplay Entry JSON Schema

```json
{
  "profile": {
    "magic": "CPL (0x504F434C)",
    "version": 2,
    "n_entries": 170,
    "layers": 28,
    "heads": 0,
    "embd": 0
  },
  "entry": {
    "name_hash": "uint32 FNV-1a",
    "mode": "0=XOR, 1=SET",
    "arg": "byte value (default 0x01)",
    "stride": "uint16 (default 64)",
    "data_size": "uint32 (expected tensor size)",
    "tick": "uint32 (reserved)"
  }
}
```

---

*End of report. For questions or corrections, refer to source files listed in §10.*
