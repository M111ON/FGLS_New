"""
build_qwen25_store.py — Geometry Store Builder
===============================================
Read qwen25_tensors_full.json → extract raw tensor blobs from GGUF
→ save to build/qwen25_tensors_raw/ + build .gsidx index

Output structure:
  qwen25_tensors_raw/
    output.weight.bin
    token_embd.weight.bin
    blk.0.attn_q.weight.bin
    ...
  qwen25_geom.gsidx   — index: name → zone, face, geo_key, path
  qwen25_geom.gsdat   — flat binary: [geo_key(8B) | zone(1B) | face(1B) | nbytes(4B)]
"""

import json
import os, sys
import struct
import hashlib
import time
import argparse

p = argparse.ArgumentParser()
p.add_argument("--gguf", default="Qwen2.5-0.5B-Instruct.F16.gguf", help="GGUF model path")
p.add_argument("--tensor-json", default="tensors_full.json", help="tensor manifest JSON path")
p.add_argument("--out", default="build/", help="output directory")
p.add_argument("--model-name", default="qwen25-0.5b", help="model identifier for geo_key")
args = p.parse_args()

# ── CONFIG ─────────────────────────────────────────────────────
GGUF_PATH    = args.gguf
TENSOR_JSON  = args.tensor_json
OUT_DIR      = os.path.join(args.out, "qwen25_tensors_raw")
GSIDX_PATH   = os.path.join(args.out, "qwen25_geom.gsidx")
GSDAT_PATH   = os.path.join(args.out, "qwen25_geom.gsdat")
CHUNK_SIZE   = 4 * 1024 * 1024   # 4MB read buffer

# ── GEOMETRY MAPPING ────────────────────────────────────────────
POGLS_FNV_PRIME  = 0x00000100000001B3
POGLS_FNV_OFFSET = 0xCBF29CE484222325
POGLS_GEO_MAGIC  = 0x00120090024005A0
FIBO = [1,1,2,3,5,8,13,21,34,55,89,144,233,377,610,987]

FRAME_CYCLE  = 1440
FRAME_FACE_SZ = 120

