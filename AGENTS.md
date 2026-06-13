# AGENTS.md — Session Handoff

## ⚠️ ห้ามใช้ git โดยเด็ดขาด
- **ห้าม run `git` commands ใดๆ ทั้งสิ้น** ไม่ว่า git status, git add, git commit, git push, git clean, git reset, git checkout ฯลฯ
- หากจำเป็นต้องตรวจสอบประวัติไฟล์ ให้ใช้ Read tool อ่านไฟล์โดยตรงเท่านั้น
- หาก user ต้องการให้ commit หรือ push เดี๋ยว user จัดการเอง
- ข้อยกเว้น: ถ้า user สั่งให้ใช้ git โดยตรง (written in stone) เท่านั้น

## Pre-scan check
- ก่อนเรียก scan() ให้เรียก get_project_index() เพื่อดูสถานะ cache
- หาก cache มีข้อมูลล่าสุด ให้ใช้ cached data แทนการสแกนใหม่
- หาก cache ไม่มีข้อมูลหรือล้าสมัย ให้สแกนตามปกติ

---

## Session June 13 — Full Pipeline Audit + 12-Face Bridge Plan

### tl;dr
Full pipeline test completed: TW capture → Shell 1+2+3 → wallet genesis on real SmolLM2-360M Q8_0 (290 tensors). **3 bugs fixed** (f16 subnormal, negative Q8_0 scale, int32 overflow). **290/290 lossless reconstruction**. **26,106 t/s** capture speed (38 µs/tensor). Comprehensive **system audit** revealed major disconnect: code is defined+tested but NOT wired into a running pipeline. Next session: **12-face bridge** — extend TW from 1 face (10×6=60 slots) to full TRing 720 (12 faces × 60 slots), then map into `geo_rewind.h` buffer + `geo_frame_seek.h` timeline.

---

### Full Pipeline Performance (SmolLM2-360M Q8_0, 290 tensors, GTX 1050 Ti)

| Step | Time | Rate |
|------|------|------|
| Load (367 MB, 290 files) | 0.285 s | 1,287 MB/s |
| TW Capture | 0.012 s | 26,106 t/s (38 µs/tensor) |
| Reconstruction | ~0 s | 85M t/s |
| Shell 1-3 bridge + wallet genesis | ~0 s | 6.7M t/s |
| Unified `.gsten` store build | 0.533 s | 688 MB/s |
| **Total pipeline** | **0.83 s** | |

| Quality Metric | Value |
|----------------|-------|
| Lossless reconstruction | 290/290 (100%) |
| Zone coverage | 10/10 (100%) |
| Slot coverage | 23/60 (38.3%) |
| Zone entropy | 3.1486 bits (94.8% of max) |
| Drain rate | 1.0% (3 tensors) |
| Frozen (Shell 3, tick=12) | 3/3 drain → wallet |

| Size | Bytes |
|------|-------|
| Raw Q8_0 store (290 .qdat) | 384,618,240 (367 MB) |
| Unified `.gsten` | 384,620,866 (367 MB + 0.0% overhead) |
| Routing `.twidx` | ~74 KB (**~5000× reduction**) |

---

### What Was Built in This Session

**New files:**
- `collection/tw_tensor_capture.h` — C bridge: Q8_0 dequant → 2D signature → TW capture with `_tw_dequant_q80()`, `_tw_dequant_f32()`, dtype-aware `tw_capture_tensor_raw()`, `tw_capture_tensor_by_name()`
- `collection/tests/test_tw_tensor_capture.c` — reads real .qdat files via RawBridge, runs TW capture on 290 tensors, verifies reconstruction
- `collection/tests/test_full_pipeline.c` — comprehensive pipeline test: load → capture → recon → Shell 1+2+3 bridge → wallet genesis → hex_tile compression test → .gsten store write
- `collection/build_gsten_store.c` — C encoder for unified .gsten store (was in progress)
- `build/smollm2.gsten` — unified store: 290 tensors → 1 file (384 MB)

**Fixed bugs:**
1. **f16 subnormal** — `_tw_half_to_float(0x0000)` returned ~5.96e-8 instead of 0.0f
2. **Q8_0 negative scales** — Python `_tw_dequant_tensor()`: `if scale_f > 0` rejected valid negative scales (e.g. -171.875) → set to 1.0
3. **int32 overflow** — `(int32_t)(sig * TW_SCALE)` where sig ≈ ±20000, TW_SCALE=207360 → product ≈ ±4.1B > INT32_MAX. Changed all `int32_t` inputs (vx, vy, resid) to `int64_t` everywhere in `tw_capture_int.h` and `tw_tensor_capture.h`

**Enhanced files:**
| File | Change |
|------|--------|
| `geom_raw_bridge.h` | Added `int dtype` to RBEntry, `.qtype` sidecar loading in `rb_load()` |
| `build_smollm2_store.py` | Added .qtype file writing, fixed negative Q8_0 scale check |
| `tw_capture_int.h` | vx/vy/resid: `int32_t` → `int64_t`, cross/mag2/dot signatures updated |
| `tw_bridge.h` | Updated resid params from `int32_t` → `int64_t` (thru resid_to_cell etc.) |

