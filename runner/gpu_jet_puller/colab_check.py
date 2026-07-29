#!/usr/bin/env python3
import os
print("=== Files in /content ===")
for f in sorted(os.listdir("/content")):
    path = f"/content/{f}"
    sz = os.path.getsize(path) if os.path.isfile(path) else 0
    if os.path.isfile(path):
        print(f"  {f:40s} {sz:>12,} B ({sz/1e6:.1f} MB)")
    else:
        print(f"  {f:40s} (DIR)")

# Search recursively for .gguf files
print("\n=== Searching for GGUF files ===")
for root, dirs, files in os.walk("/content"):
    for f in files:
        if f.endswith(".gguf"):
            path = os.path.join(root, f)
            print(f"  {path} ({os.path.getsize(path)/1e6:.1f} MB)")
    
print("\n=== Done ===")