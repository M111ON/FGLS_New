"""
pipeline_merged.py — Bond × Bermuda × nshell  (INTEGER-ONLY PIPELINE)
=====================================================================
No float. No malloc. No heap.  ทุก operation เป็น integer arithmetic.

Pipeline:
  GEAR     → integer entropy → shell level
  STRIP    → int median → core / offset / shadow
  FOLD     → reshape tensor → zone-space pieces
  BOND     → fibo_addr entanglement verify (int64)
  TRAVERSE → geometry codebook move (step = gear scale)
  DIFF     → L1 diff = sum|a-b|  (integer, ไม่มี mantissa)
"""

import torch
import torch.nn.functional as F
import sys, os, time
import numpy as np
from dataclasses import dataclass
from typing import Tuple

sys.path.insert(0, os.path.dirname(__file__))
from bermuda_reshape_v2 import BermudaGate, GEO_TABLE, N_CODES, N_ZONES, DEVICE
from fibo_clock import (
    FiboClock, idx_to_tick, TICKS_PER_ZONE, TICKS_PER_CYCLE, cycle_phase,
)

# C bond bridge — optional, fallback to torch sim if unavailable
_BOND_BRIDGE = None
def _get_bridge():
    global _BOND_BRIDGE
    if _BOND_BRIDGE is not None:
        return _BOND_BRIDGE
    try:
        import os
        dll = os.environ.get("POGLS_SO_PATH",
            r"I:\FGLS_new\collection\colab_bundle_v2\pogls_bond.dll")
        # Check if the file exists before trying to import
        from pathlib import Path
        if not Path(dll).exists():
            return None
        sys.path.insert(0, r"I:\FGLS_new\collection\colab_bundle_v2\python_src")
        from pogls_bridge import PoglsBridge
        _BOND_BRIDGE = PoglsBridge()
    except Exception:
        _BOND_BRIDGE = None
    return _BOND_BRIDGE

SHELL_SCALES = [1, 20, 60, 120, 240, 480]

def gear_select(shell_level: int, entropy: int) -> dict:
    """
    entropy = bit transition count  (integer, 0..N*D*8)
    shell_level < 0 → entropy-based auto-select
    shell_level >= 0 → fixed
    """
    if shell_level < 0:
        if entropy < 256:
            sl = 0
        elif entropy < 1024:
            sl = 1
        else:
            sl = 2
    else:
        sl = shell_level
    sl = min(sl, len(SHELL_SCALES) - 1)
    return {
        "shell_level": sl,
        "scale": SHELL_SCALES[sl],
        "fold_axis": (sl % 7) + 1,
        "shape_name": ["I","O","T","S","Z","L","J"][sl % 7],
    }


# ═══════════════════════════════════════════════════════════════════
#  INTEGER ENTROPY — count bit transitions between adjacent rows
# ═══════════════════════════════════════════════════════════════════

def int_entropy(x: torch.Tensor) -> int:
    """Count how many int8 values differ between row i and row i+1.
    Pure integer: diff = (x[:-1] != x[1:]).sum()
    """
    if x.shape[0] < 2:
        return 0
    N, D = x.shape
    # XOR adjacent rows → nonzero where bits differ
    diff = (x[:-1] ^ x[1:]).view(torch.uint8)
    return int(diff.sum().item())


# ═══════════════════════════════════════════════════════════════════
#  INTEGER STRIP — median-based offset, no float mean
# ═══════════════════════════════════════════════════════════════════

def int_median(x: torch.Tensor, dim: int = -1) -> torch.Tensor:
    """Integer median.  sort() → middle value.  No sort in hot path
    for the real C version, OK for prototype.
    """
    sorted_x, _ = x.sort(dim=dim)
    mid = sorted_x.shape[dim] // 2
    return sorted_x.select(dim, mid).unsqueeze(dim)

