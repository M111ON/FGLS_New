"""
bench_runner.py — Multi-Model FGLS/POGLS Benchmark Runner
==========================================================
Benchmarks geometric compression pipeline across multiple GGUF models.

Usage:
  python3 bench_runner.py                                    # bench all models in registry
  python3 bench_runner.py --models qwen3-0.6b-q8 smollm2-360m-q8  # specific models
  python3 bench_runner.py --max-tokens 256 --shell-levels 0 1 2    # custom config
  python3 bench_runner.py --wandb --wandb-project fgls-bench        # W&B logging

Designed for Google Colab (T4 GPU) or local execution.
"""

import os
import sys
import json
import time
import hashlib
import argparse
import subprocess
from pathlib import Path
from dataclasses import dataclass, field, asdict
from typing import List, Optional, Dict, Any

import numpy as np
import torch

# ═══════════════════════════════════════════════════════════════════
#  CONSTANTS
# ═══════════════════════════════════════════════════════════════════

SCRIPT_DIR = Path(__file__).parent
REGISTRY_PATH = SCRIPT_DIR / "model_registry.json"
DEFAULT_CACHE_DIR = Path("/content/models") if Path("/content").exists() else SCRIPT_DIR / "cache"

TRAVERSE_NAMES = ["ORBITAL", "CHIRAL", "CROSS", "HUB"]
SHELL_SCALES = [1, 20, 60, 120, 240, 480]


# ═══════════════════════════════════════════════════════════════════
#  DATA CLASSES
# ═══════════════════════════════════════════════════════════════════

@dataclass
class ModelSpec:
    id: str
    name: str
    repo: str
    file: str
    quant: str
    params: str
    url: str
    size_mb: int
    format: str = "gguf"  # "gguf" or "safetensors"

    @classmethod
    def from_dict(cls, d: dict) -> "ModelSpec":
        return cls(**{k: v for k, v in d.items() if k in cls.__dataclass_fields__})


@dataclass
class BenchResult:
    model_id: str
    model_name: str
    params: str
    quant: str
    file_size_mb: float
    tensor_count: int
    total_int8_bytes: int
    # Bond bench
    fibo_addr_ops: float = 0.0
    bond_verify_ops: float = 0.0
    # Pipeline bench (per traverse mode × shell level)
    pipeline_results: Dict[str, Dict[int, Dict]] = field(default_factory=dict)
    # Memory
    ram_peak_mb: float = 0.0
    vram_peak_mb: float = 0.0
    # Gate training
    gate_train_loss: float = 0.0
    gate_train_time_s: float = 0.0
    # Timing
    total_time_s: float = 0.0
    timestamp: str = ""


# ═══════════════════════════════════════════════════════════════════
#  MODEL REGISTRY
# ═══════════════════════════════════════════════════════════════════

def load_registry(path: Path = REGISTRY_PATH) -> List[ModelSpec]:
    with open(path) as f:
        data = json.load(f)
    return [ModelSpec.from_dict(m) for m in data["models"]]


def get_bench_config(path: Path = REGISTRY_PATH) -> dict:
    with open(path) as f:
        data = json.load(f)
    return data.get("bench_config", {})


# ═══════════════════════════════════════════════════════════════════
#  MODEL DOWNLOAD
# ═══════════════════════════════════════════════════════════════════

def download_model(spec: ModelSpec, cache_dir: Path) -> Path:
    """Download GGUF from HuggingFace if not cached."""
    cache_dir.mkdir(parents=True, exist_ok=True)
    dest = cache_dir / spec.file
    if dest.exists() and dest.stat().st_size > 1000:
        print(f"  ✓ cached: {dest.name} ({dest.stat().st_size / 1e6:.0f} MB)")
        return dest
    print(f"  ⬇ downloading {spec.name} ({spec.size_mb} MB)...")
    url = spec.url
    # curl -L follows HuggingFace 302 redirects; wget often does not
    try:
        subprocess.check_call(
            ["curl", "-L", "--progress-bar", "-o", str(dest), url],
            timeout=600
        )
    except subprocess.CalledProcessError:
        # fallback to wget with redirect
        subprocess.check_call(
            ["wget", "-q", "--max-redirect=5", "-O", str(dest), url],
            timeout=600
        )
    if not dest.exists() or dest.stat().st_size < 1000:
        raise RuntimeError(f"Download failed: {dest} ({dest.stat().st_size if dest.exists() else 0} bytes)")
    print(f"  ✓ downloaded: {dest.name} ({dest.stat().st_size / 1e6:.0f} MB)")
    return dest


