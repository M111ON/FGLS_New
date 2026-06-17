# Phase 3: Runner Integration & Demo — Research

## RESEARCH COMPLETE

## Domain Analysis

### Existing Assets

#### Runner: `runner/llama_pogls_runner_sid_v2.c` (1023 lines)
- **Tensor discovery**: VirtualQuery-based memory scan finds 291/291 tensors via GGUF name matching (proven by test_swap_all.c)
- **SID cache**: Full GGUF-level caching with hash verification. Cache holds all non-norm weight tensors (~600MB for SmolLM2-360M)
- **Decode loops**: Two paths — chat mode (interactive, ~line 913) and prompt mode (single-shot, ~line 983). Both wrap `llama_decode()` with `sid_swap_apply()/sid_swap_restore()`
- **CLI parsing**: Simple `if/else` chain (lines 257-337). Existing flags: `--chat`, `--prompt`, `--bond`, `--hex`, `--sid-face`, `--sid-corrupt`, `--dump-logits`, etc.
- **Time travel**: Delta ring journaling for checkpoint/rewind/ffwd (sid_delta_ring.h, sid_timetravel.h)

#### 12-Face Pipeline: `collection/tw_face_bridge.h` (809 lines)
- `tw_iterate_faces(vx, vy, &result)` — capture all 12 faces for a 2D signature
- `tw_capture_face(vx, vy, face, &cap)` — single-face capture with rotation
- `tw_face_to_tring(face, zone, slot, is_tri)` → 0..1439 TRing position
- `tw_face_rewind_store(rb, &cap)` — store to rewind buffer (1440 slots)
- `tw_freeze_wallet_write(path, entries, n)` — write binary freeze wallet
- `TWFreezeEntry`: 18-byte packed entry with tring_pos, freeze_addr, tick, packed_key
- `tw_face_to_frame(&cap)` → `DualFrame` timeline seek

#### Capture Core: `collection/tw_capture_int.h` (359 lines)
- `tw_capture_int_combined(vx, vy, &cap, &is_tri)` — combined hex+tri grid in one pass (12 centroids/sector)
- `tw_reconstruct_int_combined(&cap, is_tri, &vx, &vy)` — exact integer reconstruction (lossless)
- Fixed-point SCALE=207360, 10 sectors × 12 centroids/sector

#### Tensor to Signature: `collection/tw_tensor_capture.h` (206 lines)
- `tw_capture_tensor_raw(raw, nbytes, rows, cols, dtype, &out)` — dequantizes Q8_0/F32, computes 2D sig (first half / second half means), scales to TW_SCALE, runs TW capture
- Max 128 rows × 4096 cols limit (first row only for signature)

#### Timeline: `collection/geo_frame_seek.h` (216 lines)
- `frame_at(enc)` → `DualFrame` O(1) decomp, FRAME_CYCLE=1440

#### Existing Test: `collection/tests/test_tw_face_bridge.c` (495 lines)
- T1-T8 tests covering rewind buffer, freeze wallet, single/multi-face, frame seek, world A/B, full pipeline

### Integration Architecture

#### Capture Insertion Points in Runner
1. **After prompt decode** (line ~960 chat, line ~992 prompt) — captures all tensor signatures after initial context ingestion
2. **Per-token** (lines ~975, ~1008) — deferred per D-11

#### Proposed Capture Flow
```
After llama_decode() → tensor data committed in GGUF memory:
  for each found_tensor:
    Read tensor->data (first TW_TENSOR_MAX_COLS = 4096 elements)
    Dequantize Q8_0 blocks via _tw_dequant_q80 → float buffer
    Compute 2D sig: mean(first half), mean(second half)
    Scale to fixed-point: vx = sig_x * TW_SCALE, vy = sig_y * TW_SCALE
    Run tw_iterate_faces(vx, vy, &iter)
    For each face result:
      Store to rewind buffer (tw_face_rewind_store)
      If frozen: create TWFreezeEntry → freeze_log[]
  Write freeze wallet: tw_freeze_wallet_write("{outdir}/capture-{timestamp}.tw", freeze_log, n_frozen)
  Write .gsten store (binary dump of all captured data)
  Verify: reconstruct from stored data → byte-by-byte compare
  Print: "lossless: ✓ (N tensors verified)" or "lossless: ✗"
```

#### Key Requirements
- **Model-agnostic**: Works on any GGUF model (SmolLM2-360M, Qwen2.5-Coder, etc.)
- **No llama.cpp changes**: Only collection/ headers and runner code
- **Output**: Both .tw freeze wallet + .gsten store
- **Verification**: Byte-by-byte comparison after reconstruct
- **CLI**: `--capture <outdir>` flag

### Risks
1. **Q8_0 dequant in runner**: Requires bringing `_tw_half_to_float` and `_tw_dequant_q80` into runner namespace
2. **Memory**: Capture reads first 4096 float elements per tensor; for 291 tensors ≈ 4.7MB temporary buffer
3. **Performance**: 12-face iteration per tensor = 291 × 12 = 3492 capture calls per decode. Capture rate may drop
4. **Include path**: Runner needs `-I../collection` to access tw_face_bridge.h chain

### Dependencies
- Phase 1 (12-Face Bridge Core) — capture pipeline
- Phase 2 (Timeline Mapping) — frame_at integration for DualFrame
- No external dependencies beyond llama.cpp + ggml
