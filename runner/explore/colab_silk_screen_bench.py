#!/usr/bin/env python3
"""
Silk Screen Encoder — Colab Benchmark
Downloads GGUF model, compiles silk_screen_encoder.c, runs bake test.

Usage in Colab:
  !python colab_silk_screen_bench.py [model_url] [model_name]

Default: Qwen3-0.6B-Q8_0 from HuggingFace
"""
import os, sys, subprocess, time, struct

# ============================================================
# Config
# ============================================================
DEFAULT_URL = "https://huggingface.co/Qwen/Qwen3-0.6B-GGUF/resolve/main/Qwen3-0.6B-Q8_0.gguf"
DEFAULT_NAME = "Qwen3-0.6B-Q8_0.gguf"
MODEL_DIR = "/content/models"
SRC_PATH = "/content/silk_screen_encoder.c"
BIN_PATH = "/content/silk_screen_encoder"

# Alternate models for scale testing
MODELS = {
    "qwen3-0.6b":  ("https://huggingface.co/Qwen/Qwen3-0.6B-GGUF/resolve/main/Qwen3-0.6B-Q8_0.gguf", "Qwen3-0.6B-Q8_0.gguf"),
    "qwen2.5-3b":  ("https://huggingface.co/Qwen/Qwen2.5-3B-GGUF/resolve/main/Qwen2.5-3B-Q8_0.gguf", "Qwen2.5-3B-Q8_0.gguf"),
    "qwen2.5-7b":  ("https://huggingface.co/Qwen/Qwen2.5-7B-GGUF/resolve/main/Qwen2.5-7B-Q8_0.gguf", "Qwen2.5-7B-Q8_0.gguf"),
    "qwen2.5-14b": ("https://huggingface.co/Qwen/Qwen2.5-14B-GGUF/resolve/main/Qwen2.5-14B-Q8_0.gguf", "Qwen2.5-14B-Q8_0.gguf"),
    "qwen2.5-30b": ("https://huggingface.co/Qwen/Qwen2.5-32B-GGUF/resolve/main/Qwen2.5-32B-Q8_0.gguf", "Qwen2.5-32B-Q8_0.gguf"),
}

def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)

# ============================================================
# Step 1: Download model
# ============================================================
def download_model(url, name):
    os.makedirs(MODEL_DIR, exist_ok=True)
    path = os.path.join(MODEL_DIR, name)

    if os.path.isfile(path) and os.path.getsize(path) > 1_000_000:
        log(f"Model exists: {name} ({os.path.getsize(path)/1e6:.1f} MB)")
        return path

    log(f"Downloading {name}...")
    t0 = time.time()
    r = subprocess.run(
        ["wget", "-O", path, url, "-q", "--no-check-certificate"],
        capture_output=True, text=True, timeout=1800  # 30 min for large models
    )
    dt = time.time() - t0

    if r.returncode != 0 or not os.path.isfile(path) or os.path.getsize(path) < 1_000_000:
        log(f"wget failed (exit={r.returncode}), trying alt URL...")
        # Try unsloth mirror
        alt_url = url.replace("/Qwen/", "/unsloth/")
        r = subprocess.run(
            ["wget", "-O", path, alt_url, "-q", "--no-check-certificate"],
            capture_output=True, text=True, timeout=1800
        )

    sz = os.path.getsize(path) / 1e6
    log(f"Downloaded: {name} ({sz:.1f} MB) in {dt:.1f}s ({sz/dt:.1f} MB/s)")
    return path

# ============================================================
# Step 2: Write C source
# ============================================================
def write_encoder_source():
    """Write the silk_screen_encoder.c source to Colab."""
    # This is the full encoder — should be pasted from the local build
    # For now, we inline the key parts (or read from a file)
    src_file = os.path.join(os.path.dirname(__file__), "silk_screen_encoder.c")
    if os.path.isfile(src_file):
        with open(src_file, "r") as f:
            src = f.read()
        with open(SRC_PATH, "w") as f:
            f.write(src)
        log(f"C source written from {src_file} ({len(src)} bytes)")
    else:
        log("ERROR: silk_screen_encoder.c not found alongside this script")
        log(f"Expected at: {src_file}")
        sys.exit(1)

