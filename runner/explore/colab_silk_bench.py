#!/usr/bin/env python3
"""
Colab Silk Screen Benchmark — download model + compile + bench
Usage in Colab:
  !python3 colab_silk_bench.py [model_url] [filename]
"""

import subprocess, sys, os, time, urllib.request

# ── Config ───────────────────────────────────────────────────
DEFAULT_MODEL = "https://huggingface.co/Qwen/Qwen3-0.6B-GGUF/resolve/main/qwen3-0.6b-q8_0.gguf"
DEFAULT_NAME  = "qwen3-0.6b-q8_0.gguf"

MODELS = {
    "0.6b":  ("https://huggingface.co/Qwen/Qwen3-0.6B-GGUF/resolve/main/qwen3-0.6b-q8_0.gguf",
              "qwen3-0.6b-q8_0.gguf"),
    "3b":    ("https://huggingface.co/Qwen/Qwen2.5-3B-GGUF/resolve/main/qwen2.5-3b-q8_0.gguf",
              "qwen2.5-3b-q8_0.gguf"),
    "7b":    ("https://huggingface.co/Qwen/Qwen2.5-7B-GGUF/resolve/main/qwen2.5-7b-q8_0.gguf",
              "qwen2.5-7b-q8_0.gguf"),
    "14b":   ("https://huggingface.co/Qwen/Qwen2.5-14B-GGUF/resolve/main/qwen2.5-14b-q8_0.gguf",
              "qwen2.5-14b-q8_0.gguf"),
    "30b":   ("https://huggingface.co/Qwen/Qwen2.5-32B-GGUF/resolve/main/qwen2.5-32b-q8_0.gguf",
              "qwen2.5-32b-q8_0.gguf"),
}

def download(url, name):
    if os.path.exists(name):
        print(f"  ✓ {name} already exists ({os.path.getsize(name)/1e6:.0f} MB)")
        return True
    print(f"  Downloading {name}...")
    print(f"  URL: {url}")
    try:
        urllib.request.urlretrieve(url, name)
        print(f"  ✓ Downloaded: {os.path.getsize(name)/1e6:.0f} MB")
        return True
    except Exception as e:
        print(f"  ✗ Failed: {e}")
        return False

def compile_encoder():
    print("\n=== Compiling silk_screen_encoder.c ===")
    r = subprocess.run(
        ["gcc", "-O2", "-std=c11", "-Wall", "-o", "silk_screen_encoder", "silk_screen_encoder.c", "-lm"],
        capture_output=True, text=True
    )
    if r.returncode != 0:
        print(f"  ✗ Compile failed:\n{r.stderr}")
        return False
    print("  ✓ Compiled successfully")
    return True

def run_bench(model_file):
    print(f"\n=== Running benchmark: {model_file} ===")
    r = subprocess.run(
        ["./silk_screen_encoder", model_file],
        capture_output=True, text=True, timeout=300
    )
    print(r.stdout)
    if r.returncode != 0:
        print(f"  ✗ Failed:\n{r.stderr}")
    return r.returncode == 0

def main():
    print("=" * 60)
    print("  Colab Silk Screen Benchmark")
    print("=" * 60)

    # Parse args
    if len(sys.argv) > 1 and sys.argv[1] in MODELS:
        url, name = MODELS[sys.argv[1]]
        print(f"\n  Model: {sys.argv[1].upper()} ({name})")
    elif len(sys.argv) > 1:
        url = sys.argv[1]
        name = sys.argv[2] if len(sys.argv) > 2 else "model.gguf"
        print(f"\n  Model: {name} (custom URL)")
    else:
        url, name = DEFAULT_MODEL, DEFAULT_NAME
        print(f"\n  Model: default ({name})")
        print(f"  Usage: python3 colab_silk_bench.py [0.6b|3b|7b|14b|30b|<url>]")

    # Step 1: Download
    print("\n=== Step 1: Download Model ===")
    if not download(url, name):
        return 1

    # Step 2: Compile encoder
    print("\n=== Step 2: Compile Encoder ===")
    if not compile_encoder():
        return 1

    # Step 3: Run benchmark
    print("\n=== Step 3: Benchmark ===")
    t0 = time.time()
    ok = run_bench(name)
    elapsed = time.time() - t0
    print(f"\n  Total wall time: {elapsed:.1f}s")

    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
