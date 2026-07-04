# Handoff: POGLS v3 — Geo Frame Layout

## Goal of Next Session

Prototype **POGLS v3**: pure binary chunk format with no header/index/meta — just raw geo frames via `geo_frame_seek.h`. Remap Q8_0 blocks to 144² grid, delta encode, write as frame sequence.

## State of Play

### ✅ Done (July 3)
- POGLS v2 native mode works: GGUF→POGLS→inference (verified on 3 models: Qwen2.5-0.5B, SmolLM2-360M, LFM2.5-1.2B)
- Root cause found: `llama_model_init_from_user()` has bug in b9733 DLL — use GGUF file load + tensor->data redirect instead
- gguf_path embedded in POGLS header for auto-locating source GGUF
- Cleaned out unused callback code (pogls_set_tensor_data, PoglsLoadCtx)

### ❌ Not Done / Blockers
- Q8_0 weights are incompressible at byte level (1.00× ratio for zstd/shell/diamond)
- **Remap not yet implemented** — current POGLS just stores RAW bytes in GGUF order
- Need to implement block-level remap to 144² grid + delta encoding
- POGLS file size = GGUF file size (no compression gain yet)
- `--ngl` GPU mode not yet tested with POGLS

## Key Architecture Insight (July 3)

```
128 × 162 = 144² = 20736
geo_frame_seek: enc(uint16_t) → frame_at(enc) → face(0..11), slot(0..119), Hilbert/Peano, ico_idx
1440 frame cycle × stride-37 walk = full bijection
```

The address space (144²) and geo frame timeline (1440) are the SAME SYSTEM — `enc` IS the address. Storing 2 bytes per tensor is enough to derive all geometry.

### Next Architecture (POGLS v3)

```
.pogls = raw binary chunk:
         [Frame_0_data] [Frame_1_data] ... [Frame_N_data]

No magic. No header. No index. No offset table.
frame_seek(enc) → offset → read

Per-tensor metadata: first_enc (uint16_t) + n_frames (uint8_t) = 3 bytes
Delta encode between adjacent faces → zstd works (range-limited ints)
```

### Compression Pipeline (Remap-Only, No Training)

1. Read GGUF tensor data (Q8_0 blocks of 34B / 32 elems)
2. Permute blocks → 144² grid (optimize spatial correlation)
3. Delta encode: Δ[i] = block[i] - predict(neighbors via frame_at)
4. zstd compress Δ (range-limited → compressible)
5. Write frames sorted by enc order

Expected 18% reduction without training.

## Open Decisions

1. **Frame size**: What's the atomic unit? 1 Q8_0 block (34B)? 1 KB page?
2. **Delta strategy**: Weighted average of adjacent faces? Linear prediction?
3. **Skip-index**: Optional file-offset table appended to chunk, or pure sequential scan?
4. **GPU mode**: Can decode happen on-device or must be host-side?
5. **GGUF path**: GGUF still needed for metadata — embed full path or just hash?

## Skills to Use Next Session
- `msys2-build-pipeline` — for building C prototype
- `focused-fix` — systematic implementation of remap pipeline
- `cross-session-board` — track progress on board cards

## Artifacts (existing, not duplicate)

- **Board spec**: card #31 "POGLS v3: Geo Frame Layout" (on inbox board)
- **Running code**: `runner/gguf_to_pogls.c` — current v2 converter (to be replaced)
- **Runner**: `runner/llama_pogls_runner_sid_v2.c` — `--pogls` mode (tensor→data redirect)
- **Geo frame**: `collection/geo_frame_seek.h` — deterministic frame seek (1440 cycle)
- **Address space**: `runner/addr_space.h` — 128×162 = 144² unified address
- **Memory**: ID 12 (Power-of-N Address Space), ID 28 (POGLS redirect approach)
- **Model test files**: `runner/smol.pogls`, `runner/lfm12.pogls` (just generated)
