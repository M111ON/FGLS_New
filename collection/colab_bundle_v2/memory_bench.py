"""
memory_bench.py
================
Measure RAM/VRAM usage for POGLS paths.

Usage examples:
  python3 memory_bench.py --gguf /content/Qwen3-0.6B-Q8_0.gguf --max-tokens 576 --mode baseline
  python3 memory_bench.py --gguf /content/Qwen3-0.6B-Q8_0.gguf --max-tokens 576 --mode pipeline --shell 3 --traverse 1
  python3 memory_bench.py --gguf /content/Qwen3-0.6B-Q8_0.gguf --max-tokens 576 --mode both
  python3 memory_bench.py --gguf /content/Qwen3-0.6B-Q8_0.gguf --mode all
"""

import argparse
import gc
import time

import psutil
import torch

from pipeline_merged import BermudaGate, DEVICE, N_CODES, load_gguf_int8, run_pipeline
from gguf import GGUFReader


def mib(x: int) -> float:
    return float(x) / (1024.0 * 1024.0)


def process_rss_bytes() -> int:
    return psutil.Process().memory_info().rss


def cuda_peak_bytes() -> int:
    if torch.cuda.is_available() and DEVICE.type == "cuda":
        return int(torch.cuda.max_memory_allocated(device=DEVICE))
    return 0


def reset_cuda_peak() -> None:
    if torch.cuda.is_available() and DEVICE.type == "cuda":
        torch.cuda.synchronize()
        torch.cuda.reset_peak_memory_stats(device=DEVICE)


def format_row(name: str, ram_base: int, ram_peak: int, vram_peak: int, dt: float) -> str:
    ram_delta = max(0, ram_peak - ram_base)
    return (
        f"{name:10s} | "
        f"RAM base={mib(ram_base):8.1f} MiB | "
        f"RAM peak={mib(ram_peak):8.1f} MiB | "
        f"RAM +={mib(ram_delta):8.1f} MiB | "
        f"VRAM peak={mib(vram_peak):8.1f} MiB | "
        f"time={dt:6.2f}s"
    )


def run_baseline(gguf: str, max_tokens: int) -> dict:
    gc.collect()
    if torch.cuda.is_available() and DEVICE.type == "cuda":
        torch.cuda.empty_cache()
    reset_cuda_peak()

    ram_base = process_rss_bytes()
    t0 = time.perf_counter()

    x = load_gguf_int8(gguf, max_tokens=max_tokens).to(DEVICE).to(torch.int8)
    # Baseline: keep tensor load + minimal arithmetic path.
    y = x.to(torch.int16)
    _ = int(y.abs().sum().item())

    if torch.cuda.is_available() and DEVICE.type == "cuda":
        torch.cuda.synchronize()

    dt = time.perf_counter() - t0
    ram_peak = process_rss_bytes()
    vram_peak = cuda_peak_bytes()

    del x, y
    gc.collect()
    return {
        "name": "baseline",
        "ram_base": ram_base,
        "ram_peak": ram_peak,
        "vram_peak": vram_peak,
        "time": dt,
    }


