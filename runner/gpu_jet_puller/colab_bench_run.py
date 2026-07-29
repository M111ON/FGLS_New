#!/usr/bin/env python3
"""GPU Jet Puller benchmark runner for Colab T4.

Usage:
  python3 colab_bench_run.py

Expects the binary 'gpu_jet_puller_bench_sm75' in the same directory.
"""
import os
import subprocess
import sys
from datetime import datetime

LOG_FILE = "bench_results.log"
BINARY = "./gpu_jet_puller_bench_sm75"


def log(msg: str):
    line = f"[{datetime.now().strftime('%H:%M:%S')}] {msg}"
    print(line, flush=True)
    return line


def check_gpu():
    try:
        r = subprocess.run(
            ["nvidia-smi", "--query-gpu=name,memory.total",
             "--format=csv,noheader"],
            capture_output=True, text=True, timeout=15
        )
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    except Exception:
        pass
    return None


def run_benchmark() -> int:
    if not os.path.isfile(BINARY):
        log(f"FATAL: {BINARY} not found")
        return 1

    os.chmod(BINARY, 0o755)
    log(f"Launching: {BINARY}")

    with open(LOG_FILE, "a", buffering=1) as lf:
        lf.write(f"\n{'='*60}\n")
        lf.write(f"Run started: {datetime.now().isoformat()}\n")
        lf.write(f"{'='*60}\n")

        proc = subprocess.Popen(
            [f"./{BINARY}"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=1,
            text=True,
        )

        for line in proc.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
            lf.write(line)

        proc.wait()

    exit_code = proc.returncode
    with open(LOG_FILE, "a") as lf:
        lf.write(f"Exit code: {exit_code}\n")
    log(f"Binary finished with exit code {exit_code}")

    # Print the log at the end
    print("\n\n=== FULL LOG ===\n")
    with open(LOG_FILE) as lf:
        print(lf.read())

    return exit_code


def main():
    log("=== GPU Jet Puller — Colab Benchmark Run ===")

    gpu_info = check_gpu()
    if gpu_info:
        log(f"GPU detected: {gpu_info}")
    else:
        log("WARNING: No CUDA GPU detected")
        log("This benchmark requires a CUDA-capable GPU (T4, V100, A100)")
        sys.exit(0)

    exit_code = run_benchmark()
    log(f"Benchmark complete. Results saved to: {LOG_FILE}")
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
