# POGLS Golden Run Baseline (Colab T4)

Date: 2026-05-19

This document locks the known-good configuration and validation steps that produced:

- ORBITAL `code_ramp = 6.36x`
- CHIRAL `code_ramp = 2.63x`
- `RAMP > 2x: ALL PASS`

## Locked Baseline

### 1) CHIRAL traverse mapping (no step=120 foldback)

File: `bermuda_reshape_v2.py`

- CHIRAL now maps through full tick-space and then quantizes back to code-space:
  - `idx_full = tick_to_idx(new_tick)`
  - `ticks_per_code = TICKS_PER_CYCLE // n_codes` (for 1440/240 = 6)
  - `result = (idx_full // ticks_per_code) % n_codes`

Reason: avoids identity collapse at `step=120` that previously forced `code_diff=0`.

### 2) Safe int8 cast path (clamp before cast)

File: `pipeline_merged.py`

- Decoder output cast:
  - From: `round -> int8 -> clamp`
  - To: `round -> clamp(-128,127) -> int8`

- `int_strip` outputs:
  - `core` and `shadow` are clamped to `[-128,127]` before `.to(torch.int8)`.

Reason: prevents int8 wraparound corruption on out-of-range values.

### 3) Ramp evaluation metric

File: `colab_run.py` (+ summary block in `pipeline_merged.py`)

- Ramp check uses `code_diff_l1` ratio at `step=120 / step=20`.
- Active ramp checks: ORBITAL and CHIRAL only.
- CROSS and HUB are reported but not used as pass/fail gates.

Reason: CROSS/HUB are invariant modes by design; code-space ramp target belongs to traversal modes.

### 4) Codebook spread

File: `pipeline_merged.py`

- Reorder spread locked at `spread=12.0`.

Reason: increases code-space separation enough for >2x ramp target.

## Golden Run Output Snapshot

From Phase 5:

- ORBITAL code diff: `[265, 308, 756, 1960]` -> `6.36x`
- CHIRAL code diff: `[736, 385, 435, 1012]` -> `2.63x`
- CROSS code diff: `[697, 697, 697, 697]`
- HUB code diff: `[869, 869, 869, 869]`
- Final status: `RAMP > 2x: ALL PASS`

## Re-run Checklist (Colab)

1. Upload latest `colab_bundle_v2.zip` to `/content/`.
2. Extract to `/content/colab_bundle_v2`.
3. Ensure GGUF exists at `/content/Qwen3-0.6B-Q8_0.gguf` (auto-download supported).
4. Run:

```bash
python3 /content/colab_bundle_v2/colab_run.py
```

5. Verify in Phase 5:
   - ORBITAL `code_ramp > 2.0`
   - CHIRAL `code_ramp > 2.0`
   - Final line: `RAMP > 2x: ALL PASS`

## Fast Regression Checks (must hold)

- CHIRAL at step=120 must NOT produce `code_diff=0`.
- No `round -> int8 -> clamp` cast order for decoder output.
- `int_strip` must clamp before int8 cast for `core` and `shadow`.
- Ramp pass/fail must not include CROSS/HUB.

## If Run Fails

1. Recheck files are from latest zip (stale upload is most common).
2. Confirm CHIRAL mapping block in `bermuda_reshape_v2.py` matches locked baseline.
3. Confirm cast order in `pipeline_merged.py` remains clamp-before-int8.
4. Re-run once (random init/training noise can shift margins slightly).