def fibo_addr(seed: int) -> int:
    h = POGLS_FNV_OFFSET
    for i in range(8):
        h ^= (seed >> (i * 8)) & 0xFF
        h = (h * POGLS_FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    for i in range(16):
        h ^= (FIBO[i] * (seed >> (i & 7))) & 0xFFFFFFFFFFFFFFFF
        h = ((h << 13) | (h >> 51)) & 0xFFFFFFFFFFFFFFFF
    h ^= POGLS_GEO_MAGIC
    h = (h * POGLS_FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    h ^= h >> 33
    return h

def tensor_geo(name: str, shape: list, model_name: str) -> dict:
    h = hashlib.sha256(f"{model_name}:{name}:{tuple(shape)}".encode()).digest()
    seed = struct.unpack_from('<Q', h)[0]

    geo_key = fibo_addr(seed)
    enc     = geo_key % FRAME_CYCLE
    face    = enc // FRAME_FACE_SZ   # 0-11
    zone    = face                    # zone = face for now

    # classify by tensor role
    if 'attn_q' in name or 'attn_k' in name:
        role = 'attention'
    elif 'ffn' in name:
        role = 'ffn'
    elif 'norm' in name:
        role = 'norm'
    elif 'embd' in name or 'output' in name:
        role = 'embedding'
    else:
        role = 'other'

    return {
        'geo_key': geo_key,
        'enc'    : enc,
        'face'   : face,
        'zone'   : zone,
        'role'   : role,
    }

# ── EXTRACT ─────────────────────────────────────────────────────
def extract_tensors(tensors: list, gguf_path: str, out_dir: str) -> list:
    os.makedirs(out_dir, exist_ok=True)
    index = []
    total = len(tensors)

    print(f"Opening GGUF: {gguf_path}")
    with open(gguf_path, 'rb') as f:
        for i, t in enumerate(tensors):
            name   = t['name']
            offset = t['offset']
            nbytes = t['nbytes']
            shape  = t['shape']
            dtype  = t['dtype']

            # safe filename
            safe_name = name.replace('/', '_').replace('.', '_')
            out_path  = os.path.join(out_dir, f"{safe_name}.bin")

            # extract blob
            f.seek(offset)
            remaining = nbytes
            with open(out_path, 'wb') as out:
                while remaining > 0:
                    chunk = f.read(min(CHUNK_SIZE, remaining))
                    if not chunk:
                        break
                    out.write(chunk)
                    remaining -= len(chunk)

            # geometry mapping
            geo = tensor_geo(name, shape, args.model_name)

            entry = {
                'name'   : name,
                'shape'  : shape,
                'dtype'  : dtype,
                'nbytes' : nbytes,
                'path'   : out_path,
                **geo,
            }
            index.append(entry)

            if (i + 1) % 50 == 0 or (i + 1) == total:
                print(f"  [{i+1:3d}/{total}] {name} → face={geo['face']} zone={geo['zone']} ({nbytes//1024}KB)")

    return index

# ── WRITE INDEX ─────────────────────────────────────────────────
def write_gsidx(index: list, path: str):
    """Write JSON index for human-readable lookup"""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        json.dump(index, f, indent=2)
    print(f"gsidx → {path} ({len(index)} entries)")

def write_gsdat(index: list, path: str):
    """
    Write binary index: fixed 14B per entry
    [geo_key(8B) | zone(1B) | face(1B) | nbytes(4B)]
    """
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        for e in index:
            f.write(struct.pack('<QBBH',
                e['geo_key'] & 0xFFFFFFFFFFFFFFFF,
                e['zone'] & 0xFF,
                e['face'] & 0xFF,
                min(e['nbytes'] // 1024, 0xFFFF),  # KB, capped at 64MB
            ))
    size = os.path.getsize(path)
    print(f"gsdat → {path} ({size} bytes, {len(index)} records × 14B)")

# ── STATS ────────────────────────────────────────────────────────
def print_stats(index: list):
    print("\n" + "=" * 60)
    print("Geometry Distribution")

    face_count = {}
    role_count = {}
    total_bytes = 0

    for e in index:
        face_count[e['face']] = face_count.get(e['face'], 0) + 1
        role_count[e['role']] = role_count.get(e['role'], 0) + 1
        total_bytes += e['nbytes']

    print(f"\nTotal tensors : {len(index)}")
    print(f"Total size    : {total_bytes / 1024**3:.2f} GB")

    print("\nFace distribution:")
    for face in sorted(face_count):
        bar = '█' * face_count[face]
        print(f"  face {face:2d} [{bar}] {face_count[face]}")

    print("\nRole distribution:")
    for role, count in sorted(role_count.items(), key=lambda x: -x[1]):
        print(f"  {role:12s} {count}")

# ── MAIN ─────────────────────────────────────────────────────────
def main():
    print("=" * 60)
    print("Qwen2.5 Geometry Store Builder")
    print("=" * 60)

    # load tensor list
    print(f"\nLoading tensor list: {TENSOR_JSON}")
    with open(TENSOR_JSON) as f:
        tensors = json.load(f)
    print(f"  {len(tensors)} tensors loaded")

    # extract
    print(f"\nExtracting to: {OUT_DIR}")
    t0 = time.perf_counter()
    index = extract_tensors(tensors, GGUF_PATH, OUT_DIR)
    t1 = time.perf_counter()
    print(f"\nExtraction done: {t1-t0:.1f}s")

    # write index
    write_gsidx(index, GSIDX_PATH)
    write_gsdat(index, GSDAT_PATH)

    # stats
    print_stats(index)

    print("\n✓ Store ready")
    print(f"  gsidx : {GSIDX_PATH}")
    print(f"  gsdat : {GSDAT_PATH}")
    print(f"  blobs : {OUT_DIR}/")

if __name__ == "__main__":
    main()