---

### System Audit — What's Wired vs Disconnected

After building and testing the full pipeline, a comprehensive codebase audit revealed a critical architectural gap: **many components are defined + tested but completely disconnected from the running pipeline.**

#### TIER 1 — Wired end-to-end (actually runs and produces output)
```
RawBridge (.qdat) → TW capture → (zone, slot, resid)                    ✓
Shell 1+2+3 bridge → frozen state → wallet genesis (bond)               ✓
.gsten store write (concatenated archive with index)                     ✓
```

#### TIER 2 — Defined + tested but NOT wired into any pipeline
| Component | Location | What it does | Missing link |
|-----------|----------|-------------|--------------|
| `gb_load / gb_get / gb_decode_*` | `geom_raw_bridge.h` | .gsten reader (decode tiles → bytes) | No caller in `src/`, only tests |
| `pogls_scanner.h` | `geopixel/wallet/` | File → 64B chunks → ThetaCoord → callback | Not included anywhere in `collection/` |
| `pogls_reconstruct.h` | `geopixel/wallet/` | Reconstruct pipeline (init/full/chunk) | Only used by `core/` standalone tests |
| `pogls_coord_wallet.h` | `geopixel/wallet/` | .pogwallet format — store/read coordinates | Not called from `collection/src/` or `tests/` |
| `pogls_recon_file.h/c` | `geopixel/wallet/` | File-backed I/O for WeightStream | Only used by `core/` |
| `fibo_tile_dispatch.h` | `hbv_bundle/fgls/` | ScanEntry → FiboLayer → Geopixel tile bridge | Not included anywhere outside `fgls/` |
| `fibo_layer_header.h` | `hbv_bundle/fgls/` | Fibo Layer Clock Header — 16 routes | Same |
| `rewind_store / rewind_find` | `core/core/geo_rewind.h` | 972-slot O(1) state buffer | Only used by FEC code + `frustum/` tests |
| `hamburger_encode` | `geopixel/hamburger_encode.h` | GPX5 encode: tile → classify → dispatch → compress | Tests + demo main() only. Not in `src/` |
| `hb_vault.h` | `geopixel/hbv_bundle/` | Vault packer | Test CLI only |
| `ggml_backend_pwc.h` | `collection/` root | llama.cpp backend for weight offload | No include in any runner |

#### TIER 3 — Written but never read back
| Artifact | Write path | Read path | Status |
|----------|-----------|-----------|--------|
| `.gsten` | `test_full_pipeline.c` | `gb_load` exists but not called | Written but orphan |
| wallet entries | `tw_bridge()` → genesis bond | no serialize → no deserialize | In-memory only |
| `.gsidx/.gsdat` | Python `build_smollm2_store.py` | `geo_store_reader.h` exists but not linked | Orphan |

#### TIER 4 — Parallel code islands (duplicated, structurally independent)
- `geopixel/hbv_bundle/` — copies of geopixel_v21 codec stack
- `core/` — wallet/reconstruct implementations
- `frustum/` — rewind/FEC test suites (phase9/10/11)
- `Hfolder/` — iteration copies of geopixel_v21

#### TIER 5 — Headers with zero test coverage
`geo_frame_seek_wang.h`, `geo_metatron_reshape.h`, `bermuda_export.h`, `bond_to_geopixel.h`, `ctd_g10.h`, `ctd_goldberg.h`, `ctd_pent5hex.h`, `ctd_shell.h`, `geo_diamond_to_scan.h`, `geo_dual_place.h`, `geom_weight_reconstruct.h`, `ggml_backend_pwc.h`, `pogls_bond_chain.h`

---

### Architecture: Codec Stack & Compression Reality

**hex_tile on Q8_0 data** — ratio = 1.1429 (EXPANDS 14%). 85% EDGE classification. Q8_0 int8 quants are spatially uncorrelated → no geometric structure to exploit.

**ZSTD-16 on Q8_0 data** — 95.9% (4% savings only). Q8_0 is at its entropy limit.

**Conclusion**: Q8_0 is its own best storage format. The routing layer (.twidx, 74 KB) is the only meaningful compression — 5000× reduction from 367 MB. Store raw, route by coordinate.

```
QR8 store (367 MB)             .twidx (74 KB)
  ├── blk.0.ffn_up.weight      z=7 s=42 resid=(-4.1B,-1.3B)
  ├── blk.0.attn_v.weight      z=3 s=23 resid=(+1.6B,-1.8B)
  └── ...                     → coordinate-only, no dequant needed at runtime
```

---

### Next Session: 12-Face Bridge — TW → Full TRing 720

**Current state**: TW capture operates on a SINGLE face (10 sectors × 6 slots = 60 positions). TRing = 720 = 12 faces × 60 slots. `geo_rewind.h` has 972 buffer slots (720 TRing + 252 lookahead). `geo_frame_seek.h` has 1440-tick timeline with O(1) position decomposition.

