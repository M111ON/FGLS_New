#!/usr/bin/env python3
"""
Prototype: geometric capture + deterministic walk + invert-only storage
Based on @sid-runner's mental model: Hilbert 3-skip-1, Peano textile,
bond chain, frame0 reconstruct.
"""
import struct, math, sys, os, zlib

# ── Core constants (per geo_frame_seek.h) ──
FRAME_CYCLE = 1440
FRAME_STRIDE = 37       # prime walk
FRAME_EDGES = 12        # 9 Hilbert active + 3 Peano
FRAME_H_ACTIVE = 9
FRAME_FACE_SZ = 120

# ── 1. Deterministic walker ──
class GeoWalk:
    """Generates frame positions deterministically from enc."""
    def __init__(self):
        pass

    def frame_at(self, enc):
        """Return position dict for a given enc (0..1439)."""
        enc = enc % FRAME_CYCLE
        face = enc // FRAME_FACE_SZ          # 0..11 dodeca face
        group = (enc // 3) % 3                # 0..2 Hilbert group
        edge = enc % 3                        # 0..2 edge within group
        is_skip = (enc % 4 == 3)              # every 4th = invert point

        # Peano (runs on invert/skip positions)
        peano_step = (enc // 3) % 4
        peano_sub = enc % 3
        ico_idx = enc % 162                   # icosphere L2 (0..161)
        phase = (enc // FRAME_EDGES) % 12

        return {
            'enc': enc, 'face': face, 'group': group, 'edge': edge,
            'is_skip': is_skip, 'peano_step': peano_step,
            'peano_sub': peano_sub, 'ico_idx': ico_idx, 'phase': phase,
        }

    def next_enc(self, enc):
        return (enc + FRAME_STRIDE) % FRAME_CYCLE

    def walk_positions(self, n):
        """Generate first n enc positions."""
        pos = []
        e = 0
        for _ in range(n):
            f = self.frame_at(e)
            pos.append(f)
            e = self.next_enc(e)
        return pos


# ── 2. Capture: data → geometric position ──
def capture_byte(byte_val, position_idx, grid_bits=8):
    """
    Map a single byte to a 'geometric position' on the grid.
    The grid is a 256×256 torus-like space. The byte determines its
    own position via a deterministic function of the value + position_idx.
    This simulates data "finding home" on the grid.
    """
    # Mix byte value with position index to find a stable grid position
    # This simulates the self-constrain / "วิ่งกลับบ้าน" behavior
    x = (byte_val * 37 + position_idx * 13) & 255
    y = (byte_val * 71 + position_idx * 7) & 255
    return (x, y)


# ── 3. "3-skip-1" pattern ──
# Hilbert walks 3 positions, skips 1.
# "เก็บ 1 ก็รู้ 3" — from 1 stored position you know 3.
# In practice: store data at positions 0,1,2 of each 4-group
# Position 3 (the skip) is derivable from the others IF data is correlated.
def group_data(data):
    """Group data into quads for 3-skip-1 pattern."""
    groups = []
    for i in range(0, len(data), 4):
        chunk = data[i:i+4]
        groups.append(chunk)
    return groups


# ── 4. Invert calculation ──
def compute_invert(stored_chunk, skip_position):
    """
    For positions 0,1,2 stored, compute the invert (prediction error)
    for position 3 (the skip).

    Prediction: pos3 = median(pos0, pos1, pos2) or linear interpolation.
    Invert = actual − predicted.
    """
    if len(stored_chunk) < 4:
        return None  # last partial chunk, no skip
    
    pos0, pos1, pos2, pos3_actual = stored_chunk[0], stored_chunk[1], stored_chunk[2], stored_chunk[3]
    
    # Prediction: if data has structure, median/linear works well
    # If data is random, prediction is useless and invert = full delta
    predicted = (pos0 + pos1 + pos2) // 3   # simple average prediction
    
    invert = (pos3_actual - predicted) & 0xFF
    return invert


def reconstruct_from_invert(stored_chunk, invert):
    """Reconstruct the skip position from stored + invert."""
    if len(stored_chunk) < 4 or invert is None:
        return stored_chunk[:]
    pos0, pos1, pos2 = stored_chunk[0], stored_chunk[1], stored_chunk[2]
    predicted = (pos0 + pos1 + pos2) // 3
    pos3 = (predicted + invert) & 0xFF
    return stored_chunk[:3] + [pos3]


# ── 5. Encode: frame0 + invert chain ──
def encode_frame0(data):
    """
    Encode data as frame0 + invert chain using 3-skip-1 pattern.
    
    frame0: positions 0,1,2 of each quad (the "active" positions)
    invert_chain: position 3 deviations (the "skip" positions)
    header: metadata
    """
    n = len(data)
    groups = group_data(list(data))
    
    # Store active positions (0,1,2 of each quad)
    frame0 = []
    invert_chain = []
    
    for g in groups:
        if len(g) >= 3:
            frame0.extend(g[:3])  # positions 0,1,2
            if len(g) == 4:
                inv = compute_invert(g, 3)
                if inv is not None:
                    invert_chain.append(inv)
        else:
            frame0.extend(g)  # partial last group
    
    # Pack frame0 + invert_chain into output
    header = {
        'original_size': n,
        'frame0_size': len(frame0),
        'invert_count': len(invert_chain),
        'active_per_quad': 3,
        'skip_per_quad': 1,
    }
    
    return header, bytes(frame0), bytes(invert_chain)


def decode_frame0(header, frame0_bytes, invert_bytes):
    """Reconstruct original data from frame0 + invert chain."""
    frame0 = list(frame0_bytes)
    inverts = list(invert_bytes)
    
    result = []
    inv_idx = 0
    for i in range(0, len(frame0), 3):
        chunk = frame0[i:i+3]
        if len(chunk) < 3:
            result.extend(chunk)
            break
        if inv_idx < len(inverts):
            inv = inverts[inv_idx]
            inv_idx += 1
            # reconstruct quad from 3 stored + 1 invert
            quad = reconstruct_from_invert(chunk + [0], inv)
            result.extend(quad)
        else:
            result.extend(chunk)
    
    # Truncate to original size
    return bytes(result[:header['original_size']])


# ── 6. Compression ratio test ──
def test_compression(name, data):
    h, f0, inv = encode_frame0(data)
    raw_size = len(data)
    encoded_size = len(f0) + len(inv) + 32  # +32 for header overhead
    
    ratio = raw_size / encoded_size if encoded_size > 0 else 0
    
    print(f"[{name}]")
    print(f"  Original: {raw_size} bytes")
    print(f"  Frame0:   {len(f0)} bytes")
    print(f"  Inverts:  {len(inv)} bytes")
    print(f"  Total:    {encoded_size} bytes")
    print(f"  Ratio:    {ratio:.4f}x {'👍' if ratio > 1.0 else '👎'}")
    
    # Verify lossless
    decoded = decode_frame0(h, f0, inv)
    if decoded == data:
        print(f"  Lossless: ✅")
    else:
        # Count differences
        diffs = sum(1 for a, b in zip(decoded, data) if a != b)
        print(f"  Lossless: ❌ ({diffs} bytes differ)")
    
    print()
    return ratio


# ── Generate test data ──
def main():
    print("=" * 60)
    print("Geometric Capture Prototype — 3-skip-1 Invert Storage")
    print("=" * 60)
    print()
    
    # Test 1: All zeros (extreme structure)
    data_zeros = b'\x00' * 10000
    test_compression("ALL ZEROS", data_zeros)
    
    # Test 2: Sequential (smooth pattern)
    data_seq = bytes(range(256)) * 40  # 10240 bytes
    test_compression("SEQUENTIAL 0..255", data_seq)
    
    # Test 3: Sine wave (structured, predictable)
    data_sine = bytes(int(128 + 127 * math.sin(i * 0.1)) & 0xFF for i in range(10000))
    test_compression("SINE WAVE", data_sine)
    
    # Test 4: Text (English) — has structure
    text_sample = ("The quick brown fox jumps over the lazy dog. " * 200).encode()
    test_compression("ENGLISH TEXT", text_sample[:10000])
    
    # Test 5: Random (high entropy)
    data_random = os.urandom(10000)
    test_compression("RANDOM DATA", data_random)
    
    # Test 6: PNG image bytes (already compressed)
    data_png = bytes((i * 37 + 13) & 0xFF for i in range(10000))
    test_compression("SEMI-RANDOM (mul+add)", data_png)
    
    # Test 7: Compare with zstd/zlib compression
    print("=" * 60)
    print("Comparison: raw zlib on same data")
    print("=" * 60)
    for name, data in [
        ("ALL ZEROS", b'\x00' * 10000),
        ("SEQUENTIAL", bytes(range(256)) * 40),
        ("SINE WAVE", bytes(int(128 + 127 * math.sin(i * 0.1)) & 0xFF for i in range(10000))),
        ("ENGLISH TEXT", ("The quick brown fox jumps over the lazy dog. " * 200).encode()[:10000]),
        ("RANDOM", os.urandom(10000)),
    ]:
        zipped = zlib.compress(data)
        ratio = len(data) / len(zipped)
        print(f"  zlib [{name}]: {len(data)} → {len(zipped)} = {ratio:.4f}x{' 👍' if ratio > 1.0 else ' 👎'}")
    
    print()
    print("=" * 60)
    print("Analysis")
    print("=" * 60)
    print()
    print("3-skip-1 alone gives 4:3 expansion (store 3 out of 4 positions)")
    print("= 0.75x ratio BEFORE any compression")
    print()
    print("Invert = actual − prediction")
    print("If prediction is good → invert values are small → compress further")
    print("If data is random → invert values are full range → no further compression")
    print()
    print("For structured data:")
    print("  - Active positions (3/4) can be compressed via RLE/entropy")
    print("  - Inverts cluster near 0 → small values → fewer bits")
    print("For random data:")
    print("  - Every position is 'random' → no locality → no compression")
    print("  - Inverts uniformly distributed 0..255 → 1 byte each → no gain")


if __name__ == '__main__':
    main()
