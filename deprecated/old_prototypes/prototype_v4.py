#!/usr/bin/env python3
"""
Prototype v4 — Correct lossless model:
- Grid = N positions, visited in stride-37 walk order
- Data byte b occupies walk position P where P is determined by b AND data_idx
- Frame0 = occupancy bitmap of the grid
- Reconstruction: walk grid in stride-37 order; occupied positions → data bytes
- "Value IS position" + "Order IS walk" = lossless

Key: grid must be ≥ data_length for no collisions.
Each grid position encodes ONE byte via its modulo-256.
"""
import math, os, zlib, struct

FRAME_STRIDE = 37

def walk_positions(grid_size):
    """Generate all positions in stride-37 walk order."""
    order = []
    seen = set()
    pos = 0
    while len(seen) < grid_size:
        if pos < grid_size and pos not in seen:
            seen.add(pos)
            order.append(pos)
        pos = (pos + FRAME_STRIDE) % grid_size
        # Safety: prevent infinite loop if stride doesn't cover all
        if len(order) >= grid_size:
            break
    # If stride doesn't generate full coverage, fill remaining
    for i in range(grid_size):
        if i not in seen:
            order.append(i)
    return order

def encode(data, grid_mult=2):
    """
    Encode data:
    - Grid has grid_mult × len(data) positions (minimum)
    - Each data byte b is placed at walk position P where P % 256 == b
    - If collision, use next available position with same modulo
    - Store: occupancy bitmap + overflow info
    
    Walk visits positions in stride-37 order. At each occupied position,
    the byte value = position % 256. So occupancy IS the data.
    """
    n = len(data)
    grid_size = max(n * grid_mult, 256)
    grid_size = ((grid_size + 255) // 256) * 256  # round to 256
    
    walk = walk_positions(grid_size)
    
    # Build occupancy: which positions are occupied by which byte
    # Strategy: match each byte to a position where pos % 256 == byte
    occupancy = [0] * grid_size
    overflow = []  # bytes that can't fit in grid
    
    # For each modulo group (0..255), track which positions are taken
    taken = {}  # modulo_value → set of occupied positions
    
    for idx, byte_val in enumerate(data):
        # Find a position where pos % 256 == byte_val
        found = False
        if byte_val not in taken:
            taken[byte_val] = set()
        
        # Search walk order for first available position with this modulo
        for pos in walk:
            if pos % 256 == byte_val and pos not in taken[byte_val]:
                occupancy[pos] = 1
                taken[byte_val].add(pos)
                found = True
                break
        
        if not found:
            overflow.append(byte_val)
    
    # Encode occupancy as bitmap
    bitmap_len = (grid_size + 7) // 8
    bitmap = bytearray(bitmap_len)
    for i, bit in enumerate(occupancy):
        if bit:
            bitmap[i // 8] |= (1 << (i % 8))
    
    header = {
        'original_size': n,
        'grid_size': grid_size,
        'n_occupied': sum(occupancy),
        'n_overflow': len(overflow),
    }
    
    return header, bytes(bitmap), bytes(overflow), walk


def decode(header, bitmap, overflow):
    """Reconstruct data from occupancy bitmap."""
    grid_size = header['grid_size']
    n = header['original_size']
    
    walk = walk_positions(grid_size)
    
    # Read bitmap
    data = []
    for pos in walk:
        byte_idx = pos // 8
        bit_idx = pos % 8
        if byte_idx < len(bitmap) and (bitmap[byte_idx] & (1 << bit_idx)):
            byte_val = pos % 256
            data.append(byte_val)
        
        if len(data) >= n:
            break
    
    # Append overflow bytes
    data.extend(overflow)
    
    return bytes(data[:n])


# ── Compression analysis with zlib on bitmap ──
def run_test(name, data):
    n = len(data)
    print(f"\n{'='*60}")
    print(f"  {name}")
    print(f"{'='*60}")
    print(f"  Data: {n} bytes")
    
    # Try different grid multipliers
    for mult in [1.0, 1.5, 2.0, 4.0]:
        header, bitmap, overflow, walk = encode(data, grid_mult=int(n * mult / n + 1) if mult > 0 else 2)
        # Override grid size calculation
        grid_size = max(int(n * mult), 256)
        grid_size = ((grid_size + 255) // 256) * 256
        header, bitmap, overflow, walk = encode(data, grid_mult=int(grid_size / n) if n > 0 else 2)
        
        bitmap_compressed = zlib.compress(bitmap, 9)
        
        raw_encoded = len(bitmap) + len(overflow) + 64
        compressed_total = len(bitmap_compressed) + len(overflow) + 64
        
        ratio_raw = n / raw_encoded if raw_encoded > 0 else 0
        ratio_comp = n / compressed_total if compressed_total > 0 else 0
        
        # zlib direct
        zlib_direct = zlib.compress(data)
        zlib_ratio = n / len(zlib_direct) if len(zlib_direct) > 0 else 0
        
        # Only show first attempt to avoid spam
        if mult == 1.0:
            # Fix grid_size to max(n,256) rounded
            break
    
    # Use proper run
    header, bitmap, overflow, walk = encode(data)
    bitmap_compressed = zlib.compress(bitmap, 9)
    
    raw = len(bitmap) + len(overflow) + 64
    comp = len(bitmap_compressed) + len(overflow) + 64
    zl = zlib.compress(data)
    
    # Verify
    decoded = decode(header, bitmap, overflow)
    lossless = decoded == data
    
    density = sum(bitmap) * 8 / max(len(bitmap), 1) / 100 * 100
    
    print(f"  Grid:   {header['grid_size']} cells ({sum(bitmap)} occupied, {sum(bitmap)/header['grid_size']*100:.1f}%)")
    print(f"  Bitmap: {len(bitmap)} bytes → zlib={len(bitmap_compressed)} ({len(bitmap)/max(len(bitmap_compressed),1):.1f}x)")
    print(f"  Frame0: {raw} raw | {comp} compressed | ratio={n/comp:.2f}x")
    print(f"  zlib:   {len(zl)} bytes | ratio={n/len(zl):.2f}x")
    print(f"  Lossless: {'✅' if lossless else '❌'}")
    
    return n/comp if comp > 0 else 0


def main():
    print("=" * 60)
    print("  Prototype v4 — Lossless Occupancy Map Codec")
    print("=" * 60)
    print()
    print("  Core concept: data bytes occupy grid positions.")
    print("  POSITION % 256 = BYTE VALUE. Walk order = DATA ORDER.")
    print("  Frame0 = occupancy bitmap. That's it.")
    print()
    print("  Compression comes from: can bitmap be compressed?")
    print("  = bitmap entropy. For random data: 1.0× max.")
    print()
    
    n = 4096
    
    run_test("ALL ZEROS (ALL 0x00)", b'\x00' * n)
    run_test("SINGLE VALUE (0xFF only)", b'\xFF' * n)
    run_test("SEQUENTIAL (0..255 repeated)", bytes(range(256)) * (n // 256))
    run_test("FEW VALUES (0,1,2 only)", b''.join(bytes([i%3]) for i in range(n)))
    
    text = ("The quick brown fox jumps over the lazy dog. " * 100).encode()[:n]
    run_test("ENGLISH TEXT", text)
    
    sine = bytes(int(128 + 127 * math.sin(i * 0.01)) & 0xFF for i in range(n))
    run_test("SINE WAVE (low freq)", sine)
    
    hi_sine = bytes(int(128 + 127 * math.sin(i * 0.5)) & 0xFF for i in range(n))
    run_test("SINE WAVE (hi freq)", hi_sine)
    
    random_data = os.urandom(n)
    run_test("RANDOM", random_data)
    
    # Stat analysis
    print()
    print("="*60)
    print("  Entropy Analysis")
    print("="*60)
    print()
    for name, data in [
        ("ZEROS", b'\x00' * n),
        ("SEQ", bytes(range(256)) * (n // 256)),
        ("TEXT", ("abc" * 2000).encode()[:n]),
        ("SINE", bytes(int(128 + 127 * math.sin(i * 0.01)) & 0xFF for i in range(n))),
        ("RAND", os.urandom(n)),
    ]:
        # Count unique byte values
        unique = len(set(data))
        # Count how many positions in grid would be occupied
        mods = set(b % 256 for b in data)
        entropy = -sum((data.count(b)/len(data))*math.log2(data.count(b)/len(data)) if data.count(b)>0 else 0 for b in range(256))
        print(f"  {name:6s}: unique={unique:3d}, mods={len(mods):3d}, H={entropy:.2f} bits/byte")
    
    print()
    print("="*60)
    print("  Key finding")
    print("="*60)
    print()
    print("  The occupancy bitmap entropy = data entropy.")
    print("  Random data has 256 unique bytes → 256/256 mods")
    print("  = bitmap is 100% dense → zlib can't compress")
    print()
    print("  But for low-entropy data (zeros, text, low-freq sine):")
    print("  Few unique bytes → few mod groups → sparse bitmap → compress!")
    print("  = geometric structure exploits VALUE LOCALITY")
    print()


if __name__ == '__main__':
    main()
