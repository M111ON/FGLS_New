#!/usr/bin/env python3
"""Colab benchmark runner — อัปโหลด binary แล้วรัน benchmark"""
import subprocess, sys, os

# 1. อัปโหลด binaries
BINARIES = "/mnt/i/FGLS_new/colab-pack"
for f in ["geo_seek_bench", "fgls_linux", "gguf_real_bench", "beam_hilbert_icosahedron"]:
    p = os.path.join(BINARIES, f)
    subprocess.run(["colab", "upload", p, f"/content/{f}"], check=True)
    subprocess.run(["colab", "exec", "-f", "-"], 
                   input=f"!chmod +x /content/{f}\n".encode(), check=True)

# 2. รัน benchmark
cmds = [
    "/content/geo_seek_bench",
    "/content/beam_hilbert_icosahedron",
]
for cmd in cmds:
    subprocess.run(["colab", "exec", "-f", "-"],
                   input=f"!/content/{os.path.basename(cmd)}\n".encode())