# ═══════════════════════════════════════════════════════════════════
#  SAFETENSORS LOADING — raw weights → int8
# ═══════════════════════════════════════════════════════════════════

def load_safetensors_int8(path: str, max_tokens: int = 4096) -> torch.Tensor:
    """
    Load .safetensors → int8 tensor for pipeline.
    Handles FP16/BF16/FP32/INT8 weights.
    Finds largest 2D weight tensor, converts to int8 via symmetric quantize.
    """
    from safetensors.torch import load_file
    import os

    DEVICE = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    # Handle sharded files: if path is a directory, load all shards
    if os.path.isdir(path):
        import glob
        files = sorted(glob.glob(os.path.join(path, "*.safetensors")))
        if not files:
            raise FileNotFoundError(f"No .safetensors in {path}")
        state = {}
        for f in files:
            state.update(load_file(f))
    else:
        state = load_file(path)

    # Find 2D weight tensors, prefer largest
    candidates = []
    for name, tensor in state.items():
        if tensor.dim() != 2:
            continue
        if not name.endswith('.weight'):
            continue
        candidates.append((name, tensor))

    if not candidates:
        # fallback: any 2D tensor
        for name, tensor in state.items():
            if tensor.dim() == 2:
                candidates.append((name, tensor))

    if not candidates:
        raise RuntimeError("No 2D weight tensor found in safetensors")

    # Pick largest
    candidates.sort(key=lambda x: x[1].numel(), reverse=True)
    name, tensor = candidates[0]
    print(f"  selected tensor: {name}  shape={list(tensor.shape)}  dtype={tensor.dtype}")

    # Convert to float32 first (handles FP16, BF16, INT8, etc.)
    x_f32 = tensor.to(torch.float32)

    # Symmetric int8 quantize: scale = max(abs(x)) / 127
    amax = x_f32.abs().max().clamp(min=1e-6)
    scale = amax / 127.0
    x_int8 = (x_f32 / scale).round().clamp(-128, 127).to(torch.int8)

    # Transpose if needed: [out, in] → [in, out] to match GGUF convention
    if x_int8.shape[0] > x_int8.shape[1]:
        x_int8 = x_int8.T

    N = min(max_tokens, x_int8.shape[0])
    D = x_int8.shape[1]

    # Pad/trim D to 128 for gate compatibility
    if D < 128:
        x_int8 = torch.nn.functional.pad(x_int8, (0, 128 - D))
    elif D > 128:
        x_int8 = x_int8[:, :128]

    x_int8 = x_int8[:N].to(DEVICE)
    print(f"  → int8 tensor: {x_int8.shape}  range=[{x_int8.min().item()},{x_int8.max().item()}]")
    print(f"  quantize scale: {scale.item():.6f}")
    return x_int8


def inspect_safetensors(path: str) -> dict:
    """Read safetensors metadata: tensor count, sizes."""
    import os
    if os.path.isdir(path):
        import glob
        files = sorted(glob.glob(os.path.join(path, "*.safetensors")))
        if not files:
            raise FileNotFoundError(f"No .safetensors in {path}")
        from safetensors.torch import load_file
        state = {}
        for f in files:
            state.update(load_file(f))
    else:
        from safetensors.torch import load_file
        state = load_file(path)

    tensors_2d = []
    total_bytes = 0
    for name, tensor in state.items():
        if tensor.dim() == 2:
            nbytes = tensor.numel() * tensor.element_size()
            tensors_2d.append({
                "name": name,
                "shape": list(tensor.shape),
                "dtype": str(tensor.dtype),
                "nbytes": nbytes,
            })
            total_bytes += nbytes
    return {
        "tensor_count": len(tensors_2d),
        "total_bytes": total_bytes,
        "tensors": tensors_2d,
    }


