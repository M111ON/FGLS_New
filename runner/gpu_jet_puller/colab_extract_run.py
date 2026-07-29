#!/usr/bin/env python3
"""GPU Jet Puller benchmark runner — self-extracting from pre-uploaded tarball."""
import subprocess
import sys
import os
from datetime import datetime

LOG_FILE = "bench_results.log"
TARBALL = "/content/bench_payload.tar.gz"
BINARY = "/content/gpu_jet_puller_bench_sm75"


def log(msg: str):
    line = f"[{datetime.now().strftime('%H:%M:%S')}] {msg}"
    print(line, flush=True)


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


def main():
    log("=== GPU Jet Puller — Colab Benchmark Run ===")

    gpu_info = check_gpu()
    if gpu_info:
        log(f"GPU detected: {gpu_info}")
    else:
        log("WARNING: No CUDA GPU detected")
        sys.exit(0)

    # Extract tarball if needed
    if not os.path.isfile(BINARY):
        if os.path.isfile(TARBALL):
            log(f"Extracting: {TARBALL}")
            r = subprocess.run(["tar", "xzf", TARBALL, "-C", "/content"],
                               capture_output=True, text=True)
            if r.returncode != 0:
                log(f"Extract failed: {r.stderr}")
                sys.exit(1)
            log("Extracted successfully")
        else:
            log(f"FATAL: Neither {BINARY} nor {TARBALL} found")
            sys.exit(1)

    os.chmod(BINARY, 0o755)
    log(f"Launching: {BINARY}")

    with open(f"/content/{LOG_FILE}", "a", buffering=1) as lf:
        lf.write(f"\n{'='*60}\n")
        lf.write(f"Run started: {datetime.now().isoformat()}\n")
        lf.write(f"{'='*60}\n")

        proc = subprocess.Popen(
            [BINARY],
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
    with open(f"/content/{LOG_FILE}", "a") as lf:
        lf.write(f"Exit code: {exit_code}\n")
    log(f"Binary finished with exit code {exit_code}")

    # Also copy log to /content/ for easy download
    log(f"Full log at /content/{LOG_FILE}")
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