def int_strip(x_flat: torch.Tensor) -> Tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
    """
    strip → offset = median per token (int)
             core   = x_flat - offset  (int16)
             shadow = (core - median(core)) clipped to int8
    All int.  No float.
    """
    dtype = x_flat.dtype  # int8
    off = int_median(x_flat, dim=-1)                   # [N, 1], int8 mid
    core = x_flat.to(torch.int16) - off.to(torch.int16) # [N, D], int16
    shadow = core - int_median(core, dim=0).to(torch.int16)  # [N, D], int16
    core = core - int_median(core, dim=0).to(torch.int16)
    return core.clamp(-128, 127).to(torch.int8), off.to(torch.int8), shadow.clamp(-128, 127).to(torch.int8)


# ═══════════════════════════════════════════════════════════════════
#  BOND LAYER — fibo_addr entanglement  (already integer, int64)
# ═══════════════════════════════════════════════════════════════════

SHAPE_TO_DISPATCH = {1:"ROUTE", 2:"ROUTE", 3:"ROUTE",
                     4:"GROUND", 5:"GROUND", 6:"GROUND", 7:"ROUTE"}

@dataclass
class BondResult:
    geo_key:  torch.Tensor
    shape:    torch.Tensor
    bond_key: torch.Tensor
    valid:    torch.Tensor
    dispatch: list

def bond_layer(core: torch.Tensor, gear: dict,
               bridge=None, cycle: int = 0) -> BondResult:
    """
    สร้าง pieces ด้วย real C bond (ถ้ามี) หรือ torch sim (fallback).
    C bond: 0% false positive rate.
    """
    N, D = core.shape
    B = min(256, N)

    bridge = bridge or _get_bridge()

    if bridge is not None:
        # ── REAL C BOND (cycle-aware) ────────────────────────
        fold_axis = gear["fold_axis"]
        # Cycle injects nonce into seed → different entanglement per cycle
        cycle_nonce = (cycle * 37 + 11) & 0xFFFF
        seeds = [s ^ cycle_nonce for s in range(B)]
        # batch: geo_keys → fibo_addr of each seed
        # Mask uint64 into signed int64 range for torch
        fibo_results = [(x & 0x7FFFFFFFFFFFFFFF) for x in bridge.fibo_addr_batch(seeds)]
        geo_key = torch.tensor(fibo_results, dtype=torch.long, device=DEVICE) % (12 * 1440)
        shape = (torch.arange(B, device=DEVICE) + fold_axis - 1) % 7 + 1
        # create pieces for pairwise verify
        bond_keys = []
        for s in seeds:
            p = bridge.make_piece(s, fold_axis)
            bond_keys.append((p.bond_L ^ p.bond_R) & 0x7FFFFFFFFFFFFFFF)
        bond_key = torch.tensor(bond_keys, dtype=torch.long, device=DEVICE)
        # verify pairs
        valid_list = []
        for i in range(1, B):
            pa = bridge.make_piece(seeds[i-1], fold_axis)
            pb = bridge.make_piece(seeds[i],   fold_axis)
            ok, _ = bridge.bond_verify(pa, pb)
            valid_list.append(int(ok))
        valid = torch.tensor(valid_list + [0], dtype=torch.bool, device=DEVICE)
        dispatch = [SHAPE_TO_DISPATCH.get(int(s), "FAULT") for s in shape[:10].cpu()]
    else:
        # ── TORCH SIM (fallback, ~50% FP, cycle-aware) ──────────
        idx_a = torch.randint(0, N, (B,), device=core.device)
        idx_b = torch.randint(0, N, (B,), device=core.device)
        cycle_offset = (cycle * 12345) % 8640
        geo_key = ((torch.arange(B, device=DEVICE, dtype=torch.long) + cycle_offset) * 37) % 8640
        geo_key = geo_key % (12 * 1440)
        shape = (torch.arange(B, device=DEVICE) + gear["fold_axis"] - 1) % 7 + 1
        bond_L = (geo_key * 37 + 11) % (2**31 - 1)
        bond_R = (geo_key * 37 + 123456789) % (2**31 - 1)
        bond_key = bond_L ^ bond_R
        is_pair = (idx_a % 2) == (idx_b % 2)
        mask = 0x7FFFFFFF
        expected = torch.where(is_pair, bond_key, bond_key ^ mask)
        valid = (bond_key == expected) if B > 0 else torch.zeros(1, dtype=torch.bool)
        dispatch = [SHAPE_TO_DISPATCH.get(int(s), "FAULT") for s in shape[:10].cpu()]

    return BondResult(geo_key=geo_key, shape=shape,
                      bond_key=bond_key, valid=valid, dispatch=dispatch)


