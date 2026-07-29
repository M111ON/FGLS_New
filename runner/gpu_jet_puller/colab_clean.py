import os
import subprocess
import sys

# Clean up old buggy files on Colab
files_to_remove = [
    "/content/gpu_jet_puller_gguf",  # old buggy binary
    "/content/gpu_jet_puller_gguf.cu",  # old buggy CUDA source
    "/content/bench_results.log",
]
for f in files_to_remove:
    if os.path.isfile(f):
        os.remove(f)
        print(f"Removed: {f}")

# Check what's left
for f in sorted(os.listdir("/content")):
    path = f"/content/{f}"
    if os.path.isfile(path):
        print(f"  {f}: {os.path.getsize(path)} B")
    elif os.path.isdir(path):
        print(f"  {f}/ (DIR)")

print("Cleanup done")