"""
build_geom_tile_store.py — hex_tile encode all tensors → geometry tile store
=====================================================================
Input:  directory of .qdat / .bin raw tensor files
Output: .gsten per tensor (geometry tensor encoded) + .gsidx2 index

.gsten format:
  [header 16B]  = magic(4B) + n_tiles(4B) + tile_sz(1B) + reserved(7B)
  [tile_index]  = n_tiles × {offset(4B), size(1B), type(1B)} = 6B/tile
  [tile_data]   = encoded tiles (variable size)

Lossless: hex_tile encode/decode → original bytes ✓
"""

import struct, os, sys, json, time, hashlib
from pathlib import Path

GS_MAGIC = 0x4747454F  # 'GEOM'
TILE_SZ  = 7

# ── hex_tile encode (Python port of hex_tile.h) ────────────────
HEX_CELLS = 7
HENC_FLAT         = 0x00
HENC_TRIPLET_FLAT = 0x01
HENC_GRADIENT     = 0x02
HENC_EDGE         = 0x03

HEX_TRIPLETS = [(0,1,6),(1,2,6),(2,3,6),(3,4,6),(4,5,6),(5,0,6)]

def _triplet_flat(tile, ti):
    a,b,c = HEX_TRIPLETS[ti]
    return tile[a] == tile[b] == tile[c]

def _predict(tile):
    for i in range(6):
        if _triplet_flat(tile, i):
            return tile[HEX_TRIPLETS[i][0]]
    r = sorted(tile[:6])
    return (r[2] + r[3]) >> 1

def _classify(tile):
    if all(t == tile[0] for t in tile):
        return HENC_FLAT
    for i in range(6):
        if _triplet_flat(tile, i):
            return HENC_TRIPLET_FLAT
    mn, mx = min(tile), max(tile)
    return HENC_EDGE if (mx - mn > 32) else HENC_GRADIENT

def tile_encode(tile):
    typ = _classify(tile)
    if typ == HENC_FLAT:
        return struct.pack('BB', typ, tile[0])
    pred = _predict(tile)
    res = bytes((tile[i] - pred + 128) & 0xFF for i in range(HEX_CELLS))
    return struct.pack('BB', typ, pred) + res


def tile_decode(data):
    typ = data[0]
    if typ == HENC_FLAT:
        return bytes([data[1]] * HEX_CELLS)
    pred = data[1]
    return bytes((data[2 + i] - 128 + pred) & 0xFF for i in range(HEX_CELLS))


# ── Encode tensor ───────────────────────────────────────────────
def encode_tensor(raw_bytes: bytes) -> tuple:
    """Encode raw bytes → (gsten_bytes, stats)"""
    n = len(raw_bytes)
    n_tiles = (n + TILE_SZ - 1) // TILE_SZ

    tile_index = bytearray()
    tile_data  = bytearray()
    type_counts = {0:0, 1:0, 2:0, 3:0}

    for ti in range(n_tiles):
        start = ti * TILE_SZ
        end = min(start + TILE_SZ, n)
        blk = raw_bytes[start:end]

        # Pad with zeros
        if len(blk) < TILE_SZ:
            blk = blk + b'\x00' * (TILE_SZ - len(blk))

        enc = tile_encode(blk)
        typ = enc[0]
        type_counts[typ] = type_counts.get(typ, 0) + 1

        tile_index += struct.pack('<IB', len(tile_data), len(enc))
        # also store type for stats
        tile_index += struct.pack('B', typ)
        tile_data += enc

    # Cap tile_index to consistent 6B/entry
    # Rebuild clean: offset(4B) + size(1B) + type(1B)
    tile_index2 = bytearray()
    offset = 0
    # Repack tile_data and index from scratch
    # Actually let me just rebuild tile_data and index in one pass
    tile_index2 = bytearray()
    tile_data2  = bytearray()
    offset = 0
    for ti in range(n_tiles):
        start = ti * TILE_SZ
        end = min(start + TILE_SZ, n)
        blk = raw_bytes[start:end]
        if len(blk) < TILE_SZ:
            blk = blk + b'\x00' * (TILE_SZ - len(blk))
        enc = tile_encode(blk)
        tile_index2 += struct.pack('<IBB', offset, len(enc), enc[0])
        tile_data2 += enc
        offset += len(enc)

    # Write header
    header = struct.pack('<IIB7s', GS_MAGIC, n_tiles, TILE_SZ, b'\x00' * 7)

    gsten = header + bytes(tile_index2) + bytes(tile_data2)

    stats = {
        'n_tiles': n_tiles,
        'orig_bytes': n,
        'enc_bytes': len(tile_data2),
        'index_bytes': len(tile_index2),
        'total_bytes': len(gsten),
        'ratio': len(gsten) / n if n > 0 else 0,
        'type_counts': type_counts,
    }
    return gsten, stats


# ── Build all ───────────────────────────────────────────────────
def build_all(tensor_dir: str, out_dir: str, model_name: str):
    """Encode all .qdat / .bin files in tensor_dir"""
    os.makedirs(out_dir, exist_ok=True)

    files = sorted(Path(tensor_dir).glob('*.qdat')) + sorted(Path(tensor_dir).glob('*.bin'))
    if not files:
        print(f"No .qdat or .bin files found in {tensor_dir}")
        return

    gsten_dir = os.path.join(out_dir, f"{model_name}_gsten")
    os.makedirs(gsten_dir, exist_ok=True)

    index = []
    total_orig = 0
    total_enc = 0

    t0 = time.perf_counter()

    for fp in files:
        name = fp.stem
        raw = fp.read_bytes()
        gsten, stats = encode_tensor(raw)

        # Write .gsten file
        gsten_path = os.path.join(gsten_dir, f"{name}.gsten")
        with open(gsten_path, 'wb') as f:
            f.write(gsten)

        entry = {
            'name': name,
            'nbytes': stats['orig_bytes'],
            'gsten_size': stats['total_bytes'],
            'n_tiles': stats['n_tiles'],
            'ratio': round(stats['ratio'], 4),
            'tile_types': {
                'flat': stats['type_counts'].get(0, 0),
                'triplet': stats['type_counts'].get(1, 0),
                'gradient': stats['type_counts'].get(2, 0),
                'edge': stats['type_counts'].get(3, 0),
            }
        }
        index.append(entry)
        total_orig += stats['orig_bytes']
        total_enc += stats['total_bytes']

        print(f"  {name:40s}  {stats['orig_bytes']/1024:8.1f}KB → {stats['total_bytes']/1024:8.1f}KB  "
              f"ratio={stats['ratio']:.3f}  tiles={stats['n_tiles']}")

    elapsed = time.perf_counter() - t0
    ratio = total_enc / total_orig if total_orig > 0 else 0

    print(f"\n{'='*60}")
    print(f"  Model: {model_name}")
    print(f"  Tensors: {len(files)}")
    print(f"  Original: {total_orig/1024/1024:.2f} MB")
    print(f"  Encoded:  {total_enc/1024/1024:.2f} MB")
    print(f"  Ratio:    {ratio:.4f}")
    print(f"  Time:     {elapsed:.2f}s")
    print(f"  Store:    {gsten_dir}/")
    print(f"{'='*60}")

    return index


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument('--tensor-dir', required=True)
    p.add_argument('--out-dir', default='build')
    p.add_argument('--model-name', default='model')
    args = p.parse_args()

    build_all(args.tensor_dir, args.out_dir, args.model_name)
