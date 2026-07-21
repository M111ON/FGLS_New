#!/usr/bin/env python3
"""
enclosure_data_sweep.py — Test enclosure on diverse real data types.
"""

import os, sys, struct, math, random
from typing import List, Tuple

random.seed(42)

ENC_BLOCK = 48

def hilbert_4x4(x, y):
    d = 0
    for s in (2, 1):
        rx, ry = (x & s) >> (s>>1), (y & s) >> (s>>1)
        d = (d << 2) | ((3 * rx) ^ ry)
        if ry == 0:
            if rx: x, y = 3 - x if s==2 else 1 - x, 3 - y if s==2 else 1 - y
            x, y = y, x
    return d

def peano_4x4(x, y):
    return x * 4 + (3 - y) if (x & 1) else x * 4 + y

def classify_block(block: bytes) -> dict:
    matches = 0
    for i in range(min(len(block), 48)):
        f = i // 16
        loc = i % 16
        x, y = loc % 4, loc // 4
        h = f * 16 + hilbert_4x4(x, y)
        p = f * 16 + peano_4x4(x, y)
        if block[h] == block[p]:
            matches += 1
    shell = 'STRONG' if matches >= 38 else ('WEAK' if matches >= 14 else 'CHAOS')
    return {'matches': matches, 'ratio': matches/48, 'shell': shell}

def classify_file(data: bytes, label: str):
    n = len(data) // ENC_BLOCK
    if n == 0: return None
    n = min(n, 1000)
    s = w = c = m = 0
    for i in range(n):
        blk = data[i*ENC_BLOCK:(i+1)*ENC_BLOCK]
        if len(blk) < ENC_BLOCK: break
        r = classify_block(blk)
        m += r['matches']
        if r['shell'] == 'STRONG': s += 1
        elif r['shell'] == 'WEAK': w += 1
        else: c += 1
    return {'label': label, 'size': len(data), 'blocks': n,
            'avg': m/n, 'strong%': s/n*100, 'weak%': w/n*100, 'chaos%': c/n*100}

# ── Generate diverse data types ──
samples = []

# 1. Real .dll binary (Windows PE)
dll_paths = [
    r"C:\Windows\System32\kernel32.dll",
    r"C:\Windows\System32\ntdll.dll",
    r"C:\mingw64\bin\gcc.exe",
]
for dp in dll_paths:
    if os.path.isfile(dp):
        with open(dp, 'rb') as f: data = f.read()
        samples.append(('PE_binary_' + os.path.basename(dp), data))
        break

# 2. Real .exe
exe_paths = [
    r"C:\Windows\System32\notepad.exe",
    r"C:\Windows\System32\calc.exe",
]
for ep in exe_paths:
    if os.path.isfile(ep):
        with open(ep, 'rb') as f: data = f.read()[:50000]
        samples.append(('PE_exe_' + os.path.basename(ep), data))
        break

# 3. Python bytecode (.pyc)
import py_compile
pyc_data = b''
try:
    import dis
    test_code = compile("x=1\ny=2\nz=x+y\n", '<test>', 'exec')
    pyc_data = test_code.co_code * 20
except: pass
if pyc_data:
    samples.append(('Python_bytecode', pyc_data))

# 4. JSON data
import json
json_data = json.dumps({
    "layers": [{"name": "conv1", "weights": [random.random() for _ in range(256)],
                "bias": [random.random() for _ in range(64)]} for _ in range(20)],
    "optimizer": "adam", "learning_rate": 0.001
}).encode() * 5
samples.append(('ML_json_config', json_data))

# 5. Simulated Q8_0 tensor (block: 1 scale + 32 weights)
def gen_q8_tensor(size):
    r = bytearray()
    for _ in range(size // 33):
        r.append(random.randint(1, 254))  # scale
        for _ in range(32):
            r.append(random.randint(-127, 127) & 0xFF)
    return bytes(r)
data = gen_q8_tensor(50000)
samples.append(('Q8_tensor(50K)', data))

# 6. Simulated Q4_0 tensor (block: 1 scale + 32 4-bit weights packed)
def gen_q4_tensor(size):
    r = bytearray()
    for _ in range(size // 17):
        r.append(random.randint(1, 254))  # scale
        for _ in range(16):
            v1, v2 = random.randint(0,15), random.randint(0,15)
            r.append((v1 << 4) | v2)
    return bytes(r)
data = gen_q4_tensor(50000)
samples.append(('Q4_tensor(50K)', data))

# 7. Real .bmp / image file
img_path = r"C:\Windows\Web\Screen\img100.jpg"
if not os.path.exists(img_path):
    img_path = r"C:\Windows\Web\Wallpaper\Windows\img0.jpg"
if os.path.isfile(img_path):
    with open(img_path, 'rb') as f: data = f.read()[:50000]
    samples.append(('JPEG_image', data))

# 8. All zeros (control)
data = b'\x00' * 50000
samples.append(('zero(50K)', data))

# 9. Repeating pattern (control)
data = (b'\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x0C\x0D\x0E\x0F\x10' * 3125)[:50000]
samples.append(('pattern_1-16(50K)', data))

# 10. Monotonic counter
data = struct.pack('<' + 'H'*25000, *range(25000))
samples.append(('counter_16bit(50K)', data))

# 11. Random bytes (control)
data = bytes(random.randint(0,255) for _ in range(50000))
samples.append(('random(50K)', data))

# 12. ASCII natural text from Shakespeare
shakespeare = (
    "To be, or not to be, that is the question: " * 1000 +
    "Whether 'tis nobler in the mind to suffer " * 1000
)[:50000].encode('ascii')
samples.append(('shakespeare_text', shakespeare))

# ── Run classification ──
print("=" * 110)
print("ENTROPY ENCLOSURE — DIVERSE DATA TYPE CLASSIFICATION")
print("=" * 110)
print(f"{'Data Type':<30} {'Size':<10} {'Blocks':<8} {'AvgMatch':<10} {'S%':<6} {'W%':<6} {'C%':<6}  {'Shell':<8}")
print("-" * 110)

results = []
for label, data in samples:
    r = classify_file(data, label)
    if not r:
        print(f"  {label:<30} {'too small':<10}")
        continue
    s = 'STRONG' if r['avg'] >= 38 else ('WEAK' if r['avg'] >= 14 else 'CHAOS')
    bar = ('█'*int(r['strong%']/5)) + ('▓'*int(r['weak%']/5)) + ('░'*int(r['chaos%']/5))
    print(f"  {r['label']:<30} {r['size']:<10} {r['blocks']:<8} {r['avg']:<10.1f} {r['strong%']:<6.1f} {r['weak%']:<6.1f} {r['chaos%']:<6.1f}  {s:<8}")
    results.append(r)

print()
print("=" * 110)
print("FINDINGS")
print("=" * 110)
print()

for r in sorted(results, key=lambda x: -x['avg']):
    note = ""
    if r['avg'] >= 38:
        note = "✅ GEOMETRIC — SUB encode with frame seek"
    elif r['avg'] >= 14:
        note = "⚠️  PARTIAL — SPARSE encode, some structure"
    else:
        effective = r['avg'] - 6  # subtract baseline
        note = f"❌ CHAOS (effective signal: {effective:.1f}/42) — RAW fallback"

    print(f"  {r['label']:<30} avg={r['avg']:<5.1f}  {note}")

print()
print("Baseline: 6/48 geometric invariant (Hilbert==Peano at entry/exit cells)")
print("Effective signal range: 0..42 (reported 6..48)")
print()
