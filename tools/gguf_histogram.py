#!/usr/bin/env python3
"""
GGUF Weight Histogram Extractor — v2 (robust parser)
Extracts actual weight values from a GGUF model file and computes histogram.
"""

import struct
import sys
import os
import math
from collections import Counter

GGML_TYPE_F32  = 0
GGML_TYPE_F16  = 1
GGML_TYPE_Q8_0 = 8

TYPE_NAMES = {0: 'F32', 1: 'F16', 8: 'Q8_0'}

def read_str(data, pos):
    """Read GGUF length-prefixed string, return (string, new_pos)."""
    length = struct.unpack_from('<Q', data, pos)[0]
    s = data[pos+8:pos+8+length].decode('utf-8', errors='replace')
    return s, pos + 8 + length

def read_tensor_infos(data, pos, count):
    """Parse tensor info records."""
    infos = []
    for _ in range(count):
        name, pos = read_str(data, pos)
        n_dims = struct.unpack_from('<I', data, pos)[0]; pos += 4
        dims = list(struct.unpack_from(f'<{n_dims}Q', data, pos)); pos += 8*n_dims
        typ = struct.unpack_from('<I', data, pos)[0]; pos += 4
        offset = struct.unpack_from('<Q', data, pos)[0]; pos += 8
        total = 1
        for d in dims:
            total *= d
        infos.append((name, dims, typ, offset, total))
    return infos

def skip_metadata(data, pos, count):
    """Skip count metadata KV pairs."""
    for i in range(count):
        key, pos = read_str(data, pos)
        vtype = struct.unpack_from('<I', data, pos)[0]; pos += 4

        if vtype == 0:   # uint8
            pos += 1
        elif vtype == 1: # int8
            pos += 1
        elif vtype == 2: # uint16
            pos += 2
        elif vtype == 3: # int16
            pos += 2
        elif vtype == 4: # uint32
            pos += 4
        elif vtype == 5: # int32
            pos += 4
        elif vtype == 6: # float32
            pos += 4
        elif vtype == 7: # bool
            pos += 1
        elif vtype == 8: # string
            slen = struct.unpack_from('<Q', data, pos)[0]
            pos += 8 + slen
        elif vtype == 9: # array
            atype = struct.unpack_from('<I', data, pos)[0]; pos += 4
            acount = struct.unpack_from('<Q', data, pos)[0]; pos += 8
            if atype == 8:  # string array
                for _ in range(acount):
                    slen = struct.unpack_from('<Q', data, pos)[0]
                    pos += 8 + slen
            elif atype in (4, 5):  # uint32/int32 array
                pos += 4 * acount
            elif atype == 6:  # float32 array
                pos += 4 * acount
            elif atype in (0, 1, 7):  # uint8/int8/bool array
                pos += 1 * acount
            elif atype in (2, 3):  # uint16/int16 array
                pos += 2 * acount
            elif atype in (10, 11):  # uint64/int64 array
                pos += 8 * acount
            elif atype == 12:  # float64 array
                pos += 8 * acount
            else:
                pos += 4 * acount  # guess
        elif vtype == 10: # uint64
            pos += 8
        elif vtype == 11: # int64
            pos += 8
        elif vtype == 12: # float64
            pos += 8
        else:
            pos += 4  # unknown
    return pos

def extract_q8_raw(data, offset, n_weights):
    """Extract RAW int8 quantized values from Q8_0 blocks (before scale).
    This shows the true weight distribution before block normalization.
    Q8_0: [float16 scale] [int8 x 32], 34 bytes/block, 32 weights/block.
    """
    weights = []
    n_blocks = (n_weights + 31) // 32
    for b in range(n_blocks):
        boff = offset + b * 34
        if boff + 34 > len(data):
            break
        n_in = min(32, n_weights - b * 32)
        for i in range(n_in):
            w = struct.unpack_from('<b', data, boff + 2 + i)[0]
            weights.append(w)  # raw int8, before scale
    return weights

def extract_f16(data, offset, n_weights):
    """Extract F16 weights."""
    weights = []
    for i in range(n_weights):
        pos = offset + i * 2
        if pos + 2 > len(data):
            break
        w = struct.unpack_from('<e', data, pos)[0]
        weights.append(int(round(w)))
    return weights

