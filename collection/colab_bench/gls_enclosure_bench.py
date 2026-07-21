#!/usr/bin/env python3
"""
gls_enclosure_bench.py — Entropy Enclosure Field Scaling Benchmark

Tests:
  1. Per-data-type classification at scale=1 (48-byte blocks)
  2. Scale sweep (1, 2, 4, 8, 16, 32, 144) on each data type
  3. Extent classification (whole frames at scale S)
  4. Real binary files from project
  5. Frame seek integration with scaled fields
  6. Optimal scale recommendation per data type
"""

import os, sys, math, random, struct
from typing import List, Tuple, Dict

# ──────────────────────────────────────────────────────────────
# Enclosure constants
# ────────────────────────────────────────────────────────────
ENC_METATRON_COLS   = 4
ENC_METATRON_ROWS   = 4
ENC_METATRON_FLOORS = 3
ENC_METATRON_CELLS  = 16
ENC_BLOCK           = 48
ENC_TOWER           = 144
ENC_FULL            = 20736
ENC_FRAMES          = 12
ENC_CLOCK           = 1440
ENC_CLOCK_STRIDE    = 37
ENC_PENTAGONS       = 12

# ──────────────────────────────────────────────────────────────
# Core enclosure functions
# ────────────────────────────────────────────────────────────

def hilbert_4x4(x, y):
    d = 0
    rx, ry = (x & 2) >> 1, (y & 2) >> 1
    d = (d << 2) | ((3 * rx) ^ ry)
    if ry == 0:
        if rx: x, y = 3 - x, 3 - y
        x, y = y, x
    rx, ry = (x & 1), (y & 1)
    d = (d << 2) | ((3 * rx) ^ ry)
    return d

def peano_4x4(x, y):
    return x * 4 + (3 - y) if (x & 1) else x * 4 + y

def maze_coord(bc):
    f = bc // 16
    loc = bc % 16
    x, y = loc % 4, loc // 4
    return {'floor': f, 'x': x, 'y': y, 'hilbert': hilbert_4x4(x, y), 'peano': peano_4x4(x, y)}

def classify_block(block: bytes) -> dict:
    """Classify one 48-byte block → match count + shell"""
    assert len(block) == 48
    matches = 0
    for i in range(48):
        m = maze_coord(i)
        h = m['floor'] * 16 + m['hilbert']
        p = m['floor'] * 16 + m['peano']
        if block[h] == block[p]:
            matches += 1
    shell = 'STRONG' if matches >= 32 else ('WEAK' if matches >= 8 else 'CHAOS')
    return {'matches': matches, 'ratio': matches/48, 'shell': shell}

def classify_extent(data: bytes, scale: int) -> dict:
    """Classify all blocks in a scaled extent"""
    extent = scale * ENC_BLOCK
    n = len(data) // extent
    if n == 0:
        return {'blocks': 0, 'strong': 0, 'weak': 0, 'chaos': 0, 'avg': 0, 'samples': []}
    
    total_s = total_w = total_c = total_m = 0
    samples = []
    for i in range(min(n, 100)):  # sample up to 100 extents
        extent_data = data[i*extent:(i+1)*extent]
        if len(extent_data) < ENC_BLOCK:
            continue
        bcount = len(extent_data) // ENC_BLOCK
        s = w = c = m = 0
        for bi in range(bcount):
            blk = extent_data[bi*ENC_BLOCK:(bi+1)*ENC_BLOCK]
            if len(blk) < ENC_BLOCK:
                continue
            r = classify_block(blk)
            m += r['matches']
            if r['shell'] == 'STRONG': s += 1
            elif r['shell'] == 'WEAK': w += 1
            else: c += 1
        total_s += s; total_w += w; total_c += c; total_m += m
        if i < 5:  # store first 5 samples for detail
            samples.append({'extent': i, 'strong': s, 'weak': w, 'chaos': c, 'avg': m/bcount if bcount else 0})
    
    total_b = total_s + total_w + total_c
    return {
        'blocks': total_b, 'strong': total_s, 'weak': total_w, 'chaos': total_c,
        'avg': total_m / total_b if total_b else 0,
        'samples': samples,
    }

# ──────────────────────────────────────────────────────────────
# Data generators
# ────────────────────────────────────────────────────────────