**Gap**: TW bridge maps 1 face but does NOT iterate across all 12 dodecahedron faces to produce a full TRing coordinate. The coordinate system is unified (TRing 720, `geo_frame_seek.h` timeline, rewind buffer 972 slots) but TW lives on 1 face only.

**Plan**:
1. Extend `tw_bridge.h` to iterate TW capture across all 12 faces → produce `(face=0..11, zone, slot, resid)` per tensor
2. Map face+zone+slot → TRing position (0..719) using `tring_pos()` or `geo_frame_seek.h` `frame_enc()`
3. Connect to `geo_rewind.h` — store coord → slot in the 972-buffer with O(1) `rewind_store()`
4. Wire wallet serialize: frozen entries → actual `.pogwallet` file on disk via `pogls_coord_wallet.h`
5. Wire `.gsten` reader: `gb_load()` + `gb_decode_tensor()` so stored tensors can be read back

**Why 12-face first?**
- Solves the core disconnect: TW coordinate → full geometric coordinate → wallet/rewind all speak the same language
- Unlocks `geo_rewind.h` integration (backpressure KV eviction)
- Unlocks `geo_frame_seek.h` timeline (tick-aware position, not just zone+slot)
- Makes Shell 3 freeze address meaningful (position in full dodeca, not 1 face)

**Files to create:**
- `tw_face_bridge.h` — iterate 12 faces, map to TRing 720

**Files to enhance:**
- `tw_bridge.h` — add face index parameter, TRing position output
- `geom_raw_bridge.h` — wire `gb_load` into pipeline
- Wallet: `pogls_coord_wallet.h` — connect to Shell 3 freeze path

**Test:**
- Verify all 290 tensors map to unique or well-distributed TRing positions across 720 slots (not just 60)
- Verify rewind store/find roundtrip
- Verify wallet serialize → read back → match original coordinate

---

### Files Summary

| File | Purpose | Status |
|------|---------|--------|
| `collection/tw_capture_int.h` | Integer-only TW capture, SCALE=207360, int64 vx/vy/resid | Active |
| `collection/tw_tensor_capture.h` | Q8_0/F32 dequant → 2D sig → TW capture | **NEW** |
| `collection/tw_bridge.h` | TW→POGLS 3-shell bridge | Active, needs 12-face |
| `collection/geom_raw_bridge.h` | RawBridge (.qdat) + GeomBridge (.gsten) | Enhanced (dtype field) |
| `collection/hex_codec.h` + `src/hex_codec_impl.c` | Hex tile v3 codec | Standalone (no pipeline use) |
| `collection/geo_rewind.h` | 972-slot state buffer | Orphan (not wired) |
| `collection/geo_frame_seek.h` | 1440-tick O(1) timeline | Orphan (not wired) |
| `collection/tests/test_full_pipeline.c` | Full pipeline test | **NEW** |
| `collection/tests/test_tw_tensor_capture.c` | TW capture on real tensors | **NEW** |
| `collection/tests/test_tw_bridge.c` | 97 bridge tests | Active |
| `collection/tests/test_tw_capture.c` | Integer capture test | Active |
| `build/smollm2.gsten` | Unified 290-tensor store | **NEW** |
| `build/smollm2_tw.twidx` | TW routing index (74 KB) | Python-generated |

---

## Prompt Engineering Expert Skill (integrated from `skills/prompt-engineering-expert/`)

### Trigger Conditions
Must use this skill when user:
- Asks to analyze, review, or improve a prompt
- Wants to create system prompt / custom instructions for an agent or skill
- Reports prompt issues (inconsistent outputs, hallucinations, vague responses, wrong format)
- Asks about prompt engineering techniques (CoT, few-shot, XML tags, role-based, prefilling, chaining)
- Wants to design a testing/evaluation framework for prompts
- Asks about anti-patterns or best practices

### Rules
- Always check if this skill applies before acting on prompt-related requests
- If it applies, it MUST be used
- Read `skills/prompt-engineering-expert/CLAUDE.md` for core instructions
- Reference `docs/` for detailed techniques, best practices, and troubleshooting
- Do not skip the debugging workflow: Identify → Analyze → Test → Fix → Validate

### Debugging Workflow (for prompt issues)
1. **Identify**: What's not working? How does it fail?
2. **Analyze**: Is objective clear? Instructions specific? Context sufficient? Format specified?
3. **Test**: Try more context, specificity, examples, format changes
4. **Fix**: Update prompt, verify with multiple inputs
5. **Validate**: Does it generalize? Is it efficient?

### Quick Anti-Pattern Reference
| Issue | Fix |
|-------|-----|
| Inconsistent | Add format spec + examples |
| Hallucinations | Ask for sources + confidence levels |
| Vague | Add specific details + examples |
| Wrong format | Show exact format example |
| Doesn't generalize | Use variables, handle variations |
