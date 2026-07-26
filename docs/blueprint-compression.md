# Blueprint Compression via Circle Packing

## Overview

Circle packing geometry enables deterministic weight compression using tessellation properties. This document summarizes findings from testing across multiple models and formats.

## Core Insight

**Tessellation = 1 centroid sufficient (not 7)**

```
Hexagonal tessellation properties:
- Edge-to-edge contact (no gaps)
- Vertex adjacency (point-to-point)
- 60° rotational symmetry

→ Center determines 6 outer positions by rotation
→ Only 1 centroid + 1 radius needed
```

## Blueprint Structure

```
Original float32: 32 weights × 4 bytes = 128 bytes

Blueprint:
  1 centroid × 4 bytes =  4 bytes (center weight)
  1 radius  × 4 bytes =  4 bytes (rotation distance)
  32 deltas × 1 byte  = 32 bytes (quantized residuals)
  1 scale   × 4 bytes =  4 bytes (delta scaling)
  Total                = 44 bytes

Compression: 65.6% savings (float32 → blueprint)
             34.4% kept
```

## Formula

```
angle = 60° (hexagonal symmetry)

7 centroids → 1 centroid:
  Savings = (7-1)/7 = 85.7% centroid reduction

Total blueprint savings:
  (128 - 44) / 128 = 65.6%
```

## Integrity Results

### Qwen3-4B-Q5_0 (2.7GB GGUF)

| Metric | Value |
|--------|-------|
| Avg error | 0.02% |
| Max error | 0.20% |
| PSNR | 58-82 dB |
| Exact matches | 77.9% |
| Compression | 65.6% |

### Quantization Format Comparison

| Format | Avg Δ | Max Δ | Exact Match |
|--------|-------|-------|-------------|
| Q8_0 | 0.02% | 0.17% | 75% |
| Q5_0 | 0.01% | 0.17% | 84% |
| Q4_0 | 0.01% | 0.18% | 88% |
| Q2_K | 0.00% | 0.20% | 100% |

**Key finding:** Lower quantization → better compression (weights spread more)

### smolVLM-256M BF16 SafeTensors

| Layer Type | Avg Δ | Max Δ | PSNR |
|------------|-------|-------|------|
| Layer norms | 0.03-0.08% | 0.06-0.23% | 65-187 dB |
| All tested | 0.04% avg | 0.12% max | 68 dB avg |

## GGUF + Blueprint Complement

```
GGUF = weight values (quantized, small, for compute)
Blueprint = geometric structure (deterministic, for observe)
```

### Architecture

```
┌─────────────────────────────────────┐
│           GGUF Q8_0                │  ← weight values
│   32 int8 + 1 fp16 scale          │     for inference engine
└──────────────┬──────────────────────┘
               │
┌──────────────▼──────────────────────┐
│         Blueprint Layer            │  ← geometric structure
│   1 centroid + cluster mapping     │     for observation
└─────────────────────────────────────┘
```

### Use Cases

1. **GGUF for inference:** Q8_0 weights → compute as normal
2. **Blueprint for observation:** centroids show weight clustering patterns
3. **Blueprint for optimization:** geometric structure → better block boundaries

## Models Tested

| Model | Format | Size | Tensors |
|-------|--------|------|---------|
| SmolLM2-360M-Instruct | Q8_0 GGUF | 369 MB | 290 |
| Qwen3-0.6B-Q8_0 | Q8_0 GGUF | 610 MB | 290 |
| Qwen3-4B-Q5_0 | Q5_0 GGUF | 2.7 GB | — |
| smolVLM-256M-Instruct | BF16 SafeTensors | 490 MB | 471 |

## Files Created

| File | Purpose |
|------|---------|
| `analyze_weight_geometry.c` | Circle packing + Fibonacci analysis |
| `scan_gguf_weights.c` | Raw Q8_0 block scanner |
| `blueprint_integrity_test.c` | Full compress→decompress→verify |
| `analyze_safetensors.c` | SafeTensors reader + BF16 decode |

## Connection to RDH/Beam

- **Tessellation 60°** = rotation-based compression
- **1 centroid + rotation** = geometric invariant
- **Blueprint structure** = deterministic weight organization
- **GGUF + Blueprint** = values + structure = complete system
