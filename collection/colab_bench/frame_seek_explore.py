#!/usr/bin/env python3
"""
frame_seek_explore.py — เชื่อม enclosure home (x,y) → frame_seek enc (2B)
แล้วดูว่า frame_seek จะ manage storage ยังไง
"""

import math, random, struct
from typing import List, Tuple, Dict, Set

# ── Enclosure (Python port) ──────────────────────────────

ENC_FIELD = 144
ENC_FULL = 20736

def enc_find_home(data: bytes) -> Tuple[int, int]:
    acc_x, acc_y = 0, 0
    for i in range(len(data)):
        b = data[i % 48]
        d = b & 0x0F
        if d == 0:   acc_x += 1
        elif d == 1: acc_x += 1; acc_y += 1
        elif d == 2: acc_y += 1
        elif d == 3: acc_x -= 1; acc_y += 1
        elif d == 4: acc_x -= 1
        elif d == 5: acc_x -= 1; acc_y -= 1
        elif d == 6: acc_y -= 1
        elif d == 7: acc_x += 1; acc_y -= 1
        elif d == 8: acc_x += 2
        elif d == 9: acc_x += 1; acc_y += 2
        elif d == 10: acc_x -= 1; acc_y += 2
        elif d == 11: acc_x -= 2
    x = (acc_x % 144 + 144) % 144
    y = (acc_y % 144 + 144) % 144
    return (x, y)

# ── Frame Seek (Python port of core/geo_frame_seek.h) ──

FRAME_CYCLE   = 1440
FRAME_STRIDE  = 37
FRAME_FACE_SZ = 120
FRAME_EDGES   = 12
FRAME_H_ACTIVE = 9
FRAME_P_STEPS = 4
FRAME_ICO_NODES = 162

def frame_enc(t: int) -> int:
    return (t * FRAME_STRIDE) % FRAME_CYCLE

def frame_next(enc: int) -> int:
    return (enc + FRAME_STRIDE) % FRAME_CYCLE

def frame_prev(enc: int) -> int:
    return (enc + FRAME_CYCLE - FRAME_STRIDE) % FRAME_CYCLE

