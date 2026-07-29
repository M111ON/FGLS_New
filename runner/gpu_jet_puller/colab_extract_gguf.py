#!/usr/bin/env python3
"""Extract tensors using gguf library, no manual parser needed."""
import os, sys, json, subprocess

# Install gguf if not available
try:
    from gguf import GGUFReader
except ImportError:
    print("Installing gguf package...", flush=True)
    subprocess.run([sys.executable, "-m", "pip", "install", "gguf", "-q"], 
                  capture_output=True, timeout=60)
    from gguf import GGUFReader

MODEL = "/content/Qwen3-0.6B-Q4_0.gguf"
RAW = "/content/tensor_data.bin"
META = "/content/tensor_meta.json"

print("Opening GGUF...", flush=True)
reader = GGUFReader(MODEL)
tensors = reader.tensors
print(f"Tensors: {len(tensors)}", flush=True)

total = 0
with open(RAW, "wb") as out:
    for i, t in enumerate(tensors):
        data = t.data.tobytes()
        out.write(data)
        total += len(data)
        if i < 5:
            print(f"  [{i}] {t.name}: shape={list(t.shape)}, {len(data)} B", flush=True)

print(f"EXTRACT_OK:{total}", flush=True)