def gen_random(n): return bytes(random.randint(0,255) for _ in range(n))
def gen_zeros(n): return b'\x00' * n
def gen_uniform(n, v=0xAA): return bytes([v] * n)
def gen_counting(n): return bytes(i % 256 for i in range(n))
def gen_fibonacci(n):
    r = bytearray(n); a, b = 0, 1
    for i in range(n): r[i] = a & 0xFF; a, b = b, a + b
    return bytes(r)
def gen_prime_markers(n):
    r = bytearray(n)
    is_prime = bytearray(b'\x01') * n
    if n > 0: is_prime[0] = 0
    if n > 1: is_prime[1] = 0
    for i in range(2, int(n**0.5)+1):
        if is_prime[i]:
            for j in range(i*i, n, i): is_prime[j] = 0
    for i in range(n): r[i] = 0xFF if is_prime[i] else 0x00
    return bytes(r)
def gen_sine(n): return bytes(int(128 + 127 * math.sin(i * 0.1)) for i in range(n))
def gen_step(n):
    return bytes([0xFF if (i//16)%2==0 else 0x00 for i in range(n)])
def gen_text_repeat(n, t="HelloWorld! "):
    return ((t * (n//len(t)+1))[:n]).encode('ascii')
def gen_natural_text(n):
    words = ["the","be","to","of","and","a","in","that","have","i","it","for","not",
             "on","with","he","as","you","do","at","this","but","his","by","from",
             "they","we","say","her","she","or","an","will","my","one","all","would",
             "there","their","what","so","up","out","if","about","who","get","which","go","me"]
    r = bytearray()
    while len(r) < n:
        r.extend(random.choice(words).encode('ascii'))
        r.append(ord(' '))
    return bytes(r[:n])
def gen_q8_weights(n):
    r = bytearray(n)
    for i in range(0, n, 33):
        if i < n: r[i] = random.randint(1, 200)
        for j in range(1, min(33, n-i)):
            r[i+j] = random.randint(-127, 127) & 0xFF
    return bytes(r)
def gen_q4_weights(n):
    r = bytearray(n)
    for i in range(0, n, 17):
        if i < n: r[i] = random.randint(1, 200)
        for j in range(1, min(17, n-i)):
            v1, v2 = random.randint(0,15), random.randint(0,15)
            r[i+j] = (v1 << 4) | v2
    return bytes(r)

DATA_GENERATORS = {
    'zeros': gen_zeros,
    'uniform_0xAA': lambda n: gen_uniform(n, 0xAA),
    'uniform_0xFF': lambda n: gen_uniform(n, 0xFF),
    'step_wave': gen_step,
    'prime_markers': gen_prime_markers,
    'text_repeat': lambda n: gen_text_repeat(n, "HelloWorld! "),
    'natural_text': gen_natural_text,
    'sine_wave': gen_sine,
    'random': gen_random,
    'q8_weights': gen_q8_weights,
    'q4_weights': gen_q4_weights,
    'counting': gen_counting,
    'fibonacci': gen_fibonacci,
}

# ──────────────────────────────────────────────────────────────
# Benchmark
# ────────────────────────────────────────────────────────────

def bench_block_level(samples=200):
    """Classify individual 48-byte blocks from each data type"""
    print("=" * 100)
    print("1. BLOCK-LEVEL CLASSIFICATION (scale=1, 48-byte blocks)")
    print("=" * 100)
    print(f"{'Data Type':<22} {'AvgMatch':<10} {'Ratio':<8} {'S%':<6} {'W%':<6} {'C%':<6}  Distribution")
    print("-" * 100)
    
    rows = []
    for name, gen in DATA_GENERATORS.items():
        data = gen(samples * ENC_BLOCK)
        s = w = c = m = 0
        for i in range(samples):
            blk = data[i*ENC_BLOCK:(i+1)*ENC_BLOCK]
            r = classify_block(blk)
            m += r['matches']
            if r['shell'] == 'STRONG': s += 1
            elif r['shell'] == 'WEAK': w += 1
            else: c += 1
        avg = m / samples
        sp = s/samples*100; wp = w/samples*100; cp = c/samples*100
        bar = ('█'*int(sp/5)) + ('▓'*int(wp/5)) + ('░'*int(cp/5))
        rows.append({'name': name, 'avg': avg, 'sp': sp, 'wp': wp, 'cp': cp})
        print(f"  {name:<22} {avg:<10.1f} {avg/48:<8.3f} {sp:<6.1f} {wp:<6.1f} {cp:<6.1f} {bar}")
    
    print()
    return rows

def bench_scale_sweep(samples=50):
    """Sweep scale=1,2,4,8,16,32,144 on each data type"""
    print("=" * 100)
    print("2. SCALE SWEEP — avg matches per block at each scale")
    print("=" * 100)
    print(f"{'Data Type':<22} ", end="")
    scales = [1, 2, 4, 8, 16, 32, 144]
    for s in scales:
        print(f"{'S='+str(s):<10}", end="")
    print()
    print("-" * 100)
    
    for name, gen in DATA_GENERATORS.items():
        # Generate enough data for the largest scale
        max_extent = max(scales) * ENC_BLOCK
        total_needed = max(samples * max_extent, 100 * ENC_BLOCK)
        data = gen(total_needed)
        
        print(f"  {name:<22} ", end="")
        for s in scales:
            r = classify_extent(data, s)
            if r['blocks'] > 0:
                print(f"{r['avg']:<10.1f}", end="")
            else:
                print(f"{'?':>10}", end="")
        print()
    
    print()

def bench_real_files():
    """Classify actual binary files from the project"""
    print("=" * 100)
    print("3. REAL BINARY FILES")
    print("=" * 100)
    
    search_paths = [
        (r"core", ".h"),
        (r"core", ".c"),
        (r"pipeline", ".c"),
        (r"tests", None),
    ]
    
    files_found = []
    for base_dir, ext in search_paths:
        full = os.path.join(os.getcwd(), base_dir)
        if not os.path.isdir(full):
            full = os.path.join(os.getcwd(), "..", base_dir) if os.path.isdir(os.path.join(os.getcwd(), "..", base_dir)) else None
        if full and os.path.isdir(full):
            for f in os.listdir(full):
                if ext and not f.endswith(ext): continue
                fp = os.path.join(full, f)
                if os.path.isfile(fp) and os.path.getsize(fp) >= 48:
                    files_found.append(fp)
                    if len(files_found) >= 20: break
        if len(files_found) >= 20: break
    
    if not files_found:
        # Try directories relative to script
        script_dir = os.path.dirname(os.path.abspath(__file__))
        for base in ["..", "../.."]:
            full = os.path.abspath(os.path.join(script_dir, base, "core"))
            if os.path.isdir(full):
                for f in os.listdir(full):
                    if f.endswith(".h") or f.endswith(".c"):
                        fp = os.path.join(full, f)
                        if os.path.isfile(fp) and os.path.getsize(fp) >= 48:
                            files_found.append(fp)
                            if len(files_found) >= 15: break
                break
    
    if not files_found:
        print("  (no binary files found nearby)")
        return
    
    print(f"{'File':<45} {'Size':<8} {'Blocks':<8} {'AvgMatch':<10} {'S%':<6} {'W%':<6} {'C%':<6} {'Best Scale':<12}")
    print("-" * 100)
    
    for fp in sorted(files_found)[:15]:
        try:
            with open(fp, 'rb') as fh:
                data = fh.read()
        except: continue
        
        fname = os.path.basename(fp)
        sz = len(data)
        if sz < 48: continue
        
        # Block-level at scale=1
        nblocks = sz // ENC_BLOCK
        s = w = c = m = 0
        for i in range(min(nblocks, 1000)):
            blk = data[i*ENC_BLOCK:(i+1)*ENC_BLOCK]
            r = classify_block(blk)
            m += r['matches']
            if r['shell'] == 'STRONG': s += 1
            elif r['shell'] == 'WEAK': w += 1
            else: c += 1
        avg = m / min(nblocks, 1000)
        sp = s/min(nblocks,1000)*100; wp = w/min(nblocks,1000)*100; cp = c/min(nblocks,1000)*100
        
        # Find best scale
        best_scale = 1
        best_avg = avg
        for test_s in [2, 4, 8, 16, 32, 64, 144]:
            ext = test_s * ENC_BLOCK
            if sz < ext: break
            nr = classify_extent(data, test_s)
            if nr['blocks'] > 0 and nr['avg'] > best_avg:
                best_avg = nr['avg']; best_scale = test_s
        
        best_label = f"S={best_scale}" if best_scale > 1 else "S=1"
        
        print(f"  {fname:<45} {sz:<8} {nblocks:<8} {avg:<10.1f} {sp:<6.1f} {wp:<6.1f} {cp:<6.1f} {best_label:<12}")
    
    print()

def bench_frame_seek():
    """Frame seek integration with scaled fields"""
    print("=" * 100)
    print("4. FRAME SEEK INTEGRATION")
    print("=" * 100)
    print()
    
    print("  Timeline (stride-37 on 1440 clock):")
    print(f"  {'Frame':<8} {'Timeline':<10} {'Scale=1 offset':<16} {'Scale=4 offset':<16} {'Scale=16 offset':<16}")
    print(f"  {'-'*66}")
    
    for fi in range(16):
        t = (fi * ENC_CLOCK_STRIDE) % ENC_CLOCK
        o1 = fi * 1 * ENC_BLOCK
        o4 = fi * 4 * ENC_BLOCK
        o16 = fi * 16 * ENC_BLOCK
        print(f"  {fi:<8} {t:<10} {o1:<16} {o4:<16} {o16:<16}")
    
    print()
    
    print("  Field capacity at each scale:")
    print(f"  {'Scale':<8} {'Bytes/Frame':<14} {'Total Field':<18} {'Enclosure Role':<30}")
    print(f"  {'-'*70}")
    for s in [1, 2, 4, 8, 16, 32, 64, 144, 1440]:
        bpf = s * ENC_BLOCK
        total = ENC_FULL * bpf
        if total < 1024:
            role = f"single block ({bpf}B per frame)"
        elif total < 1024*1024:
            role = f"frame = {bpf/1024:.1f} KB, field = {total/1024:.1f} KB"
        elif total < 1024*1024*1024:
            role = f"frame = {bpf/1024:.1f} KB, field = {total/(1024*1024):.1f} MB"
        else:
            role = f"frame = {bpf/(1024*1024):.1f} MB, field = {total/(1024*1024*1024):.1f} GB"
        print(f"  {s:<8} {bpf:<14} {total:<18} {role:<30}")
    
    print()

def print_recommendations(block_rows):
    """Print optimal settings per data type"""
    print("=" * 100)
    print("5. RECOMMENDATIONS")
    print("=" * 100)
    print()
    print(f"{'Data Type':<22} {'Avg Match':<10} {'Class':<10} {'Best Scale':<12} {'Enclosure Mode':<25}")
    print("-" * 100)
    
    for row in sorted(block_rows, key=lambda r: -r['avg']):
        avg = row['avg']
        name = row['name']
        
        if avg >= 32:
            cls = "STRONG"
            scale = 1
            mode = "SUB + Frame Seek"
        elif avg >= 8:
            cls = "WEAK"
            scale = 1
            mode = "SPARSE + Frame Seek"
        else:
            cls = "CHAOS"
            # Chaos data benefits from larger scale (averaging effect)
            # Test scale=4 to see if entropy averages out
            data = DATA_GENERATORS[name](200 * ENC_BLOCK)
            r4 = classify_extent(data, 4)
            r16 = classify_extent(data, 16)
            if r4['avg'] >= 8:
                scale = 4; cls = "WEAK*"; mode = "SPARSE + Scale=4"
            elif r16['avg'] >= 8:
                scale = 16; cls = "WEAK*"; mode = "SPARSE + Scale=16"
            else:
                scale = 1; mode = "RAW fallback"
        
        print(f"  {name:<22} {row['avg']:<10.1f} {cls:<10} S={scale:<8} {mode:<25}")
    
    print()
    print("  * WEAK* = scale averaging pulled matches above WEAK threshold")
    print("  Note: Tensor weights (Q8/Q4) fail because quantization block boundary")
    print("        does NOT align with Metatron block boundary (48 vs 33/17).")
    print()

# ──────────────────────────────────────────────────────────────
# Main
# ────────────────────────────────────────────────────────────

if __name__ == '__main__':
    random.seed(42)
    
    block_rows = bench_block_level(200)
    bench_scale_sweep(50)
    bench_real_files()
    bench_frame_seek()
    print_recommendations(block_rows)
    
    print("=" * 100)
    print("KEY INSIGHT: geo_jump 48 = atomic Hilbert unit.")
    print("  Subdivision = field scaling.  12-gon topology is invariant.")
    print("  Scale S = each enclosure frame holds S x 48 bytes.")
    print("  Hilbert maze still operates on 48-byte quanta internally.")
    print("=" * 100)