# ============================================================
# Step 3: Compile
# ============================================================
def compile_encoder():
    log("Compiling silk_screen_encoder.c ...")
    t0 = time.time()
    r = subprocess.run(
        ["gcc", "-O2", "-std=c11", "-o", BIN_PATH, SRC_PATH, "-lm"],
        capture_output=True, text=True, timeout=120
    )
    dt = time.time() - t0

    if r.returncode != 0:
        log(f"COMPILE FAILED ({dt:.1f}s):")
        log(r.stderr[:1000])
        sys.exit(1)

    sz = os.path.getsize(BIN_PATH) / 1024
    log(f"Compiled: {sz:.0f} KB in {dt:.1f}s")

# ============================================================
# Step 4: Run benchmark
# ============================================================
def run_benchmark(model_path):
    log(f"Running silk screen bake on {os.path.basename(model_path)}...")
    t0 = time.time()
    r = subprocess.run(
        [BIN_PATH, model_path],
        capture_output=True, text=True, timeout=600
    )
    dt = time.time() - t0

    # Print output
    for line in r.stdout.split("\n"):
        if line.strip():
            print(f"  {line}", flush=True)

    if r.stderr.strip():
        # Filter out GGUF debug lines
        for line in r.stderr.split("\n"):
            if line.strip() and "[GGUF]" not in line:
                log(f"STDERR: {line[:200]}")

    log(f"Exit: {r.returncode} ({dt:.1f}s)")
    return r.returncode

# ============================================================
# Step 5: Multi-model scale test
# ============================================================
def run_scale_test():
    """Run bake on multiple models to measure scale."""
    print("\n" + "="*60)
    print("  SILK SCREEN SCALE TEST — Multiple Models")
    print("="*60 + "\n")

    results = []
    for name, (url, fname) in MODELS.items():
        try:
            path = download_model(url, fname)
            sz_mb = os.path.getsize(path) / 1e6
            log(f"Testing {name} ({sz_mb:.0f} MB)...")

            t0 = time.time()
            r = subprocess.run([BIN_PATH, path], capture_output=True, text=True, timeout=600)
            dt = time.time() - t0

            # Parse output for key metrics
            exact = lossless = bake_ms = 0
            for line in r.stdout.split("\n"):
                if "Exact matches:" in line and "100.0%" in line:
                    lossless = 1
                if "Encode time:" in line:
                    try: bake_ms = float(line.split(":")[1].strip().split()[0])
                    except: pass

            results.append({
                "name": name, "size_mb": sz_mb, "time_s": dt,
                "lossless": lossless, "bake_ms": bake_ms
            })
            log(f"  {name}: {dt:.1f}s, lossless={'YES' if lossless else 'NO'}")

        except Exception as e:
            log(f"  {name}: FAILED — {e}")
            results.append({"name": name, "error": str(e)})

    # Summary
    print("\n" + "="*60)
    print("  SCALE TEST RESULTS")
    print("="*60)
    print(f"  {'Model':<20} {'Size (MB)':>10} {'Time (s)':>10} {'Lossless':>10}")
    print(f"  {'-----':<20} {'---------':>10} {'---------':>10} {'--------':>10}")
    for r in results:
        if "error" in r:
            print(f"  {r['name']:<20} {'ERROR':>10} {r['error'][:20]:>10}")
        else:
            print(f"  {r['name']:<20} {r['size_mb']:>10.0f} {r['time_s']:>10.1f} {'YES' if r.get('lossless') else 'NO':>10}")

# ============================================================
# Main
# ============================================================
if __name__ == "__main__":
    log("="*60)
    log("  Silk Screen Encoder — Colab Benchmark")
    log("="*60)

    # Check for scale test flag
    if len(sys.argv) > 1 and sys.argv[1] == "--scale":
        write_encoder_source()
        compile_encoder()
        run_scale_test()
        sys.exit(0)

    # Single model test
    url = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_URL
    name = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_NAME

    model_path = download_model(url, name)
    write_encoder_source()
    compile_encoder()
    exit_code = run_benchmark(model_path)

    log("DONE")
    sys.exit(exit_code)
