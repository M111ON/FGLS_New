#!/usr/bin/env python3
"""FGLS Colab Benchmark"""
import subprocess, sys, urllib.request, os, json, time

# ── Install deps ──
subprocess.run(["apt-get", "update", "-qq"], check=False)
subprocess.run(["apt-get", "install", "-y", "-qq", "wget"], check=False)

# ── Download binaries ──
BINARIES = {
    "geo_seek_bench": "https://github.com/googlecolab/google-colab-cli/raw/main/...",  # ต้องแก้เป็น URL จริง
}

for name, url in BINARIES.items():
    urllib.request.urlretrieve(url, f"/content/{name}")
    os.chmod(f"/content/{name}", 0o755)

# ── Run benchmark ──
result = subprocess.run(["/content/geo_seek_bench"], capture_output=True, text=True, timeout=60)
print(result.stdout)
print("=== DONE ===")