# ═══════════════════════════════════════════════════════════════════
#  GGUF INSPECTION
# ═══════════════════════════════════════════════════════════════════

def inspect_gguf(path: str) -> dict:
    """Read GGUF metadata: tensor count, sizes, quant type."""
    from gguf import GGUFReader
    r = GGUFReader(path)
    tensors_2d_q8 = []
    total_int8 = 0
    for t in r.tensors:
        if not t.name.endswith('.weight') or len(t.shape) < 2:
            continue
        M, B = t.data.shape
        if B % 34 != 0:
            continue
        n_blocks = B // 34
        int8_bytes = M * n_blocks * 32
        tensors_2d_q8.append({
            "name": t.name,
            "shape": list(t.shape),
            "int8_bytes": int8_bytes,
        })
        total_int8 += int8_bytes
    return {
        "tensor_count": len(tensors_2d_q8),
        "total_int8_bytes": total_int8,
        "tensors": tensors_2d_q8,
    }


# ═══════════════════════════════════════════════════════════════════
#  BOND BENCHMARK
# ═══════════════════════════════════════════════════════════════════

def bench_fibo_addr(n: int = 100000) -> float:
    """Measure fibo_addr throughput (ops/s). Pure Python fallback."""
    POGLS_FNV_PRIME  = 0x00000100000001B3
    POGLS_FNV_OFFSET = 0xCBF29CE484222325
    POGLS_GEO_MAGIC  = 0x00120090024005A0
    FIBO = [1,1,2,3,5,8,13,21,34,55,89,144,233,377,610,987]

    seeds = [i * 0x9E3779B97F4A7C15 for i in range(n)]
    t0 = time.perf_counter()
    for s in seeds:
        h = POGLS_FNV_OFFSET
        for i in range(8):
            h ^= (s >> (i * 8)) & 0xFF
            h = (h * POGLS_FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
        for i in range(16):
            h ^= (FIBO[i] * (s >> (i & 7))) & 0xFFFFFFFFFFFFFFFF
            h = ((h << 13) | (h >> 51)) & 0xFFFFFFFFFFFFFFFF
        h ^= POGLS_GEO_MAGIC
        h = (h * POGLS_FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
        h ^= h >> 33
    dt = time.perf_counter() - t0
    return n / dt


def bench_int_entropy(n_rows: int = 1000, dim: int = 128) -> dict:
    """Measure integer entropy computation on random int8 data."""
    x = torch.randint(-128, 127, (n_rows, dim), dtype=torch.int8)
    t0 = time.perf_counter()
    diff = (x[:-1] ^ x[1:]).view(torch.uint8)
    entropy = int(diff.sum().item())
    dt = time.perf_counter() - t0
    return {"entropy": entropy, "time_ms": dt * 1000, "rows": n_rows}


# ═══════════════════════════════════════════════════════════════════
#  PIPELINE BENCHMARK
# ═══════════════════════════════════════════════════════════════════

def bench_pipeline_single(
    gate, x_int8: torch.Tensor, mode: int, shell_level: int,
    max_tokens: int = 576, bridge=None
) -> dict:
    """Run pipeline once and return metrics."""
    from pipeline_merged import run_pipeline, l1_norm, load_gguf_int8

    N = min(x_int8.shape[0], max_tokens)
    x = x_int8[:N]
    out_shape = (N // 16, 16) if N >= 16 else (N, 1)

    torch.cuda.synchronize() if torch.cuda.is_available() else None
    t0 = time.perf_counter()
    out, snap = run_pipeline(gate, x, out_shape, mode=mode,
                              shell_level=shell_level, bridge=bridge)
    torch.cuda.synchronize() if torch.cuda.is_available() else None
    dt = time.perf_counter() - t0

    # Throughput: tokens processed per second
    throughput = N / dt if dt > 0 else 0

    return {
        "traverse": snap.traverse,
        "shell_level": snap.shell_level,
        "scale": snap.scale,
        "shape_name": snap.shape_name,
        "diff_l1": snap.diff_l1,
        "diff_max": snap.diff_max,
        "code_diff_l1": snap.code_diff_l1,
        "bond_valid": snap.bond_valid_count,
        "bond_total": snap.bond_total,
        "geo_seek_frames": snap.geo_seek.get('n_frames', 0),
        "geo_seek_delta_ratio": snap.geo_seek.get('delta_ratio', 0),
        "time_ms": dt * 1000,
        "throughput_tok_s": throughput,
        "tokens": N,
    }


def train_gate_on_model(x_int8: torch.Tensor, n_codes: int = 60,
                         epochs: int = 500, dim: int = 128) -> tuple:
    """Train BermudaGate on model weights. Returns (gate, train_time, final_loss)."""
    from bermuda_reshape_v2 import BermudaGate, N_CODES

    DEVICE = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    gate = BermudaGate(dim, code_dim=64, n_codes=n_codes).to(DEVICE)
    opt = torch.optim.AdamW(
        list(gate.encoder.parameters()) + list(gate.decoder.parameters()),
        lr=3e-4, weight_decay=1e-4
    )
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, epochs)

    # Prepare training data
    N = x_int8.shape[0]
    D = x_int8.shape[1]
    x_f = x_int8.to(torch.float32).to(DEVICE)
    off = x_f.mean(dim=-1, keepdim=True)
    core = x_f - off
    sw = core - core.mean(dim=0, keepdim=True)
    sw_scale = sw.abs().max().clamp(min=1.0)
    sw = sw / sw_scale

    t0 = time.perf_counter()
    gate.train()
    for ep in range(epochs):
        idx = torch.randint(0, N, (min(N, 192),), device=DEVICE)
        xb = sw[idx]
        co, _, _, vq = gate(xb, mode=0)
        loss = (co - xb).pow(2).mean() + 0.1 * vq
        opt.zero_grad()
        loss.backward()
        opt.step()
        sched.step()
    gate.eval()
    dt = time.perf_counter() - t0

    return gate, dt, float(loss.item())


# ═══════════════════════════════════════════════════════════════════
#  MEMORY MEASUREMENT
# ═══════════════════════════════════════════════════════════════════

def get_ram_mb() -> float:
    try:
        import psutil
        return psutil.Process().memory_info().rss / (1024 * 1024)
    except ImportError:
        return 0.0


def get_vram_mb() -> float:
    if torch.cuda.is_available():
        return torch.cuda.max_memory_allocated() / (1024 * 1024)
    return 0.0


def reset_cuda_peak():
    if torch.cuda.is_available():
        torch.cuda.synchronize()
        torch.cuda.reset_peak_memory_stats()


# ═══════════════════════════════════════════════════════════════════
#  SINGLE MODEL BENCH
# ═══════════════════════════════════════════════════════════════════

def bench_one_model(
    spec: ModelSpec,
    cache_dir: Path,
    max_tokens: int = 576,
    shell_levels: List[int] = [0, 1, 2, 3],
    gate_epochs: int = 500,
    bond_n: int = 100000,
) -> BenchResult:
    """Full benchmark for a single model."""
    DEVICE = torch.device("cuda" if torch.cuda.is_available() else "CPU")
    is_safetensors = getattr(spec, "format", "gguf") == "safetensors"
    print(f"\n{'='*60}")
    fmt_tag = "safetensors" if is_safetensors else "GGUF"
    print(f"BENCH: {spec.name} ({spec.params}, {spec.quant}) [{fmt_tag}]")
    print(f"{'='*60}")

    # 1. Download
    model_path = download_model(spec, cache_dir)

    # 2. Inspect
    print(f"\n  Inspecting {fmt_tag}...")
    if is_safetensors:
        info = inspect_safetensors(str(model_path))
        print(f"  2D tensors: {info['tensor_count']}")
        print(f"  Total bytes: {info['total_bytes'] / 1e6:.1f} MB")
    else:
        info = inspect_gguf(str(model_path))
        print(f"  Q8_0 tensors: {info['tensor_count']}")
        print(f"  Total int8 bytes: {info['total_int8_bytes'] / 1e6:.1f} MB")

    result = BenchResult(
        model_id=spec.id,
        model_name=spec.name,
        params=spec.params,
        quant=spec.quant,
        file_size_mb=gguf_path.stat().st_size / 1e6,
        tensor_count=info["tensor_count"],
        total_int8_bytes=info["total_int8_bytes"],
        timestamp=time.strftime("%Y-%m-%d %H:%M:%S"),
    )

    # 3. Bond bench
    print(f"\n  Bond bench ({bond_n:,} iterations)...")
    fibo_ops = bench_fibo_addr(bond_n)
    result.fibo_addr_ops = fibo_ops
    print(f"  fibo_addr: {fibo_ops:,.0f} ops/s")

    # 4. Load tensor
    print(f"\n  Loading {fmt_tag} tensor (max_tokens={max_tokens})...")
    reset_cuda_peak()
    ram_before = get_ram_mb()
    if is_safetensors:
        x_int8 = load_safetensors_int8(str(model_path), max_tokens=max_tokens).to(DEVICE)
    else:
        from pipeline_merged import load_gguf_int8
        x_int8 = load_gguf_int8(str(model_path), max_tokens=max_tokens).to(DEVICE)
    print(f"  Tensor: {x_int8.shape}  dtype={x_int8.dtype}")

    # 5. Train gate
    print(f"\n  Training gate ({gate_epochs} epochs)...")
    gate, train_time, train_loss = train_gate_on_model(
        x_int8, epochs=gate_epochs
    )
    result.gate_train_loss = train_loss
    result.gate_train_time_s = train_time
    print(f"  Gate: loss={train_loss:.4f}  time={train_time:.1f}s")

    # 6. Pipeline bench: all traverse modes × shell levels
    print(f"\n  Pipeline bench...")
    bridge = None  # C bond bridge optional
    for mode_idx, mode_name in enumerate(TRAVERSE_NAMES):
        result.pipeline_results[mode_name] = {}
        for sl in shell_levels:
            r = bench_pipeline_single(gate, x_int8, mode_idx, sl,
                                       max_tokens=max_tokens, bridge=bridge)
            result.pipeline_results[mode_name][sl] = r

        # Print summary for this mode
        best_sl = max(result.pipeline_results[mode_name].keys(),
                       key=lambda s: result.pipeline_results[mode_name][s]["throughput_tok_s"])
        best = result.pipeline_results[mode_name][best_sl]
        print(f"  {mode_name:8s}: best_shell={best['scale']:>4}  "
              f"diff_l1={best['diff_l1']:>8}  "
              f"code_diff={best['code_diff_l1']:>8}  "
              f"geo_frames={best.get('geo_seek_frames',0)}  "
              f"delta_ratio={best.get('geo_seek_delta_ratio',0):.3f}  "
              f"throughput={best['throughput_tok_s']:>8.0f} tok/s  "
              f"time={best['time_ms']:>6.1f}ms")

    # 7. Code ramp check
    for mode_name in ["ORBITAL", "CHIRAL"]:
        if mode_name in result.pipeline_results and len(result.pipeline_results[mode_name]) >= 2:
            diffs = [result.pipeline_results[mode_name][sl]["code_diff_l1"]
                     for sl in sorted(result.pipeline_results[mode_name].keys())]
            if diffs[0] > 0:
                ramp = diffs[-1] / max(diffs[1], 1)
                status = "✓" if ramp > 2.0 else "✗"
                print(f"  {mode_name} code_ramp: {ramp:.2f}x {status}")

    # 8. Memory
    result.ram_peak_mb = get_ram_mb() - ram_before
    result.vram_peak_mb = get_vram_mb()
    print(f"\n  Memory: RAM +={result.ram_peak_mb:.1f} MB  VRAM peak={result.vram_peak_mb:.1f} MB")

    result.total_time_s = train_time + sum(
        r["time_ms"] / 1000
        for modes in result.pipeline_results.values()
        for r in modes.values()
    )
    print(f"  Total bench time: {result.total_time_s:.1f}s")

    del x_int8, gate
    if torch.cuda.is_available():
        torch.cuda.empty_cache()

    return result


# ═══════════════════════════════════════════════════════════════════
#  MULTI-MODEL BENCH
# ═══════════════════════════════════════════════════════════════════

def bench_all_models(
    model_ids: Optional[List[str]] = None,
    cache_dir: Path = DEFAULT_CACHE_DIR,
    max_tokens: int = 576,
    shell_levels: List[int] = [0, 1, 2, 3],
    gate_epochs: int = 500,
    bond_n: int = 100000,
) -> List[BenchResult]:
    """Benchmark multiple models and return results."""
    registry = load_registry()
    config = get_bench_config()

    # Override defaults with registry config
    max_tokens = max_tokens or config.get("max_tokens", 576)
    shell_levels = shell_levels or config.get("shell_levels", [0, 1, 2, 3])
    gate_epochs = gate_epochs or config.get("gate_epochs", 500)
    bond_n = bond_n or config.get("bond_iterations", 100000)

    # Filter models
    if model_ids:
        specs = [m for m in registry if m.id in model_ids]
        missing = set(model_ids) - {m.id for m in specs}
        if missing:
            print(f"⚠ unknown models: {missing}")
            print(f"  available: {[m.id for m in registry]}")
    else:
        specs = registry

    results = []
    for spec in specs:
        try:
            r = bench_one_model(spec, cache_dir, max_tokens, shell_levels,
                                gate_epochs, bond_n)
            results.append(r)
        except Exception as e:
            print(f"❌ {spec.name} FAILED: {e}")
            import traceback
            traceback.print_exc()

    return results


# ═══════════════════════════════════════════════════════════════════
#  RESULTS OUTPUT
# ═══════════════════════════════════════════════════════════════════

def print_comparison_table(results: List[BenchResult]):
    """Print side-by-side comparison of all models."""
    if not results:
        print("No results to compare.")
        return

    print(f"\n{'='*100}")
    print(f"MODEL COMPARISON")
    print(f"{'='*100}")
    header = f"{'Model':<20} {'Params':>6} {'Size':>7} {'Tensors':>8} {'fibo/s':>10} {'ORBITAL':>10} {'CHIRAL':>10} {'Ramp':>6} {'VRAM':>7}"
    print(header)
    print("-" * 100)

    for r in results:
        # Best throughput across shell levels for ORBITAL and CHIRAL
        orb_best = max(
            (v["throughput_tok_s"] for v in r.pipeline_results.get("ORBITAL", {}).values()),
            default=0
        )
        chi_best = max(
            (v["throughput_tok_s"] for v in r.pipeline_results.get("CHIRAL", {}).values()),
            default=0
        )
        # Code ramp
        ramp = 0.0
        if "ORBITAL" in r.pipeline_results and len(r.pipeline_results["ORBITAL"]) >= 2:
            diffs = [v["code_diff_l1"] for k, v in sorted(r.pipeline_results["ORBITAL"].items())]
            if diffs[0] > 0:
                ramp = diffs[-1] / max(diffs[1], 1)

        ramp_tag = f"{ramp:.1f}x" if ramp > 0 else "N/A"
        print(f"{r.model_name:<20} {r.params:>6} {r.file_size_mb:>5.0f}MB {r.tensor_count:>8} "
              f"{r.fibo_addr_ops:>10,.0f} {orb_best:>10,.0f} {chi_best:>10,.0f} "
              f"{ramp_tag:>6} {r.vram_peak_mb:>5.0f}MB")

    print(f"{'='*100}")


def save_results_json(results: List[BenchResult], path: Path):
    """Save results to JSON for later analysis."""
    data = []
    for r in results:
        d = asdict(r)
        # Convert int keys in pipeline_results to str for JSON
        if "pipeline_results" in d:
            fixed = {}
            for mode, levels in d["pipeline_results"].items():
                fixed[mode] = {str(k): v for k, v in levels.items()}
            d["pipeline_results"] = fixed
        data.append(d)
    with open(path, "w") as f:
        json.dump(data, f, indent=2)
    print(f"\n✓ Results saved to {path}")


def plot_comparison(results: List[BenchResult], output_dir: Path):
    """Generate comparison plots."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not available, skipping plots.")
        return

    if not results:
        return

    output_dir.mkdir(parents=True, exist_ok=True)

    # 1. Throughput comparison
    fig, axes = plt.subplots(1, 2, figsize=(14, 5))

    model_names = [r.model_name for r in results]
    x = np.arange(len(model_names))
    width = 0.35

    for ax_idx, mode in enumerate(["ORBITAL", "CHIRAL"]):
        ax = axes[ax_idx]
        throughputs = []
        for r in results:
            modes = r.pipeline_results.get(mode, {})
            best = max((v["throughput_tok_s"] for v in modes.values()), default=0)
            throughputs.append(best)
        bars = ax.bar(x, throughputs, width, color=["#4ecdc4", "#ff6b6b"][ax_idx])
        ax.set_ylabel("Throughput (tokens/s)")
        ax.set_title(f"{mode} Best Throughput")
        ax.set_xticks(x)
        ax.set_xticklabels(model_names, rotation=30, ha="right", fontsize=8)
        for bar, val in zip(bars, throughputs):
            ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 50,
                    f"{val:,.0f}", ha="center", fontsize=7)

    plt.tight_layout()
    plt.savefig(output_dir / "throughput_comparison.png", dpi=150)
    plt.close()
    print(f"✓ Saved throughput_comparison.png")

    # 2. Code ramp bar chart
    fig, ax = plt.subplots(figsize=(10, 5))
    ramp_vals = []
    for r in results:
        if "ORBITAL" in r.pipeline_results and len(r.pipeline_results["ORBITAL"]) >= 2:
            diffs = [v["code_diff_l1"] for k, v in sorted(r.pipeline_results["ORBITAL"].items())]
            ramp = diffs[-1] / max(diffs[1], 1) if diffs[0] > 0 else 0
        else:
            ramp = 0
        ramp_vals.append(ramp)

    colors = ["#4ecdc4" if v > 2.0 else "#ff6b6b" for v in ramp_vals]
    bars = ax.bar(x, ramp_vals, color=colors)
    ax.axhline(y=2.0, color="gray", linestyle="--", alpha=0.5, label="2x target")
    ax.set_ylabel("Code Ramp Ratio")
    ax.set_title("ORBITAL Code Ramp by Model")
    ax.set_xticks(x)
    ax.set_xticklabels(model_names, rotation=30, ha="right")
    ax.legend()
    for bar, val in zip(bars, ramp_vals):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.05,
                f"{val:.1f}x", ha="center", fontsize=8)

    plt.tight_layout()
    plt.savefig(output_dir / "code_ramp_comparison.png", dpi=150)
    plt.close()
    print(f"✓ Saved code_ramp_comparison.png")

    # 3. Memory comparison
    fig, ax = plt.subplots(figsize=(10, 5))
    vram_vals = [r.vram_peak_mb for r in results]
    ram_vals = [r.ram_peak_mb for r in results]
    ax.bar(x - width/2, vram_vals, width, label="VRAM", color="#7b68ee")
    ax.bar(x + width/2, ram_vals, width, label="RAM Δ", color="#ffb347")
    ax.set_ylabel("Memory (MB)")
    ax.set_title("Memory Usage by Model")
    ax.set_xticks(x)
    ax.set_xticklabels(model_names, rotation=30, ha="right")
    ax.legend()
    plt.tight_layout()
    plt.savefig(output_dir / "memory_comparison.png", dpi=150)
    plt.close()
    print(f"✓ Saved memory_comparison.png")


# ═══════════════════════════════════════════════════════════════════
#  W&B INTEGRATION
# ═══════════════════════════════════════════════════════════════════

def log_to_wandb(results: List[BenchResult], project: str = "fgls-bench"):
    """Log results to Weights & Biases."""
    try:
        import wandb
    except ImportError:
        print("wandb not installed, skipping W&B logging.")
        return

    wandb.init(project=project, name=f"bench-{time.strftime('%Y%m%d-%H%M')}")

    for r in results:
        # Log summary metrics
        orb_best = max(
            (v["throughput_tok_s"] for v in r.pipeline_results.get("ORBITAL", {}).values()),
            default=0
        )
        chi_best = max(
            (v["throughput_tok_s"] for v in r.pipeline_results.get("CHIRAL", {}).values()),
            default=0
        )

        wandb.log({
            f"{r.model_id}/file_size_mb": r.file_size_mb,
            f"{r.model_id}/tensor_count": r.tensor_count,
            f"{r.model_id}/fibo_addr_ops": r.fibo_addr_ops,
            f"{r.model_id}/orbital_throughput": orb_best,
            f"{r.model_id}/chiral_throughput": chi_best,
            f"{r.model_id}/vram_peak_mb": r.vram_peak_mb,
            f"{r.model_id}/gate_train_loss": r.gate_train_loss,
            f"{r.model_id}/gate_train_time_s": r.gate_train_time_s,
        })

        # Log per-shell-level details
        for mode_name, levels in r.pipeline_results.items():
            for sl, metrics in levels.items():
                wandb.log({
                    f"{r.model_id}/{mode_name}/shell_{sl}/diff_l1": metrics["diff_l1"],
                    f"{r.model_id}/{mode_name}/shell_{sl}/code_diff_l1": metrics["code_diff_l1"],
                    f"{r.model_id}/{mode_name}/shell_{sl}/throughput": metrics["throughput_tok_s"],
                    f"{r.model_id}/{mode_name}/shell_{sl}/time_ms": metrics["time_ms"],
                })

    wandb.finish()
    print("✓ Logged to W&B")


# ═══════════════════════════════════════════════════════════════════
#  MAIN
# ═══════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description="FGLS Multi-Model Benchmark")
    parser.add_argument("--models", nargs="*", default=None,
                        help="Model IDs to benchmark (default: all)")
    parser.add_argument("--cache-dir", type=Path, default=DEFAULT_CACHE_DIR,
                        help="Directory to cache downloaded GGUFs")
    parser.add_argument("--max-tokens", type=int, default=576)
    parser.add_argument("--shell-levels", nargs="+", type=int, default=[0, 1, 2, 3])
    parser.add_argument("--gate-epochs", type=int, default=500)
    parser.add_argument("--bond-n", type=int, default=100000)
    parser.add_argument("--output-dir", type=Path, default=SCRIPT_DIR / "results")
    parser.add_argument("--wandb", action="store_true", help="Log to Weights & Biases")
    parser.add_argument("--wandb-project", default="fgls-bench")
    parser.add_argument("--no-plot", action="store_true", help="Skip plot generation")
    args = parser.parse_args()

    print("=" * 60)
    print("FGLS Multi-Model Benchmark")
    print(f"Device: {'CUDA ' + torch.cuda.get_device_name(0) if torch.cuda.is_available() else 'CPU'}")
    print(f"PyTorch: {torch.__version__}")
    print(f"CUDA: {torch.version.cuda if torch.cuda.is_available() else 'N/A'}")
    print("=" * 60)

    results = bench_all_models(
        model_ids=args.models,
        cache_dir=args.cache_dir,
        max_tokens=args.max_tokens,
        shell_levels=args.shell_levels,
        gate_epochs=args.gate_epochs,
        bond_n=args.bond_n,
    )

    if results:
        print_comparison_table(results)
        args.output_dir.mkdir(parents=True, exist_ok=True)
        save_results_json(results, args.output_dir / "bench_results.json")
        if not args.no_plot:
            plot_comparison(results, args.output_dir)
        if args.wandb:
            log_to_wandb(results, args.wandb_project)

    print(f"\n{'='*60}")
    print(f"BENCH COMPLETE: {len(results)} models")
    print(f"{'='*60}")


if __name__ == "__main__":
    main()
