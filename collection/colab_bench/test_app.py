"""Test RDH pipeline (standalone — no gradio)"""
import sys, time

# ── Copy pipeline functions from app_hf (no gradio) ──
FIELD_W = 144
FIELD_H = 144
FRAME_CYCLE = 1440
FRAME_FACE_SZ = 120
FRAME_STRIDE = 37

DIRS = {
    0: (1, 0), 1: (1, 1), 2: (0, 1), 3: (1, -1),
    4: (-1, 0), 5: (-1, -1), 6: (0, -1), 7: (-1, 1),
    8: (2, 0), 9: (1, 2), 10: (-1, 2), 11: (-2, 0),
}

def rdh_capture(data):
    acc_x, acc_y = 0, 0
    steps = max(len(data), 48)
    for i in range(steps):
        b = data[i % len(data)]
        d = b & 0x0F
        if d in DIRS:
            dx, dy = DIRS[d]
            acc_x += dx
            acc_y += dy
        if (i & 0xFFF) == 0xFFF:
            acc_x %= FIELD_W
            acc_y %= FIELD_H
    wedge = (acc_x % FIELD_W + FIELD_W) % FIELD_W
    ring  = (acc_y % FIELD_H + FIELD_H) % FIELD_H
    return ring * FIELD_W + wedge

def decompose(key):
    return key // FIELD_W, key % FIELD_W

def frame_at(enc):
    face   = enc // FRAME_FACE_SZ
    slot   = enc % FRAME_FACE_SZ
    phase  = (enc // 12) % 12
    ico    = enc % 162
    return face, slot, phase, ico

def pipeline(data):
    lines = []
    t0 = time.perf_counter()
    key = rdh_capture(data)
    t1 = time.perf_counter()
    ring, wedge = decompose(key)
    enc = key % FRAME_CYCLE
    face, slot, phase, ico = frame_at(enc)
    next_enc = (enc + FRAME_STRIDE) % FRAME_CYCLE
    prev_enc = (enc - FRAME_STRIDE + FRAME_CYCLE) % FRAME_CYCLE
    
    lines.append(f"Input:  {len(data)} bytes ({(t1-t0)*1e6:.1f} us)")
    lines.append(f"")
    lines.append(f"  rdh_capture -> key = {key} (0..{FIELD_W*FIELD_H-1})")
    lines.append(f"  decompose   -> home = ({wedge}, {ring})")
    lines.append(f"  enc (2B)    -> {enc} (0x{enc:04x})  ratio: {len(data)/max(2,2):.0f}x")
    lines.append(f"  frame_at    -> face={face} slot={slot} phase={phase} ico={ico}")
    lines.append(f"  stride-37   -> next={next_enc}  prev={prev_enc}")
    lines.append(f"")
    lines.append(f"  Timeline stride-37 walk:")
    v = enc
    for i in range(6):
        f, s, p, ic = frame_at(v)
        lines.append(f"    enc={v:4d}  face={f:2d} slot={s:3d} phase={p:2d} ico={ic:3d}")
        v = (v + FRAME_STRIDE) % FRAME_CYCLE
    lines.append(f"    ... (cycles through all 1440)")
    lines.append(f"")
    lines.append(f"  Robust: {'PASS' if key == rdh_capture(data) else 'FAIL'}")
    return "\n".join(lines)

# ── Tests ──
print("=== 48-byte pattern ===")
data = bytes((i * 3 + 7) & 0xFF for i in range(48))
print(pipeline(data))

print()
print("=== rdh_capture.h (1KB) ===")
with open("../../collection/rdh/rdh_capture.h", "rb") as f:
    data = f.read()
print(pipeline(data[:1024]))

print()
print("=== Deterministic ===")
a = rdh_capture(b"hello")
b = rdh_capture(b"hello")
print(f"  {'PASS' if a == b else 'FAIL'}: rdh_capture(b'hello') = {a} == {b}")

print()
print("=== Speed: 10MB simulated ===")
import random
r = random.Random(42)
big = bytes(r.randint(0,255) for _ in range(10_000_000))
t0 = time.perf_counter()
k = rdh_capture(big)
t1 = time.perf_counter()
print(f"  10MB -> key={k}  time={t1-t0:.2f}s  ({10/(t1-t0):.1f} MB/s)")
