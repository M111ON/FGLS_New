#!/usr/bin/env python3
"""
Prototype v2 — closer to actual concept:
- Grid = dodecahedral topology (simplified as toroidal 2D with face-wrapping)
- geo_frame_seek stride-37 deterministic walk
- Data self-constrains to grid position by VALUE (not by index)
- Frame0 = occupancy map + bond links only
- Reconstruct = walk + occupancy → data bytes
"""
import struct, math, os, sys, zlib
from collections import defaultdict

# ── Core constants (from geo_frame_seek.h) ──
FRAME_CYCLE = 1440
FRAME_STRIDE = 37
FRAME_FACE_SZ = 120    # slots per face
FRAME_EDGES = 12       # 9H + 3P
DODECA_FACES = 12
TOTAL_SLOTS = FRAME_CYCLE  # 1440 = 12 × 120

# ── 1. Deterministic timeline walker ──
class Timeline:
    def seek(self, enc):
        enc = enc % FRAME_CYCLE
        face = enc // FRAME_FACE_SZ            # 0..11
        slot_in_face = enc % FRAME_FACE_SZ     # 0..119
        group = (enc // 3) % 3                 # 0..2 Hilbert group
        edge = enc % 3                         # 0..2
        is_skip = (enc % 4 == 3)               # every 4th = invert
        return {
            'enc': enc, 'face': face, 'slot': slot_in_face,
            'group': group, 'edge': edge, 'is_skip': is_skip
        }

    def next_enc(self, enc):
        return (enc + FRAME_STRIDE) % FRAME_CYCLE

    def walk(self, start=0, n=FRAME_CYCLE):
        frames = []
        e = start
        for _ in range(n):
            frames.append(self.seek(e))
            e = self.next_enc(e)
        return frames

# ── 2. Grid: dodecahedral wrap ──
# Each face = 120 slots. Slots wrap across faces as adjacency.
class Grid:
    """Simplified: 2D grid where face=row, slot=col, with wrap-around."""
    def __init__(self, faces=12, slots_per_face=120):
        self.faces = faces       # row count
        self.slots = slots_per_face  # col count

    def cell_for(self, face, slot):
        return (face % self.faces, slot % self.slots)

    def neighbors(self, face, slot):
        """6 neighbors (hex-like topology)."""
        adjacency = [
            ((face, slot - 1), (face, slot + 1),           # same face
             ((face - 1) % self.faces, slot),               # prev face
             ((face + 1) % self.faces, slot),               # next face
             ((face - 1) % self.faces, (slot + 1) % self.slots),
             ((face + 1) % self.faces, (slot - 1) % self.slots))
        ]
        return adjacency

# ── 3. Capture: data → grid position (self-constrain) ──
def data_to_position(byte_val, iteration=0, faces=12, slots_per_face=120):
    """
    Data "finds home" on the grid based on its value.
    The byte's value AND its position in data determine grid location.
    This is the self-constrain mechanism — like solving a maze.
    """
    # Mix byte value with its index to find a unique position
    # This simulates the "data runs through the maze and settles"
    total = faces * slots_per_face  # 1440
    
    # The self-constrain: byte determines its own position
    # via a deterministic function
    pos = (byte_val * 37 + iteration * 13 + (byte_val >> 4) * 7) % total
    
    face = pos // slots_per_face
    slot = pos % slots_per_face
    return face, slot

# ── 4. Frame0 encoder ──
def encode_frame0(data):
    """
    Encode data as: occupancy map (which grid cells have data?)
    + bond chain (how neighbouring cells link)
    
    Frame0 = occupancy bitmap + occupied values at those positions.
    """
    n = len(data)
    tl = Timeline()
    grid = Grid()
    
    # Data self-constrains to grid positions
    # Build occupancy map: which (face,slot) has data
    occupancy = {}   # (face,slot) → byte_val
    bond_chain = []  # list of (from, to) connections
    
    prev_pos = None
    for i, byte in enumerate(data):
        face, slot = data_to_position(byte, i)
        occupancy[(face, slot)] = byte
        
        if prev_pos is not None:
            bond_chain.append((prev_pos, (face, slot)))
        prev_pos = (face, slot)
    
    # Encode occupancy as sparse bitmap
    # Occupied cells = {(face,slot): value}
    # Empty cells = implicitly "no data here"
    occ_list = sorted(occupancy.items(), key=lambda kv: kv[0][0] * grid.slots + kv[0][1])
    
    # Pack occupancy
    header = {
        'original_size': n,
        'occupied': len(occ_list),
        'total_cells': grid.faces * grid.slots,
    }
    
    # Frame0 = run-length encoded occupancy map + values
    # If data clusters → RLE compresses well
    # If data is random → RLE compresses poorly
    frame0_bytes = bytearray()
    prev_face, prev_slot = -1, -1
    for (face, slot), val in occ_list:
        if prev_face == -1:
            # First entry: store absolute
            frame0_bytes.extend(struct.pack('>HH', face, slot))
        else:
            # Delta from previous position
            df = (face - prev_face) % 256
            ds = (slot - prev_slot) % 256
            frame0_bytes.extend(struct.pack('>BB', df, ds))
        frame0_bytes.append(val)
        prev_face, prev_slot = face, slot
    
    # Bind chain = just the order of occupancy entries
    bond_bytes = struct.pack('>I', len(bond_chain))
    
    return header, bytes(frame0_bytes), bytes(bond_bytes), occupancy


def decode_frame0(header, frame0_bytes):
    """Reconstruct data from frame0 occupancy map."""
    n = header['original_size']
    # Read occupancy entries
    occ = {}
    pos = 0
    prev_face, prev_slot = -1, -1
    first = True
    while pos < len(frame0_bytes):
        if first:
            face = struct.unpack('>H', frame0_bytes[pos:pos+2])[0]
            slot = struct.unpack('>H', frame0_bytes[pos+2:pos+4])[0]
            pos += 4
            first = False
        else:
            df = frame0_bytes[pos]
            ds = frame0_bytes[pos + 1]
            face = (prev_face + df) % 0xFFFF
            slot = (prev_slot + ds) % 0xFFFF
            pos += 2
        val = frame0_bytes[pos]
        occ[(face, slot)] = val
        prev_face, prev_slot = face, slot
        pos += 1
    
    # Reconstruct data by re-running the self-constrain
    # For each position, find which data byte matches
    data = bytearray()
    tl = Timeline()
    for i in range(n):
        byte = None
        for (face, slot), val in occ.items():
            f2, s2 = data_to_position(val, i, 12, 120)
            if f2 == face and s2 == slot:
                byte = val
                break
        if byte is not None:
            data.append(byte)
        else:
            data.append(0)
    
    return bytes(data)


# ── 5. Tests ──
def run_test(name, data):
    print(f"\n{'='*60}")
    print(f"  TEST: {name}")
    print(f"{'='*60}")
    print(f"  Original: {len(data)} bytes")
    
    h, f0, bond, occ = encode_frame0(data)
    encoded_size = len(f0) + len(bond) + 64  # header overhead
    
    # Also run zlib on original for comparison
    zipped = zlib.compress(data)
    zlib_ratio = len(data) / len(zipped) if len(zipped) > 0 else 0
    
    # Grid statistics
    total_cells = 12 * 120
    occupied = len(occ)
    density = occupied / total_cells * 100
    
    # Cluster analysis: count occupied cells within 1 step of another
    clustered = 0
    occ_set = set(occ.keys())
    for face, slot in occ_set:
        for df in [-1, 0, 1]:
            for ds in [-1, 0, 1]:
                if df == 0 and ds == 0:
                    continue
                neighbor = ((face + df) % 12, (slot + ds) % 120)
                if neighbor in occ_set:
                    clustered += 1
                    break
    
    cluster_pct = clustered / occupied * 100 if occupied > 0 else 0
    
    # Compress frame0 with zlib (simulating "per-route codec")
    f0_compressed = zlib.compress(f0, 9)
    f0_ratio = len(f0) / len(f0_compressed) if len(f0_compressed) > 0 else 0
    
    total_with_codec = len(f0_compressed) + len(bond) + 64
    total_ratio = len(data) / total_with_codec if total_with_codec > 0 else 0
    
    print(f"  Grid:       12 faces × 120 slots = {total_cells} cells")
    print(f"  Occupied:   {occupied} cells ({density:.2f}%)")
    print(f"  Clustered:  {cluster_pct:.1f}%")
    print(f"  Frame0:     {len(f0)} bytes")
    print(f"  Bond:       {len(bond)} bytes")
    print(f"  Raw encoded:{encoded_size} bytes (ratio={len(data)/max(1,encoded_size):.4f}x)")
    print(f"  zlib frame0:{len(f0)}→{len(f0_compressed)} ({f0_ratio:.2f}x)")
    print(f"  Total(compressed): {total_with_codec} bytes (ratio={total_ratio:.4f}x)")
    print(f"  zlib direct:{len(zipped)} bytes ({zlib_ratio:.4f}x)")
    
    # Verify
    decoded = decode_frame0(h, f0)
    if decoded == data:
        print(f"  Lossless: ✅")
    else:
        diffs = sum(1 for a, b in zip(decoded, data) if a != b)
        print(f"  Lossless: ❌ ({diffs}/{len(data)} diff)")
    
    return total_ratio


def main():
    print("=" * 60)
    print("  Prototype v2 — Dodeca Grid + Self-Constrain + Frame0")
    print("=" * 60)
    
    # Test 1: Zeros
    run_test("ALL ZEROS", b'\x00' * 5000)
    
    # Test 2: Sequential
    run_test("SEQUENTIAL", bytes(range(256)) * 20)  # 5120
    
    # Test 3: Text
    text = ("The quick brown fox jumps over the lazy dog. " * 120).encode()[:5000]
    run_test("ENGLISH TEXT", text)
    
    # Test 4: Sine wave
    sine = bytes(int(128 + 127 * math.sin(i * 0.05)) & 0xFF for i in range(5000))
    run_test("SINE WAVE", sine)
    
    # Test 5: Random
    rand = os.urandom(5000)
    run_test("RANDOM", rand)
    
    # Test 6: Mixed structure (text + random interleaved)
    mixed = bytearray()
    for i in range(250):
        mixed.extend(b'AAAA')
        mixed.extend(os.urandom(16))
    run_test("MIXED pattern+random", bytes(mixed))
    
    # Test 7: GGUF-like tensor weight pattern (small smooth values)
    weights = bytes(int(127 + 50 * math.sin(i * 0.3) * math.cos(i * 0.07)) & 0xFF for i in range(5000))
    run_test("TENSOR-LIKE WEIGHTS", weights)
    
    print()
    print("=" * 60)
    print("  Summary")
    print("=" * 60)
    print()
    print("  Key insight: compression comes from OCCUPANCY CLUSTERING")
    print("  The denser the occupancy → harder to compress")
    print("  Structured data stays local → sparse occupancy → compact frame0")
    print("  Random data spreads everywhere → dense occupancy → frame0 ≈ original")
    print()


if __name__ == '__main__':
    main()