def run_pipeline_mode(gguf: str, max_tokens: int, traverse: int, shell: int, cycle: int) -> dict:
    gc.collect()
    if torch.cuda.is_available() and DEVICE.type == "cuda":
        torch.cuda.empty_cache()
    reset_cuda_peak()

    ram_base = process_rss_bytes()
    t0 = time.perf_counter()

    x = load_gguf_int8(gguf, max_tokens=max_tokens).to(DEVICE).to(torch.int8)
    gate = BermudaGate(128, code_dim=64, n_codes=N_CODES).to(DEVICE)
    gate.eval()

    n = x.shape[0]
    out, snap = run_pipeline(
        gate,
        x,
        out_shape=(n // 16, 16),
        mode=traverse,
        shell_level=shell,
        bridge=None,
        cycle=cycle,
    )
    _ = (out.numel(), snap.code_diff_l1, snap.diff_l1)

    if torch.cuda.is_available() and DEVICE.type == "cuda":
        torch.cuda.synchronize()

    dt = time.perf_counter() - t0
    ram_peak = process_rss_bytes()
    vram_peak = cuda_peak_bytes()

    del out, x, gate
    gc.collect()
    return {
        "name": "pipeline",
        "ram_base": ram_base,
        "ram_peak": ram_peak,
        "vram_peak": vram_peak,
        "time": dt,
    }


def run_full_model_proxy(gguf: str) -> dict:
    """
    Full-model proxy path:
    - Parse all GGUF tensors
    - Materialize all Q8_0 int8 blocks into RAM
    This approximates full-weight memory pressure without requiring a runtime backend.
    """
    gc.collect()
    if torch.cuda.is_available() and DEVICE.type == "cuda":
        torch.cuda.empty_cache()
    reset_cuda_peak()

    ram_base = process_rss_bytes()
    t0 = time.perf_counter()

    reader = GGUFReader(gguf)
    mats = []
    total_bytes = 0
    for t in reader.tensors:
        if not t.name.endswith(".weight") or len(t.shape) < 2:
            continue
        data = t.data
        if len(data.shape) != 2:
            continue
        m, b = data.shape
        if b % 34 != 0:
            continue
        nb = b // 34
        blocks = data.reshape(m, nb, 34)
        int8_raw = blocks[:, :, :32].reshape(m, nb * 32).astype("int8")
        mats.append(int8_raw)
        total_bytes += int8_raw.nbytes

    # Touch memory so OS commits pages.
    checksum = 0
    for a in mats:
        checksum ^= int(a[0, 0])

    if torch.cuda.is_available() and DEVICE.type == "cuda":
        torch.cuda.synchronize()

    dt = time.perf_counter() - t0
    ram_peak = process_rss_bytes()
    vram_peak = cuda_peak_bytes()

    del mats, reader, checksum
    gc.collect()
    return {
        "name": "full_proxy",
        "ram_base": ram_base,
        "ram_peak": ram_peak,
        "vram_peak": vram_peak,
        "time": dt,
        "bytes": total_bytes,
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--gguf", required=True, help="Path to GGUF file")
    ap.add_argument("--max-tokens", type=int, default=576)
    ap.add_argument("--mode", choices=["baseline", "pipeline", "both", "full", "all"], default="both")
    ap.add_argument("--traverse", type=int, default=1, help="0 ORBITAL, 1 CHIRAL, 2 CROSS, 3 HUB")
    ap.add_argument("--shell", type=int, default=3, help="Shell level (0..3)")
    ap.add_argument("--cycle", type=int, default=0)
    args = ap.parse_args()

    rows = []
    print("=" * 72)
    print("POGLS Memory Bench")
    print("=" * 72)
    print(f"device={DEVICE}  cuda={torch.cuda.is_available()}  gguf={args.gguf}")

    if args.mode in ("baseline", "both", "all"):
        rows.append(run_baseline(args.gguf, args.max_tokens))
    if args.mode in ("pipeline", "both", "all"):
        rows.append(run_pipeline_mode(args.gguf, args.max_tokens, args.traverse, args.shell, args.cycle))
    if args.mode in ("full", "all"):
        rows.append(run_full_model_proxy(args.gguf))

    print("\nResults")
    print("-" * 72)
    for r in rows:
        print(format_row(r["name"], r["ram_base"], r["ram_peak"], r["vram_peak"], r["time"]))
        if r.get("bytes") is not None:
            print(f"{'':10s}   materialized int8 weights={mib(r['bytes']):.1f} MiB")

    if len(rows) == 2 and rows[0]["name"] == "baseline" and rows[1]["name"] == "pipeline":
        b = rows[0]
        p = rows[1]
        b_ram = max(1, b["ram_peak"] - b["ram_base"])
        p_ram = max(1, p["ram_peak"] - p["ram_base"])
        b_vram = max(1, b["vram_peak"])
        p_vram = max(1, p["vram_peak"])
        print("-" * 72)
        print(
            "delta(pipeline vs baseline): "
            f"RAM x={p_ram / b_ram:.2f}, VRAM x={p_vram / b_vram:.2f}"
        )


if __name__ == "__main__":
    main()