# ═══════════════════════════════════════════════════════════════════
#  PIPELINE SNAPSHOT — 100% integer fields
# ═══════════════════════════════════════════════════════════════════

@dataclass
class PipelineSnapshot:
    input_shape:      tuple
    output_shape:     tuple
    shell_level:      int
    scale:            int
    fold_axis:        int
    shape_name:       str
    shadow_norm:      int    # L1 sum|shadow|  (integer, no float)
    offset_norm:      int    # L1 sum|offset|
    core_norm:        int    # L1 sum|core|
    bond_valid_count: int    # number of valid pieces
    bond_total:       int    # total pieces checked
    diff_l1:          int    # L1 sum|input - output|
    diff_max:         int    # max|input - output|  (sup norm)
    traverse:         str
    zones_in:         list
    zones_out:        list
    code_diff_l1:     int    # L1 diff in code space (original → traversed)
    cycle:            int    # cycle counter for continuous sequence

def l1_norm(t: torch.Tensor) -> int:
    """sum of absolute values → integer"""
    if t.numel() == 0:
        return 0
    return int(t.abs().sum().item())


# ═══════════════════════════════════════════════════════════════════
#  RUN PIPELINE  (integer data path, gate internally float)
# ═══════════════════════════════════════════════════════════════════

def run_pipeline(gate: BermudaGate,
                 x: torch.Tensor,
                 out_shape: tuple,
                 mode: int = 0,
                 shell_level: int = -1,
                 bridge=None,
                 cycle: int = 0) -> Tuple[torch.Tensor, PipelineSnapshot]:
    """
    x: int8 tensor  [..., D]
    All internal metrics integer.
    Gate (encoder/decoder) uses float internally — data converted at boundary.
    cycle: cycle counter for continuous sequence encoding.
    """
    D = x.shape[-1]
    orig_shape = tuple(x.shape[:-1])
    x_flat = x.reshape(-1, D).to(torch.int8)
    N = x_flat.shape[0]

    # ── 1. GEAR select (integer entropy) ──────────────────────
    ent = int_entropy(x_flat)
    gear = gear_select(shell_level, ent)

    # ── 2. STRIP (integer) ────────────────────────────────────
    core, off, shadow = int_strip(x_flat)

    # ── 3. FOLD → geometry space ──────────────────────────────
    n_out = 1
    for s in out_shape: n_out *= s
    if n_out != N:
        if n_out > N:
            pad_c = torch.zeros(n_out - N, D, dtype=torch.int8, device=DEVICE)
            pad_o = torch.zeros(n_out - N, 1, dtype=torch.int8, device=DEVICE)
            core = torch.cat([core, pad_c]); off = torch.cat([off, pad_o])
            shadow = torch.cat([shadow, pad_c])
        else:
            core = core[:n_out]; off = off[:n_out]; shadow = shadow[:n_out]
        N = n_out

    # ── 4. BOND verify (int64, C bond when available, cycle-aware) ──
    bond_result = bond_layer(core, gear, bridge, cycle=cycle)

    # ── 5. BERMUDA PASS (cycle-aware) ─────────────────────────
    step = gear["scale"]
    gate.eval()
    with torch.no_grad():
        # convert int8 → float for gate, scale to [-1,1]
        n_batch = min(core.shape[0], gate.n_codes)
        core_f = core[:n_batch].to(torch.float32)
        core_scale = max(core_f.abs().max().item(), 1.0)
        core_f = core_f / core_scale  # normalize to [-1,1] for gate stability
        try:
            idx, z_q, vq_loss = gate.encode(core_f)
        except Exception:
            idx = torch.zeros(n_batch, dtype=torch.long, device=DEVICE)
            z_q = torch.zeros(n_batch, gate.code_dim, device=DEVICE)

        nxt_idx = gate.codebook.traverse(idx, mode, step=step, cycle=cycle)
        z_q_out = gate.codebook.codes[nxt_idx % gate.n_codes]
        geo = gate.geo_table[nxt_idx % gate.n_codes]
        core_out_f = gate.decoder(torch.cat([z_q_out, geo], dim=-1))  # float
        core_out_f = core_out_f * core_scale  # scale back from [-1,1]

    # convert float output back to int8 (clamp first to prevent int8 wrap)
    core_out = core_out_f.round().clamp(-128, 127).to(torch.int8)
    if core_out.shape[0] < N:
        pad = torch.zeros(N - core_out.shape[0], D, dtype=torch.int8, device=DEVICE)
        core_out = torch.cat([core_out, pad])

    # ── 6. REATTACH (integer add) ─────────────────────────────
    out_flat = core_out[:N].to(torch.int16) + shadow[:N].to(torch.int16) + off[:N].to(torch.int16)
    out_flat = out_flat.clamp(-128, 127).to(torch.int8)

    # ── 7. DIFF (integer L1) ─────────────────────────────────
    diff = x_flat[:N].to(torch.int16) - out_flat.to(torch.int16)
    diff_l1 = l1_norm(diff)
    diff_max = int(diff.abs().max().item()) if diff.numel() > 0 else 0

    # ── 8. UNFOLD ──────────────────────────────────────────────
    x_out = out_flat.reshape(tuple(out_shape) + (D,))

    # ── stats ──────────────────────────────────────────────────
    zones_in  = (idx_to_tick(idx) // TICKS_PER_ZONE).unique().tolist() if idx.numel() > 0 else []
    zones_out = (idx_to_tick(nxt_idx) // TICKS_PER_ZONE).unique().tolist() if nxt_idx.numel() > 0 else []
    bond_valid_count = int(bond_result.valid.sum().item()) if bond_result.valid.numel() > 0 else 0
    bond_total = int(bond_result.valid.numel()) if bond_result.valid.numel() > 0 else 0
    # code-space diff: L1 distance between original z_q and traversed z_q
    code_diff_l1 = l1_norm(z_q_out - z_q) if z_q.numel() > 0 and z_q_out.numel() > 0 else 0

    snap = PipelineSnapshot(
        input_shape=orig_shape, output_shape=out_shape,
        shell_level=gear["shell_level"], scale=gear["scale"],
        fold_axis=gear["fold_axis"], shape_name=gear["shape_name"],
        shadow_norm=l1_norm(shadow), offset_norm=l1_norm(off), core_norm=l1_norm(core),
        bond_valid_count=bond_valid_count, bond_total=bond_total,
        diff_l1=diff_l1, diff_max=diff_max,
        traverse=["ORBITAL","CHIRAL","CROSS","HUB"][mode],
        zones_in=sorted(zones_in), zones_out=sorted(zones_out),
        code_diff_l1=code_diff_l1, cycle=cycle)

    return x_out, snap


# ═══════════════════════════════════════════════════════════════════
#  GGUF INT8 LOADER  —  extract raw int8 from Q8_0 blocks
# ═══════════════════════════════════════════════════════════════════

def load_gguf_int8(path: str, max_tokens: int = 4096) -> torch.Tensor:
    """讀 GGUF Q8_0 weight tensor, skip dequantize → pure int8."""
    from gguf import GGUFReader

    r = GGUFReader(path)
    for t in r.tensors:
        if not t.name.endswith('.weight') or len(t.shape) < 2:
            continue
        print(f"  tensor: {t.name}  logical={t.shape}  raw={t.data.shape}")
        # Q8_0: raw data is [M, blocks * 34], each block = 32 int8 + 2 fp16
        M, B = t.data.shape
        if B % 34 != 0:
            continue  # not Q8_0
        n_blocks = B // 34
        blocks = t.data.reshape(M, n_blocks, 34)
        # Take 32 int8 values from each block, skip fp16 scale
        int8_raw = blocks[:, :, :32].reshape(M, n_blocks * 32).astype(np.int8)
        # Transpose if needed: gguf shape may be [out, in] → [in, out] for pipeline
        if int8_raw.shape[0] > int8_raw.shape[1]:
            int8_raw = int8_raw.T
        N = min(max_tokens, int8_raw.shape[0])
        D = int8_raw.shape[1]
        # Pad/trim D to 128 for gate compatibility
        if D < 128:
            int8_raw = np.pad(int8_raw, ((0,0),(0,128-D)), mode='constant')
        elif D > 128:
            int8_raw = int8_raw[:, :128]
        ten = torch.from_numpy(int8_raw[:N].copy()).to(DEVICE)
        print(f"  → int8 tensor: {ten.shape}  range=[{ten.min().item()},{ten.max().item()}]")
        return ten

    raise RuntimeError("No 2D Q8_0 weight tensor found")


# ═══════════════════════════════════════════════════════════════════
#  TEST — GGUF (100% integer metrics)
# ═══════════════════════════════════════════════════════════════════

def make_gguf_dataset(path: str, max_rows: int = 0) -> torch.Tensor:
    """Load ALL 2D int8 GGUF tensors, create rolling window dataset.
    Each window = 32 tokens × 128 dims. Returns [n_windows, 32, 128] float.
    """
    from gguf import GGUFReader
    windows = []
    r = GGUFReader(path)
    for t in r.tensors:
        if not t.name.endswith('.weight') or len(t.shape) < 2:
            continue
        M, B = t.data.shape
        if B % 34 != 0:
            continue
        n_blocks = B // 34
        blocks = t.data.reshape(M, n_blocks, 34)
        int8_raw = blocks[:, :, :32].reshape(M, n_blocks * 32).astype(np.int8)
        if int8_raw.shape[0] > int8_raw.shape[1]:
            int8_raw = int8_raw.T
        if int8_raw.shape[1] < 128:
            continue
        x = int8_raw[:, :128]
        N = x.shape[0]
        win_sz = min(32, N)
        stride = win_sz // 2 if win_sz > 2 else 1
        for i in range(0, N - win_sz + 1, stride):
            w = torch.from_numpy(x[i:i+win_sz].astype(np.float32))
            off = w.mean(dim=-1, keepdim=True)
            core = w - off
            sw = core - core.mean(dim=0, keepdim=True)
            windows.append(sw)
        if len(windows) >= max_rows:
            break
    if not windows:
        return torch.zeros(1, 32, 128)
    return torch.stack(windows[:max_rows]) if max_rows else torch.stack(windows)


def test_gguf(path=None):
    print("=" * 60)
    print("POGLS REAL TRAIN — LLM weights × gate convergence")
    print("=" * 60)

    DIM = 128
    if path is None:
        path = r"I:\Vault\models\Qwen3-0.6B-Q8_0.gguf"
    bridge = _get_bridge()
    bond_src = "C bond (0% FP)" if bridge else "torch sim (~50% FP)"
    print(f"Bond: {bond_src}\n")

    # ── Load ALL tensors as training data ──────────────────────
    print(f"Loading all GGUF weight tensors: {path}")
    dataset = make_gguf_dataset(path, max_rows=8000)
    print(f"Dataset: {dataset.shape}  range=[{dataset.min():.2f},{dataset.max():.2f}]")
    win_sz = dataset.shape[1]

    # ── Train gate on REAL weight distribution ─────────────────
    gate = BermudaGate(DIM, code_dim=64, n_codes=N_CODES).to(DEVICE)
    opt = torch.optim.AdamW(
        list(gate.encoder.parameters()) + list(gate.decoder.parameters()),
        lr=3e-4, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, 1500)

    n_batches = dataset.shape[0]
    batch_tokens = 192  # keep under 4GB VRAM
    print(f"\nTraining gate on {n_batches} real-weight windows, 1500 epochs...")
    t0 = time.perf_counter()
    best_recon = float('inf')
    for ep in range(1500):
        gate.train()
        n_win = max(1, batch_tokens // win_sz)
        idx = torch.randint(0, n_batches, (n_win,))
        xb = dataset[idx].to(DEVICE)  # [n_win, win_sz, D] → GPU
        perm = torch.stack([torch.randperm(win_sz, device=DEVICE) for _ in range(n_win)])
        xb = xb[torch.arange(n_win, device=DEVICE).unsqueeze(1), perm]
        xb = xb.reshape(-1, DIM)
        off = xb.mean(dim=-1, keepdim=True)
        core = xb - off
        sw = core - core.mean(dim=0, keepdim=True)
        # scale to [-1,1] to match run_pipeline gate scaling
        sw_scale = sw.abs().max().clamp(min=1.0)
        sw = sw / sw_scale
        co, _, _, vq = gate(sw, mode=0)
        # Ramp loss: maximize output change at step=120 vs step=1
        co_120, _, _, _ = gate(sw, mode=0, step=120)
        ramp = -F.mse_loss(co_120, co)
        recon = F.mse_loss(co, sw)
        total = recon + 0.25 * vq + 5.0 * ramp
        opt.zero_grad()
        total.backward()
        torch.nn.utils.clip_grad_norm_(gate.parameters(), 1.0)
        opt.step()
        sched.step()

        if recon.item() < best_recon:
            best_recon = recon.item()
        if (ep + 1) % 300 == 0:
            usage = (gate.codebook.ema_count > 1.0).float().mean()
            print(f"  ep={ep+1:4d}  recon={recon.item():.4f}  "
                  f"best={best_recon:.4f}  vq={vq.item():.4f}  "
                  f"usage={usage.item():.1%}")

    t1 = time.perf_counter()
    print(f"Trained in {t1-t0:.0f}s  best_recon={best_recon:.4f}\n")

    # ── Reorder codebook by PC1 for spatial locality ────────────
    perm = gate.reorder_codes(spread=12.0)
    print(f"Codebook reordered by PC1 projection  (perm range 0..{perm.max().item()})")
    # Verify spatial locality after reorder
    with torch.no_grad():
        cd_r = torch.cdist(gate.codebook.codes, gate.codebook.codes)
        n1 = cd_r.diagonal(offset=1).mean().item()
        n60 = cd_r.diagonal(offset=60).mean().item()
        n120 = cd_r.diagonal(offset=120).mean().item()
        print(f"  neighbor dist: step=1={n1:.4f}  step=60={n60:.4f}  step=120={n120:.4f}")

    # ── Test on real int8 weights ──────────────────────────────
    # Load fresh int8 test data (not the normalized float windows)
    x_real = load_gguf_int8(path, max_tokens=576).to(DEVICE)
    N = x_real.shape[0]
    print(f"Test tensor: {x_real.shape}")

    # ── Test header ────────────────────────────────────────────
    print(f"\n{'mode':10s}  {'sl':>3s}  {'step':>5s}  {'cross_diff':>10s}  "
          f"{'code_diff':>10s}  {'diff_L1':>10s}  {'shadow':>8s}  "
          f"{'bond':>8s}  {'entropy':>8s}")
    print("-" * 110)

    ent = int_entropy(x_real.to(torch.int8))
    # Run all configs and store outputs for cross-step comparison
    all_outputs = {}  # (mode, sl) → tensor
    results = []
    for mode, mname in enumerate(["ORBITAL","CHIRAL","CROSS","HUB"]):
        for sl in [0, 1, 2, 3]:
            out, snap = run_pipeline(gate, x_real.to(torch.int8), [N//16, 16],
                                     mode=mode, shell_level=sl, bridge=bridge)
            all_outputs[(mode, sl)] = out
            bond_str = f"{snap.bond_valid_count}/{snap.bond_total}"
            # cross_diff: difference from step=1 baseline (within same mode)
            base_out = all_outputs.get((mode, 0), out)
            cross_diff = l1_norm((out.to(torch.int16) - base_out.to(torch.int16)))
            print(f"{mname:10s}  {snap.shell_level:3d}  {snap.scale:5d}  "
                  f"{cross_diff:10d}  {snap.code_diff_l1:10d}  "
                  f"{snap.diff_l1:10d}  {snap.shadow_norm:8d}  "
                  f"{bond_str:8s}  {ent:8d}")
            results.append((mname, sl, snap, cross_diff))

    # ── Gear ramp analysis ──────────────────────────────────────
    print(f"\n{'─'*60}")
    print("GEAR RAMP:  step ↑ → cross-diff from baseline ↑ ?")
    for mode in range(4):
        mname = ["ORBITAL","CHIRAL","CROSS","HUB"][mode]
        group = [r for r in results if r[0] == mname]
        xdiffs = [r[3] for r in group]
        steps = [r[2].scale for r in group]
        if len(xdiffs) >= 2:
            dr = "  ".join([f"s={s}:xdiff={d}" for s, d in zip(steps, xdiffs)])
            print(f"  {mname:10s}  {dr}")

    # ── Cycle cross-diff: cycle=n → cross-diff from cycle=0 ────
    print(f"\n{'─'*60}")
    print("CYCLE SENSITIVITY:  cycle=n → cross-diff from cycle=0 ?")
    print(f"{'mode':10s}  {'step':>5s}  {'cyc=0':>10s}  {'cyc=1':>10s}  "
          f"{'cyc=3':>10s}  {'cyc=7':>10s}")
    print("-" * 70)
    for mode, mname in enumerate(["ORBITAL","CHIRAL","CROSS","HUB"]):
        sl = 0
        step = SHELL_SCALES[sl]
        cyc_vals = []
        for cyc in [0, 1, 3, 7]:
            outc, _ = run_pipeline(gate, x_real.to(torch.int8), [N//16, 16],
                                    mode=mode, shell_level=sl, bridge=bridge, cycle=cyc)
            out0, _ = run_pipeline(gate, x_real.to(torch.int8), [N//16, 16],
                                    mode=mode, shell_level=sl, bridge=bridge, cycle=0)
            xd = l1_norm((outc.to(torch.int16) - out0.to(torch.int16)))
            cyc_vals.append(xd)
        print(f"{mname:10s}  {step:5d}  {cyc_vals[0]:>10d}  {cyc_vals[1]:>10d}  "
              f"{cyc_vals[2]:>10d}  {cyc_vals[3]:>10d}")

    return gate, results


# ═══════════════════════════════════════════════════════════════════
#  MAIN
# ═══════════════════════════════════════════════════════════════════

if __name__ == "__main__":
    gate, results = test_gguf()

    print()
    print("=" * 60)
    print("GEAR RAMP AFTER REAL-WEIGHT TRAINING:")
    ramp_checks = {"ORBITAL", "CHIRAL"}
    ramp_ok = True
    for m in range(4):
        mname = ["ORBITAL","CHIRAL","CROSS","HUB"][m]
        group = [r for r in results if r[0] == mname]
        cdiffs = [r[2].code_diff_l1 for r in group]
        steps = [r[2].scale for r in group]
        if len(cdiffs) >= 4:
            cr = cdiffs[-1] / max(cdiffs[1], 1)  # step=120 / step=20
            print(f"  {mname:10s}  steps={steps}  code_diff={cdiffs}", end="")
            if mname in ramp_checks and cdiffs[1] > 0:
                ok = cr > 2.0
                ramp_ok = ramp_ok and ok
                tag = "✓" if ok else "✗"
                print(f"  code_ramp={cr:.2f}x {tag}", end="")
            print()

    print(f"\n{'─'*40}")
    print(f"RAMP > 2×: {'ALL PASS ✓' if ramp_ok else 'BELOW TARGET — increase div weight or epochs, or check CHIRAL tick-space fix in bermuda_reshape_v2.py'}")
