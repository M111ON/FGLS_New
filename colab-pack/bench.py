#!/usr/bin/env python3
"""bench.py - รัน FGLS benchmark on Colab"""
import subprocess, sys, os, urllib.request

BIN_URL = "https://github.com/.../releases/download/latest/geo_seek_bench"

# Download and run
subprocess.run(["wget", "-q", BIN_URL, "-O", "/content/geo_seek_bench"], check=True)
subprocess.run(["chmod", "+x", "/content/geo_seek_bench"], check=True)
result = subprocess.run(["/content/geo_seek_bench"], capture_output=True, text=True)
print(result.stdout)
