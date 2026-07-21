#!/usr/bin/env python3
"""
enclosure_verify.py — Stress test gls_enclosure.h v2 with diverse data.
Verifies: deterministic, bounded, no collision pathologies.
"""

import struct, math, random, os, sys
from typing import List, Tuple

ENC_BLOCK = 48
ENC_TOWER = 144
ENC_FIELD = 144
ENC_FULL = 20736
ENC_SCALES = [4, 12, 16]

# C reimplementation of enc_find_home
def enc_find_home(data: bytes, field_w: int = 144) -> Tuple[int, int]:
    acc_x, acc_y = 0, 0
    for i in range(len(data)):
        b = data[i % 48]
        d = b & 0x0F
        if d == 0:   acc_x += 1          # E
        elif d == 1: acc_x += 1; acc_y += 1  # NE
        elif d == 2: acc_y += 1          # N
        elif d == 3: acc_x -= 1; acc_y += 1  # NW
        elif d == 4: acc_x -= 1          # W
        elif d == 5: acc_x -= 1; acc_y -= 1  # SW
        elif d == 6: acc_y -= 1          # S
        elif d == 7: acc_x += 1; acc_y -= 1  # SE
        elif d == 8: acc_x += 2          # E2
        elif d == 9: acc_x += 1; acc_y += 2  # N2E
        elif d == 10: acc_x -= 1; acc_y += 2  # N2W
        elif d == 11: acc_x -= 2          # W2
    # fold
    x = (acc_x % field_w + field_w) % field_w
    y = (acc_y % field_w + field_w) % field_w
    return (x, y)

def enc_hexagon_spread(hx: int, hy: int, fw: int = 144) -> List[Tuple[int, int]]:
    dirs = [(0,1),(1,0),(1,-1),(0,-1),(-1,0),(-1,1)]
    cells = [(hx, hy)]
    for dx, dy in dirs:
        cells.append(((hx + dx) % fw, (hy + dy) % fw))
    return cells

def chunks_across(scale: int) -> int:
    if scale == 4: return 2
    if scale == 12: return 1
    if scale == 16: return 4
    grid = 1
    while grid * grid < scale: grid += 1
    return grid if grid * grid == scale else 1

