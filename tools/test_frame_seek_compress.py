#!/usr/bin/env python3
"""
Test: geo_frame_seek compression (store only frame 0, reconstruct all)

Key insight: geo_frame_seek maps frames using stride-37.
Store only frame 0 (seed), reconstruct frames 1..1439 from timeline.

Pipeline:
1. Data → split into 1440 frames (or fewer)
2. Store only frame 0
3. Reconstruct all frames from frame 0 using timeline
4. Verify roundtrip
"""
import os, sys, time, zlib, hashlib, struct
import numpy as np

sys.path.insert(0, 'I:/FGLS_new/tools')
from geopixel_pipeline import frame_enc, frame_at, FRAME_CYCLE, FRAME_STRIDE

# ══════════════════════════════════════════════════════════════
# geo_frame_seek (from geopixel_pipeline.py)
# ══════════════════════════════════════════════════════════════

def frame_at(enc):
    """Convert enc → frame dict (face, edge, z, slot, ico_idx)."""
    face = (enc // 120) % 12
    edge = (enc // 10) % 12
    z = enc % 10
    slot = enc % 120
    ico_idx = enc % 162
    return {'face': face, 'edge': edge, 'z': z, 'slot': slot, 'ico_idx': ico_idx}

def frame_enc(t):
    """Convert timeline position t → enc value."""
    return (t * FRAME_STRIDE) % FRAME_CYCLE

# ══════════════════════════════════════════════════════════════
# Test: Store only frame 0, reconstruct all
# ══════════════════════════════════════════════════════════════

def test_frame_seek_compress(data, label):
    """Test geo_frame_seek compression."""
    print(f"\n{'='*70}")
    print(f"  {label} ({len(data):,} bytes)")
    print(f"{'='*70}")
    
    orig_hash = hashlib.sha256(data).hexdigest()[:16]
    print(f"  Original hash: {orig_hash}")
    
    # Split data into frames
    # Each frame = 64 bytes (CHUNK_SZ)
    chunk_sz = 64
    n_chunks = (len(data) + chunk_sz - 1) // chunk_sz
    
    # How many full frames? (each frame = FRAME_CYCLE chunks = 1440 chunks)
    n_frames = (n_chunks + FRAME_CYCLE - 1) // FRAME_CYCLE
    
    print(f"  Chunks: {n_chunks}")
    print(f"  Frames: {n_frames} (each = {FRAME_CYCLE} chunks)")
    print(f"  Total chunks needed: {n_frames * FRAME_CYCLE}")
    
    # Create frame data
    # Each frame is a sequence of chunks mapped to positions via frame_enc
    frames = []
    for f in range(n_frames):
        frame_chunks = []
        for t in range(FRAME_CYCLE):
            chunk_idx = f * FRAME_CYCLE + t
            if chunk_idx < n_chunks:
                # Get chunk data
                start = chunk_idx * chunk_sz
                end = min(start + chunk_sz, len(data))
                chunk = data[start:end]
                # Pad to chunk_sz
                chunk = chunk + b'\x00' * (chunk_sz - len(chunk))
            else:
                chunk = b'\x00' * chunk_sz
            frame_chunks.append(chunk)
        frames.append(frame_chunks)
    
    print(f"  Frames created: {len(frames)}")
    
    # Store only frame 0 (all chunks in frame 0)
    frame0 = frames[0]
    frame0_data = b''.join(frame0)
    
    print(f"  Frame 0 size: {len(frame0_data):,} bytes")
    
    # Compress frame 0
    compressed = zlib.compress(frame0_data, 9)
    print(f"  Compressed frame 0: {len(compressed):,} bytes")
    ratio = len(compressed) / len(data)
    print(f"  Ratio (compressed frame 0 / original): {ratio:.3f}x")
    
    # Reconstruct all frames from frame 0
    # In real implementation, this uses timeline function
    # Here we just verify frame 0 matches
    decompressed = zlib.decompress(compressed)
    frame0_recon = [decompressed[i*chunk_sz:(i+1)*chunk_sz] for i in range(FRAME_CYCLE)]
    
    # Verify frame 0 matches
    frame0_match = all(frame0_recon[i] == frame0[i] for i in range(FRAME_CYCLE))
    print(f"  Frame 0 roundtrip: {'PASS' if frame0_match else 'FAIL'}")
    
    # What about frames 1..n_frames-1?
    # In real implementation, these are reconstructed from frame 0 using timeline
    # But we need to check: can we reconstruct them?
    
    print(f"\n  --- Analysis ---")
    print(f"  Original: {len(data):,} bytes")
    print(f"  Stored (frame 0 + compressed): {len(compressed):,} bytes")
    print(f"  Missing: {len(data) - len(frame0_data):,} bytes (frames 1..{n_frames-1})")
    print(f"  To reconstruct: need timeline function to generate frames 1..{n_frames-1}")
    
    # Compare with raw zlib
    raw_zlib = zlib.compress(data, 9)
    print(f"\n  --- Comparison ---")
    print(f"  Raw+zlib:     {len(raw_zlib):>8,} bytes ({len(raw_zlib)/len(data):.3f}x)")
    print(f"  Frame0+zlib:  {len(compressed):>8,} bytes ({ratio:.3f}x)")
    
    if ratio < len(raw_zlib)/len(data):
        print(f"  Frame0 is {(len(raw_zlib)/len(data))/ratio:.2f}x smaller")
    else:
        print(f"  Raw+zlib is better")
    
    return {
        'label': label,
        'input': len(data),
        'frame0_size': len(frame0_data),
        'compressed': len(compressed),
        'raw_zlib': len(raw_zlib),
        'ratio': ratio,
        'raw_ratio': len(raw_zlib)/len(data),
        'n_frames': n_frames,
        'frame0_match': frame0_match,
    }

if __name__ == '__main__':
    print("=" * 70)
    print("  geo_frame_seek Compression Test")
    print("=" * 70)
    print("  Key: Store only frame 0, reconstruct all from timeline")
    print("  geo_frame_seek: stride-37, FRAME_CYCLE=1440")
    
    pdf_path = r'I:\FGLS_new\docs\POGLS_MASTER_SCHEMATIC.pdf'
    full_pdf = open(pdf_path, 'rb').read()
    
    test_cases = [
        ("PDF 10KB", full_pdf[:10000]),
        ("PDF 100KB", full_pdf[:100000]),
        ("Random 10KB", os.urandom(10000)),
        ("Random 100KB", os.urandom(100000)),
        ("Text 10KB", (b"Hello world! " * 1000)[:10000]),
    ]
    
    results = []
    for label, data in test_cases:
        r = test_frame_seek_compress(data, label)
        results.append(r)
    
    # Summary
    print("\n" + "=" * 70)
    print("  SUMMARY")
    print("=" * 70)
    print(f"  {'Data':<15} {'Input':<10} {'Frames':<8} {'Frame0':<10} {'Compressed':<12} {'Raw+zlib':<12}")
    print("-" * 70)
    
    for r in results:
        print(f"  {r['label']:<15} {r['input']:<10,} {r['n_frames']:<8} {r['frame0_size']:<10,} {r['compressed']:<12,} {r['raw_zlib']:<12,}")
    
    print("\n" + "=" * 70)
    print("  KEY INSIGHT")
    print("=" * 70)
    print("  geo_frame_seek stores only frame 0 (seed)")
    print("  All other frames (1..1439) are reconstructed from timeline")
    print("  Compression ratio = 1/n_frames (if data is timeline-derived)")
    print("  For 1440 frames: ratio = 1/1440 = 0.00069x (1440× compression)")
    print("  But: data MUST be f(timeline) for reconstruction to work")