def extract_f32(data, offset, n_weights):
    """Extract F32 weights."""
    weights = []
    for i in range(n_weights):
        pos = offset + i * 4
        if pos + 4 > len(data):
            break
        w = struct.unpack_from('<f', data, pos)[0]
        weights.append(int(round(w)))
    return weights

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <model.gguf> [max_tensors=5]")
        sys.exit(1)

    path = sys.argv[1]
    max_tensors = int(sys.argv[2]) if len(sys.argv) > 2 else 5

    size = os.path.getsize(path)
    print(f"File: {path} ({size/1024/1024:.1f} MB)\n")

    with open(path, 'rb') as f:
        data = f.read()

    # Parse GGUF header
    pos = 0
    magic = data[0:4]; pos += 4
    if magic != b'GGUF':
        print("Not a GGUF file"); sys.exit(1)
    
    version = struct.unpack_from('<I', data, pos)[0]; pos += 4
    tensor_count = struct.unpack_from('<Q', data, pos)[0]; pos += 8
    kv_count = struct.unpack_from('<Q', data, pos)[0]; pos += 8
    
    print(f"GGUF v{version}, {tensor_count} tensors, {kv_count} metadata entries")
    
    pos = skip_metadata(data, pos, kv_count)
    print(f"Metadata end offset: {pos}")
    
    infos = read_tensor_infos(data, pos, tensor_count)
    
    # Advance pos past tensor infos
    # read_tensor_infos returns the position after all infos
    # We need to recompute: pos after reading tensor_count records
    tmp_pos = pos
    for _ in range(tensor_count):
        _, tmp_pos = read_str(data, tmp_pos)  # name
        n_dims = struct.unpack_from('<I', data, tmp_pos)[0]; tmp_pos += 4
        tmp_pos += 8 * n_dims  # dims
        tmp_pos += 4  # type
        tmp_pos += 8  # offset
    tensor_info_end = tmp_pos
    
    # Align tensor data to 32 bytes
    tdata_start = tensor_info_end
    if tdata_start % 32 != 0:
        tdata_start = (tdata_start + 31) & ~31
    
    # List all tensors with their types
    print(f"\n{'Type':>6} {'#weights':>12} {'Name'}")
    print("-" * 80)
    for name, dims, typ, offset, n_weights in infos:
        tn = TYPE_NAMES.get(typ, f'?{typ}')
        print(f"{tn:>6} {n_weights:>12,}  {name[:60]}")
    
    # Extract weights
    all_weights = []
    tensors_done = 0
    
    for name, dims, typ, offset, n_weights in infos:
        if tensors_done >= max_tensors:
            break
        
        actual_offset = tdata_start + offset
        
        # Skip embedding/output layers (too large, often separate scale)
        skip_keywords = ['embed', 'tok_embed', 'output', 'lm_head', 'norm', 'rms_norm']
        if any(k in name.lower() for k in skip_keywords):
            continue
        
        if typ == GGML_TYPE_Q8_0:
            ws = extract_q8_raw(data, actual_offset, n_weights)
        elif typ == GGML_TYPE_F16:
            ws = extract_f16(data, actual_offset, n_weights)
        elif typ == GGML_TYPE_F32:
            ws = extract_f32(data, actual_offset, n_weights)
        else:
            continue
        
        if ws:
            all_weights.extend(ws)
            tensors_done += 1
            print(f"\n  [{TYPE_NAMES.get(typ,'?')}] {name[:50]}: {len(ws):,} weights extracted")
            
            # Per-tensor stats
            layer_hist = Counter(ws)
            layer_total = len(ws)
            layer_entropy = -sum(c/layer_total*math.log2(c/layer_total) 
                                  for c in layer_hist.values())
            print(f"    entropy={layer_entropy:.3f} bits/weight, "
                  f"range=[{min(ws)},{max(ws)}], "
                  f"zeros={layer_hist.get(0,0)} ({layer_hist.get(0,0)/layer_total*100:.1f}%)")
    
    if not all_weights:
        print("\nNo weights extracted!")
        sys.exit(1)
    
    total = len(all_weights)
    hist = Counter(all_weights)
    entropy = -sum(c/total*math.log2(c/total) for c in hist.values())
    nonzero = len(hist)
    
    print(f"\n{'='*70}")
    print(f"OVERALL HISTOGRAM — {total:,} weights from {tensors_done} tensors")
    print(f"{'='*70}")
    print(f"Unique values: {nonzero}/256")
    print(f"Shannon entropy: {entropy:.4f} bits/weight")
    print(f"Fixed Q8:        8.000 bits/weight")
    print(f"Variable floor:  {entropy:.3f} bits/weight\n")
    
    # Show distribution by magnitude range
    mag_ranges = [
        ('|w|=0      ', lambda v: v == 0),
        ('|w|=1-3    ', lambda v: 1 <= abs(v) <= 3),
        ('|w|=4-15   ', lambda v: 4 <= abs(v) <= 15),
        ('|w|=16-63  ', lambda v: 16 <= abs(v) <= 63),
        ('|w|=64-127 ', lambda v: 64 <= abs(v) <= 127),
    ]
    
    print(f"{'Range':>12} {'Count':>12} {'%':>8} {'Accum%':>8}")
    print("-" * 44)
    accum = 0
    for rname, rfunc in mag_ranges:
        rcount = sum(c for v,c in hist.items() if rfunc(v))
        accum += rcount
        print(f"{rname:>12} {rcount:>12,} {rcount/total*100:>7.3f}% {accum/total*100:>7.3f}%")
    
    print()
    
    # Top 10 most frequent values
    print(f"{'Value':>6} {'Count':>12} {'%':>8} {'BeamCode':>8}")
    print("-" * 40)
    for v, c in sorted(hist.items(), key=lambda x: -x[1])[:10]:
        beamcode = (v + 128) & 0xFF
        print(f"{v:>6} {c:>12,} {c/total*100:>7.3f}% {beamcode:>8}")
    
    print()
    
    # Variable-length code strategies
    print(f"{'='*70}")
    print(f"VARIABLE-LENGTH CODE ESTIMATION")
    print(f"{'='*70}\n")
    
    schemes = [
        ("Uniform 8b",     [(0, 255, 8)]),
        ("1|5|9   ",       [(0,0,1),(1,7,6),(8,255,9)]),
        ("1|5|7|9 ",       [(0,0,1),(1,3,5),(4,15,8),(16,255,9)]),
        ("1|5|7|9|12",     [(0,0,1),(1,3,5),(4,15,8),(16,63,10),(64,255,12)]),
        ("1|6|8|10",       [(0,0,1),(1,7,6),(8,31,9),(32,255,11)]),
        ("3|6|9   ",       [(0,0,3),(1,7,7),(8,255,9)]),
        ("Zigzag 1|4|8",   [(0,0,1),(1,3,4),(4,255,8)]),
        ("Zigzag 1|4|7|10",[(0,0,1),(1,3,4),(4,15,8),(16,255,10)]),
    ]
    
    print(f"{'Scheme':>15} {'Avg bits':>10} {'Total MB':>10} {'Saved MB':>10}")
    print("-" * 50)
    q8_mb = total * 8 / 8 / 1024 / 1024
    q8_avg = 8.0
    for sname, levels in schemes:
        total_cost = 0
        for v, count in hist.items():
            absv = abs(v)
            bits = 8
            for lo, hi, b in levels:
                if lo <= absv <= hi:
                    bits = b
                    break
            total_cost += count * bits
        avg = total_cost / total
        mb = total_cost / 8 / 1024 / 1024
        saved = q8_mb - mb
        print(f"{sname:>15} {avg:>10.4f} {mb:>10.2f} {saved:>10.2f}")
    
    print(f"\n{'':>15} {'------':>10} {'------':>10}")
    print(f"{'Q8 fixed':>15} {q8_avg:>10.2f} {q8_mb:>10.2f} {'0.00':>10}")
    print(f"{'Entropy floor':>15} {entropy:>10.4f} {entropy*total/8/1024/1024:>10.2f}")

if __name__ == '__main__':
    main()