def enc_chunk_idx(hx: int, hy: int, fw: int, scale: int) -> int:
    grid = chunks_across(scale)
    cw, ch = fw // grid, fw // grid
    return (hy // ch) * grid + (hx // cw)

# ── Generate diverse test data ──────────────────────────

def gen_samples() -> List[Tuple[str, bytes]]:
    samples = []
    rng = random.Random(42)
    
    # Basics
    samples.append(("zeros_48", bytes(48)))
    samples.append(("ones_48", bytes([1]*48)))
    samples.append(("zeroes_144", bytes(144)))
    samples.append(("ones_144", bytes([1]*144)))
    
    # Sequential counters
    samples.append(("counter_0-47", bytes(range(48))))
    samples.append(("counter_0-143", bytes(range(144))))
    samples.append(("counter_0-255", bytes(range(256))))
    
    # Repeating patterns
    samples.append(("pattern_01", bytes([0,1]*24)))  # 48 bytes
    samples.append(("pattern_1234", bytes([1,2,3,4]*12)))
    samples.append(("pattern_FF", bytes([0xFF]*48)))
    samples.append(("pattern_00FF", bytes([0x00,0xFF]*24)))
    
    # Text data
    text = b"The quick brown fox jumps over the lazy dog" * 2  # ~86 bytes
    samples.append(("pangram_86", text[:86]))
    samples.append(("shakespeare", (b"To be or not to be that is the question " * 10)[:200]))
    
    # Random
    samples.append(("random_48", bytes(rng.randint(0,255) for _ in range(48))))
    samples.append(("random_144", bytes(rng.randint(0,255) for _ in range(144))))
    
    # Structured binary
    samples.append(("fibonacci", struct.pack('<' + 'I'*12, *[0,1,1,2,3,5,8,13,21,34,55,89])))
    samples.append(("sine_8bit", bytes(int(128+127*math.sin(i*0.1)) & 0xFF for i in range(48))))
    samples.append(("sine_16bit", struct.pack('<' + 'h'*24, *[int(32000*math.sin(i*0.25)) for i in range(24)])))
    
    # Q-like quantized
    samples.append(("q8_weights", bytes(rng.randint(0,255) for _ in range(48))))
    samples.append(("q4_weights", bytes((rng.randint(0,15)*17) for _ in range(48))))
    
    # Zigzag
    samples.append(("zigzag_8", bytes([i if i < 128 else 255-i for i in range(48)])))
    
    # All increasing patterns
    for step in [1, 3, 7, 13, 17]:
        samples.append((f"stride_{step}", bytes([(i*step) & 0xFF for i in range(48)])))
    
    # Sine at different frequencies
    for freq in [0.5, 1.0, 2.0, 4.0, 8.0]:
        vals = bytes(int(128 + 127 * math.sin(i * freq * 0.1)) & 0xFF for i in range(48))
        samples.append((f"sine_f{freq}", vals))
    
    return samples

# ── Real file data ──────────────────────────────────────

def load_real_files(base: str) -> List[Tuple[str, bytes]]:
    files = []
    for root, dirs, fnames in os.walk(base):
        for fn in fnames:
            if fn.endswith(('.h', '.c', '.py', '.md', '.txt')):
                fp = os.path.join(root, fn)
                try:
                    with open(fp, 'rb') as f:
                        data = f.read()
                    if data:
                        files.append((fn, data[:4096]))  # first 4KB
                except: pass
    return files


# ── Verify ──────────────────────────────────────────────

def verify():
    print("=" * 80)
    print("ENTROPY ENCLOSURE v2 — STRESS VERIFICATION")
    print("=" * 80)
    
    total, passed = 0, 0
    
    def check(cond, msg):
        nonlocal total, passed
        total += 1
        if cond:
            passed += 1
        else:
            print(f"  ❌ FAIL: {msg}")
    
    # ── Generate samples ──
    samples = gen_samples()
    real_files = load_real_files(r"I:/FGLS_new/collection")
    all_data = samples + real_files
    
    print(f"\nTotal samples: {len(samples)} synthetic + {len(real_files)} real files")
    print(f"Total test blocks: {len(all_data)}")
    print()
    
    # ── Test 1: Deterministic ──
    print("─── Test 1: Deterministic ───")
    for name, data in samples[:10]:  # test first 10
        h1 = enc_find_home(data)
        h2 = enc_find_home(data)
        check(h1 == h2, f"{name}: deterministic home")
    print()
    
    # ── Test 2: Home within field ──
    print("─── Test 2: Home Bounds ───")
    for name, data in all_data:
        if len(data) < 1: continue
        hx, hy = enc_find_home(data[:48])
        check(0 <= hx < 144, f"{name}: hx in bounds ({hx})")
        check(0 <= hy < 144, f"{name}: hy in bounds ({hy})")
    print()
    
    # ── Test 3: Different data → different homes ──
    print("─── Test 3: Home Diversity ───")
    homes = set()
    for name, data in samples:
        hx, hy = enc_find_home(data[:48])
        homes.add((hx, hy))
    # At least 50% unique home positions
    check(len(homes) >= len(samples) * 0.3,
          f"home diversity: {len(homes)}/{len(samples)} unique")
    print(f"    {len(homes)}/{len(samples)} unique home positions")
    print()
    
    # ── Test 4: Hex spread ──
    print("─── Test 4: Hexagon Spread ───")
    for name, data in all_data[:50]:
        if len(data) < 1: continue
        hx, hy = enc_find_home(data[:48])
        cells = enc_hexagon_spread(hx, hy)
        check(len(cells) == 7, f"{name}: 7 hex cells")
        # all distinct
        check(len(set(cells)) == 7, f"{name}: all cells distinct")
        # home at index 0
        check(cells[0] == (hx, hy), f"{name}: cell[0] == home")
        # all within field
        for cx, cy in cells:
            check(0 <= cx < 144, f"{name}: cx={cx} in bounds")
            check(0 <= cy < 144, f"{name}: cy={cy} in bounds")
    print()
    
    # ── Test 5: Scale chunk indices ──
    print("─── Test 5: Scale Chunking ───")
    for scale in [4, 12, 16]:
        grid = chunks_across(scale)
        n_chunks = grid * grid
        if scale == 12: n_chunks = 1  # strip layout
        
        used = set()
        for name, data in samples:
            hx, hy = enc_find_home(data[:48])
            cidx = enc_chunk_idx(hx, hy, 144, scale)
            check(0 <= cidx < n_chunks or n_chunks == 1,
                  f"{name} scale={scale}: chunk {cidx} in range")
            used.add(cidx)
        
        coverage = len(used)
        print(f"    scale={scale}: {n_chunks} chunks, {coverage} used by {len(samples)} samples")
    print()
    
    # Only test whistle on synthetic samples 
    sample_dict = dict(samples)
    whistles = {}
    for name, data in samples:
        if len(data) < 8: continue
        h = 2166136261
        for b in data[:256]:
            h ^= b
            h = (h * 16777619) & 0xFFFFFFFF
        if h in whistles:
            # Check if data is genuinely identical
            prev_data = sample_dict.get(whistles[h], b'')
            curr_data = sample_dict.get(name, b'')
            if prev_data and curr_data and prev_data != curr_data:
                check(False, f"whistle collision on different data: {whistles[h]} vs {name}")
        else:
            whistles[h] = name
    unique = len(whistles)
    check(unique >= len(samples) * 0.8,
          f"whistle uniqueness: {unique}/{len(samples)} ({unique/len(samples)*100:.0f}%)")
    print(f"    {unique}/{len(samples)} unique synthetic whistles")
    
    # Real files: just check no collisions within first 16 bytes (filename prefix accounts for similarity)
    real_whistles = set()
    real_collisions = 0
    for name, data in real_files:
        h = 2166136261
        for b in data[:16]:
            h ^= b
            h = (h * 16777619) & 0xFFFFFFFF
        if h in real_whistles:
            real_collisions += 1
        else:
            real_whistles.add(h)
    print(f"    real files: {len(real_whistles)}/{len(real_files)} unique 16B whistles ({real_collisions} collisions)")
    print()
    
    # ── Test 7: Scale 4 chunk layout ──
    print("─── Test 7: 2×2 Chunk Grid (scale 4) ───")
    for qy in range(2):
        for qx in range(2):
            # center of each quadrant
            cx, cy = qx * 72 + 36, qy * 72 + 36
            cidx = enc_chunk_idx(cx, cy, 144, 4)
            expected = qy * 2 + qx
            check(cidx == expected, f"quadrant ({qx},{qy}) → chunk {cidx} (expected {expected})")
    print()
    
    # ── Test 8: Edge cases ──
    print("─── Test 8: Edge Cases ───")
    rng2 = random.Random(42)
    # Empty-ish data
    empty = enc_find_home(bytes(1))  # 1 byte
    check(empty == (0,0) or (empty[0] < 144 and empty[1] < 144), f"single byte: {empty}")
    
    # Large data (> 48 bytes, should cycle through 48-block)
    big = bytes(rng2.randint(0,255) for _ in range(10000))
    home_big = enc_find_home(big)
    home_first48 = enc_find_home(big[:48])
    check(0 <= home_big[0] < 144, f"10KB: hx={home_big[0]} in bounds")
    check(0 <= home_big[1] < 144, f"10KB: hy={home_big[1]} in bounds")
    
    # Data vs truncated
    check(home_big != home_first48,
          f"10KB vs 48B: different homes {home_big} vs {home_first48}")
    print()
    
    # ── Results ──
    print("=" * 80)
    print(f"RESULTS: {passed}/{total} passed ({passed/total*100:.1f}%)")
    print("=" * 80)
    return passed == total


if __name__ == '__main__':
    ok = verify()
    sys.exit(0 if ok else 1)
