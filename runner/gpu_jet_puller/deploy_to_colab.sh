#!/bin/bash
# deploy_to_colab.sh — Package + deploy GPU Jet Puller to Colab (GPU & CPU sessions)
#   - Packs the binary + self-contained run.py into a tar.gz
#   - run.py detects GPU vs CPU session, streams stdout+stderr in real-time,
#     and captures all output to gpu_jet_puller.log
set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PACKDIR="/tmp/gpu_jet_packer"
PACKTAR="$ROOT/runner/gpu_jet_puller/gpu_jet_puller_colab.tar.gz"

BINARY="gpu_jet_puller_final"
BINARY_FALLBACK="gpu_jet_puller_colab"

echo "=== Packaging GPU Jet Puller for Colab ==="
rm -rf "$PACKDIR"
mkdir -p "$PACKDIR"

# Pick the best available binary
if [ -f "$ROOT/runner/gpu_jet_puller/$BINARY" ]; then
    cp "$ROOT/runner/gpu_jet_puller/$BINARY" "$PACKDIR/gpu_jet_puller"
    echo "  Binary: $BINARY"
elif [ -f "$ROOT/runner/gpu_jet_puller/$BINARY_FALLBACK" ]; then
    cp "$ROOT/runner/gpu_jet_puller/$BINARY_FALLBACK" "$PACKDIR/gpu_jet_puller"
    echo "  Binary: $BINARY_FALLBACK (fallback)"
else
    echo "FATAL: Neither $BINARY nor $BINARY_FALLBACK found in runner/gpu_jet_puller/"
    exit 1
fi

# Self-contained run.py — GPU/CPU detection, real-time streaming, full log capture
cat > "$PACKDIR/run.py" << 'PYEOF'
#!/usr/bin/env python3
"""
GPU Jet Puller runner for Colab.

Handles both GPU and CPU sessions:
  - GPU: detects nvidia-smi, runs the binary, streams stdout+stderr in real-time
  - CPU: prints a clear message asking user to switch to GPU runtime
  - All output is captured to gpu_jet_puller.log for post-hoc inspection
"""
import subprocess
import sys
import os
from datetime import datetime

LOG_FILE = "gpu_jet_puller.log"
BINARY   = "./gpu_jet_puller"


def log(msg: str):
    """Print a timestamped line to both stdout and the log file."""
    line = f"[{datetime.now().strftime('%H:%M:%S')}] {msg}"
    print(line, flush=True)
    return line


def check_gpu():
    """Return GPU info string, or None if no CUDA GPU is reachable."""
    try:
        r = subprocess.run(
            ["nvidia-smi", "--query-gpu=name,memory.total",
             "--format=csv,noheader"],
            capture_output=True, text=True, timeout=15
        )
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    except FileNotFoundError:
        pass          # nvidia-smi not on PATH (CPU session)
    except subprocess.TimeoutExpired:
        pass          # hung driver
    except Exception:
        pass
    return None


def run_binary() -> int:
    """Launch the GPU binary, streaming output line-by-line, and logging all."""
    if not os.path.isfile(BINARY):
        log(f"FATAL: {BINARY} not found — extract the tarball first")
        return 1

    os.chmod(BINARY, 0o755)
    log(f"Launching: {BINARY}")

    with open(LOG_FILE, "a", buffering=1) as lf:
        lf.write(f"\n{'=' * 60}\n")
        lf.write(f"Run started: {datetime.now().isoformat()}\n")
        lf.write(f"{'=' * 60}\n")

        # Merge stderr into stdout so nothing is missed
        proc = subprocess.Popen(
            [f"./{BINARY}"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=1,
            text=True,
        )

        # Stream every line to terminal AND log file simultaneously
        for line in proc.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
            lf.write(line)

        proc.wait()

    exit_code = proc.returncode
    with open(LOG_FILE, "a") as lf:
        lf.write(f"Exit code: {exit_code}\n")
    log(f"Binary finished with exit code {exit_code}")
    return exit_code


def main():
    log("=== GPU Jet Puller — Colab Run ===")

    gpu_info = check_gpu()
    if gpu_info:
        log(f"GPU detected: {gpu_info}")
    else:
        log("WARNING: No CUDA GPU detected — this is a CPU-only Colab session.")
        log("The GPU Jet Puller requires a CUDA-capable GPU (T4, V100, A100, etc.).")
        log("To switch: Runtime → Change runtime type → T4 GPU (or A100)")
        log("Exiting gracefully. No binary was launched.")
        sys.exit(0)   # graceful exit on CPU — no crash, just guidance

    exit_code = run_binary()
    log(f"Full output saved to: {LOG_FILE}")
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
PYEOF

# Package
cd "$PACKDIR"
tar czf "$PACKTAR" *
echo ""
echo "=== Packed: $PACKTAR ==="
ls -lh "$PACKTAR"
echo ""

# Print deploy instructions
echo "=== Deploy Commands ==="
echo ""
echo "  GPU session (T4/V100/A100):"
echo "    colab upload -s <session> gpu_jet_puller_colab.tar.gz /content/"
echo "    colab exec -s <session>: cd /content && tar xzf gpu_jet_puller_colab.tar.gz && python3 run.py"
echo ""
echo "  CPU session (auto-detects no GPU, exits gracefully):"
echo "    colab upload -s <session> gpu_jet_puller_colab.tar.gz /content/"
echo "    colab exec -s <session>: cd /content && tar xzf gpu_jet_puller_colab.tar.gz && python3 run.py"
echo ""
echo "  View full log after run:"
echo "    colab exec -s <session>: cat /content/gpu_jet_puller.log"
echo ""
echo "=== Package contents ==="
tar tzf "$PACKTAR"