def frame_at(enc: int) -> dict:
    """Decompose enc → frame (same as C frame_at)"""
    face = enc // FRAME_FACE_SZ
    slot = enc % FRAME_FACE_SZ
    return {
        'enc': enc,
        'face': face,
        'slot': slot,
        'group': face % 3,
        'edge': enc % 3,
        'is_skip': (enc % FRAME_EDGES) >= FRAME_H_ACTIVE,
        'peano_step': (enc // 3) % FRAME_P_STEPS,
        'peano_sub': enc % 3,
        'ico_idx': enc % FRAME_ICO_NODES,
        'phase': (enc // FRAME_EDGES) % 12,
    }

# ── Mapping: home (x,y) → enc ────────────────────────────
# 144×144 field = 20736 positions
# frame_seek = 1440 positions (2 bytes)
# 
# fold: enc = (home_y * ENC_FIELD + home_x) % FRAME_CYCLE
# This maps every 20736/1440 = 14.4 home positions → 1 enc
# Collisions are OK — data's path determines unique home within group

def home_to_enc(hx: int, hy: int) -> int:
    return (hy * ENC_FIELD + hx) % FRAME_CYCLE

def generate_samples() -> List[Tuple[str, bytes]]:
    rng = random.Random(42)
    samples = []
    
    # Zero/constant patterns
    samples.append(("zeros", bytes(48)))
    samples.append(("ones", bytes([1]*48)))
    samples.append(("0xFF", bytes([0xFF]*48)))
    
    # Counters
    samples.append(("counter_0-47", bytes(range(48))))
    samples.append(("counter_0-143", bytes(range(144))))
    
    # Patterns
    for p in [(0xAA,0x55), (0x00,0xFF), (0x0F,0xF0), (0x01,0x80)]:
        samples.append((f"pat_{p[0]:02x}{p[1]:02x}", bytes([p[0],p[1]]*24)))
    
    # Text
    text = b"The quick brown fox jumps over the lazy dog " * 4
    samples.append(("pangram", text[:192]))
    samples.append(("shakespeare", (b"To be or not to be " * 20)[:200]))
    
    # Stride patterns
    for step in [1, 3, 7, 13, 17, 37]:
        samples.append((f"stride_{step}", bytes([(i*step) & 0xFF for i in range(48)])))
    
    # Random
    for i in range(20):
        samples.append((f"random_{i}", bytes(rng.randint(0,255) for _ in range(48))))
    
    # Sine waves
    for i, f in enumerate([0.5, 1.0, 2.0, 4.0, 8.0]):
        samples.append((f"sine_{i}", bytes(int(128+127*math.sin(i*f*0.1)) & 0xFF for i in range(48))))
    
    return samples

def explore():
    print("=" * 80)
    print("FRAME SEEK × ENCLOSURE — STORAGE EXPLORATION")
    print("=" * 80)
    
    samples = generate_samples()
    print(f"\nSamples: {len(samples)}")
    print()
    
    # ── 1. Map all samples → home → enc → frame ──
    frame_visits: Dict[int, List[str]] = {}
    face_visits: Dict[int, List[str]] = {}
    phase_visits: Dict[int, List[str]] = {}
    
    for name, data in samples:
        hx, hy = enc_find_home(data)
        enc = home_to_enc(hx, hy)
        f = frame_at(enc)
        
        frame_visits.setdefault(enc, []).append(name)
        face_visits.setdefault(f['face'], []).append(name)
        phase_visits.setdefault(f['phase'], []).append(name)
    
    n_frames = len(frame_visits)
    n_faces = len(face_visits)
    n_phases = len(phase_visits)
    
    print(f"─── Coverage ───")
    print(f"  Unique enc values: {n_frames}/{FRAME_CYCLE} ({n_frames/FRAME_CYCLE*100:.1f}%)")
    print(f"  Unique faces:      {n_faces}/12 ({n_faces/12*100:.1f}%)")
    print(f"  Unique phases:     {n_phases}/12 ({n_phases/12*100:.1f}%)")
    
    # ── 2. Phase grouping ──
    print(f"\n─── Phase Distribution ───")
    for p in range(12):
        count = len(phase_visits.get(p, []))
        bar = "#" * count
        print(f"  phase {p:2d}: {count:3d} {bar}")
    
    # ── 3. Storage Scenarios ──
    print(f"\n─── Storage Scenarios ───")
    
    # Scenario A: Store all 1440 frames (wasteful, baseline)
    full_store = FRAME_CYCLE  # 1440 × 2 bytes = 2880 bytes just for enc
    print(f"\n  A) Store ALL {FRAME_CYCLE} frames:")
    print(f"     {full_store} enc × 2B = {full_store*2} bytes (baseline)")
    
    # Scenario B: Store only used frames
    used_store = n_frames * 2
    reduction_b = (1 - (used_store / (FRAME_CYCLE * 2))) * 100
    print(f"  B) Store {n_frames} used frames only:")
    print(f"     {n_frames} enc × 2B = {used_store} bytes ({reduction_b:.1f}% reduction)")
    
    # Scenario C: Store by phase — only needed phases
    if n_phases > 0:
        phase_bits = math.ceil(math.log2(n_phases)) if n_phases > 1 else 1
        phase_store = n_frames * (1 + phase_bits)  # enc(2B) + phase index
        print(f"  C) Phase-indexed ({n_phases} phases, {phase_bits} bits/phase):")
        print(f"     {n_frames} × (2B+{phase_bits}b) ≈ {n_frames * 2} + {n_frames * phase_bits // 8} = {n_frames * 2 + n_frames * phase_bits // 8} bytes")
    
    # Scenario D: Store only face + slot (reconstruct enc)
    face_store = n_faces * 1  # one byte for face
    print(f"  D) Face-based ({n_faces} faces):")
    print(f"     {n_faces} × 1B = {face_store} bytes")
    
    # ── 4. Stride walk analysis ──
    print(f"\n─── Stride-47 Walk Connectivity ───")
    sorted_encs = sorted(frame_visits.keys())
    gaps = []
    for i in range(len(sorted_encs) - 1):
        gap = sorted_encs[i+1] - sorted_encs[i]
        gaps.append(gap)
    
    if gaps:
        avg_gap = sum(gaps) / len(gaps)
        max_gap = max(gaps)
        print(f"  Avg gap between used enc: {avg_gap:.1f}")
        print(f"  Max gap: {max_gap}")
    
    # How many stride-37 steps to traverse all used frames?
    # Build the walk order
    visited_set = set(sorted_encs)
    walk_order = []
    e = 0
    for _ in range(FRAME_CYCLE):
        if e in visited_set:
            walk_order.append(e)
        e = frame_next(e)
    
    if walk_order:
        # Continuous runs in walk order
        runs = 1
        for i in range(1, len(walk_order)):
            if walk_order[i] != frame_next(walk_order[i-1]):
                runs += 1
        print(f"  Runs in stride-37 walk: {runs}")
        print(f"  Walk accesses {len(walk_order)} used frames")
        if runs > 0:
            print(f"  Avg frames per run: {len(walk_order)/runs:.1f}")
    
    # ── 5. Collision analysis ──
    print(f"\n─── Home Collisions (same enc → different data) ───")
    collisions = [(enc, names) for enc, names in frame_visits.items() if len(names) > 1]
    if collisions:
        print(f"  {len(collisions)} collisions among {n_frames} used enc values")
        for enc, names in sorted(collisions)[:5]:
            print(f"    enc={enc:4d}: {', '.join(names[:4])}")
    else:
        print(f"  No collisions! {len(samples)} samples → {n_frames} unique enc")
    
    # ── 6. Reconstruction ──
    print(f"\n─── Frame Reconstruction (example) ───")
    for name in ["zeros", "counter_0-47", "pangram", "random_0"]:
        data = dict(samples)[name]
        hx, hy = enc_find_home(data)
        enc = home_to_enc(hx, hy)
        f = frame_at(enc)
        print(f"  {name:20s} → home({hx:3d},{hy:3d}) → enc={enc:4d} → face={f['face']} slot={f['slot']} phase={f['phase']} ico={f['ico_idx']}")
    
    print(f"\n{'='*80}")
    print(f"CONCLUSION: {n_frames}/{FRAME_CYCLE} frames used by {len(samples)} samples")
    print(f"Storage needed: {n_frames * 2} bytes (only used frames)")
    if n_frames > 0:
        comp = (FRAME_CYCLE * 2) / (n_frames * 2)
        print(f"Compression vs full timeline: {comp:.1f}×")
    
    # ── Key insight ──
    print(f"\n─── Key Question ───")
    print(f"Frame_seek stride-37 walks through all {FRAME_CYCLE} positions in a cycle.")
    print(f"Used frames = {n_frames}. If stride-37 walk visits them in {runs} runs,")
    print(f"we can store JUST these {n_frames} enc values + stride = O(1) navigation.")
    print(f"Each frame = 2 bytes. Total = {n_frames * 2} bytes for all sample data.")

if __name__ == '__main__':
    explore()
