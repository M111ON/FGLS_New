#!/usr/bin/env python3
"""
Prototype v3 — faithful to concept:
- Grid scales to fit data (bijection: 1 position ↔ 1 byte value)
- geo_frame_seek timeline determines walk order (stride-37)
- Frame0 = ONLY stored state; timeline auto-generates rest
- Data self-constrains: byte VALUE determines grid POSITION
- Reconstruction: walk grid in deterministic order, read positions
- If grid position = value: position addresses are value → implicit storage
"""
import math, os, sys, zlib, struct
from collections import defaultdict

# ── Constants ──
FRAME_CYCLE = 1440
FRAME_STRIDE = 37
FRAME_FACE_SZ = 120

# ── 1. Deterministic walk (geo_frame_seek) ──
def frame_at(enc):
    enc = enc % FRAME_CYCLE
    face = enc // FRAME_FACE_SZ
    slot = enc % FRAME_FACE_SZ
    group = (enc // 3) % 3
    edge = enc % 3
    is_skip = (enc % 4 == 3)
    phase = (enc // 12) % 12
    return {
        'enc': enc, 'face': face, 'slot': slot,
        'group': group, 'edge': edge,
        'is_skip': is_skip, 'phase': phase
    }

def walk_generator(start=0, n=FRAME_CYCLE):
    """Yield frames in stride-37 order (not sequential!)."""
    e = start
    for _ in range(n):
        yield frame_at(e)
        e = (e + FRAME_STRIDE) % FRAME_CYCLE

# ── 2. The core: BIJECTIVE value↔position mapping ──
# For each byte value 0..255, assign a set of DISTINCT grid positions.
# The "self-constrain" = byte finds its position; position decodes to byte.
#
# Key insight: the GRID defines the byte value at each cell.
# We DON'T store "byte 0xA3 at position P".
# We store: "OCCUPANCY MAP" — which positions are occupied.
# The POSITION itself encodes the byte value via the walk order.

def build_position_table(grid_size):
    """
    Build a bijection f: position_in_walk_order (0..grid_size-1) → byte_value (0..255).
    The walk follows stride-37 order. Position `i` in the walk maps to byte `i % 256`.
    
    So: occupying position P in the walk means: the byte is P % 256.
    The POSITION IS THE VALUE — no separate value storage needed!
    """
    # Walk order: 0 → 37 → 74 → ... (mod grid_size)
    # Position in walk = enc at that step
    # Let's use sequential position in the walk → byte value
    pass

# ── 3. Simpler approach: encode via stride-37 walk positions ──
#
# The walk visits grid positions in delta order (stride-37).
# Each position visited can encode up to 256 values (1 byte).
# 
# System: data byte b → occupy walk position P where P % 256 == b.
# Reconstruction: walk all positions; if position i is occupied → byte = i % 256.
#
# Only store: which walk positions are occupied (bitmap).
# The bitmap IS the data.

class GeoEncoder:
    """Encodes data as a sparse occupancy bitmap on a geometric walk."""
    
    def __init__(self, walk_length=FRAME_CYCLE):
        self.walk_length = walk_length
        self.stride = FRAME_STRIDE
        
    def encode(self, data):
        """Encode data → occupancy bitmap of walk positions."""
        n = len(data)
        
        # We need enough walk positions for all bytes.
        # Each byte occupies a position where position % 256 == byte_value.
        # If multiple bytes have the same value? They occupy different
        # positions with the same modulo-256. So we need extra positions.
        
        # Strategy: build occupancy list for stride-37 walk
        # Position i in the walk encodes byte (i % 256) if occupied.
        # Walk order is: enc = (i * stride) % walk_length
        
        # Assign positions
        walk_len = max(n * 2, self.walk_length)  # allow 2× room
        # Round up to multiple of 256 for clean modulo
        walk_len = ((walk_len + 255) // 256) * 256
        
        # Build occupancy: for each data byte at index data_idx,
        # assign it a walk position where pos % 256 == byte.
        # Use data_idx to pick which "slot" within that modulo group.
        occupancy = [0] * walk_len
        
        for idx, byte_val in enumerate(data):
            # Find a position where pos % 256 == byte_val
            # Use idx as offset to avoid collisions
            group = idx // 256  # which set of 256 positions
            pos_in_group = idx % 256
            # Base position for this byte
            base = byte_val  # first position with this byte's modulo
            # The actual position in the walk
            pos = (base + group * 256 + pos_in_group) % walk_len
            occupancy[pos] = 1
        
        # Build walk order
        walk_order = []
        walk_len_adjusted = ((walk_len + 255) // 256) * 256
        for i in range(walk_len_adjusted):
            enc = (i * self.stride) % walk_len_adjusted
            walk_order.append(enc)
        
        # Store: occupancy bitmap + walk parameters
        return occupancy, walk_len_adjusted, walk_order
    
    def decode(self, occupancy, walk_len):
        """Reconstruct data from occupancy bitmap."""
        data = []
        walk_order = []
        for i in range(walk_len):
            enc = (i * self.stride) % walk_len
            walk_order.append(enc)
        
        for pos in walk_order:
            if occupancy[pos]:
                byte_val = pos % 256
                data.append(byte_val)
        
        return bytes(data)
    
    def encode_compact(self, data):
        """Encode data and produce compact output."""
        occ, walk_len, _ = self.encode(data)
        
        # Encode occupancy map as bitmap
        n_bytes = (walk_len + 7) // 8
        bitmap = bytearray(n_bytes)
        for i, bit in enumerate(occ):
            if bit:
                bitmap[i // 8] |= (1 << (i % 8))
        
        # Compress bitmap with zlib (per-route codec)
        bitmap_compressed = zlib.compress(bytes(bitmap), 9)
        
        header = {
            'original_size': len(data),
            'walk_len': walk_len,
            'n_occupied': sum(occ),
            'bitmap_size': len(bitmap),
        }
        
        return header, bytes(bitmap_compressed)
    
    def decode_compact(self, header, bitmap_compressed):
        """Decode from compressed bitmap."""
        bitmap = zlib.decompress(bitmap_compressed)
        walk_len = header['walk_len']
        
        # Convert bitmap back to occupancy
        occupancy = [0] * walk_len
        for i in range(walk_len):
            if bitmap[i // 8] & (1 << (i % 8)):
                occupancy[i] = 1
        
        return self.decode(occupancy, walk_len)


# ── 4. More sophisticated: value-IS-position via lookup table ──
#
# Real system: the grid has 256^N positions (for N-byte values).
# A byte value directly indexes into this space.
# Grid position = byte value. No ambiguity.
# "Capture" = data lands at its own-value position.
# "Frame0" = which positions are populated.

class DirectEncoder:
    """
    The simplest form of the concept:
    - Grid = 256 × 256 × ... hyperspace
    - Byte value 0xA3 occupies position 0xA3 in the grid
    - Occupancy = bitmap of 256 bits per dimension
    - No value storage needed: position = value
    """
    
    def encode(self, data):
        # Group into blocks of 64 bytes for practical storage
        block_size = 64
        blocks = []
        
        for i in range(0, len(data), block_size):
            chunk = data[i:i+block_size]
            
            # For each byte, map to 1-of-256 position
            # Occupancy = 256-bit bitmap per position
            # 64 bytes × 256 bits = 2048 bytes per block
            # vs 64 bytes raw → 32× expansion!
            #
            # BUT: if we compress the bitmap (RLE, zstd), and data is sparse
            # in value space (only a few unique values per block)...
            
            bitmap = bytearray(256)
            for byte_val in chunk:
                bitmap[byte_val // 8] |= (1 << (byte_val % 8))
            
            blocks.append({
                'n': len(chunk),
                'bitmap': bitmap
            })
        
        header = {
            'original_size': len(data),
            'block_size': block_size,
            'n_blocks': len(blocks),
        }
        
        # Pack blocks: each = 32 bytes bitmap (256 bits)
        packed = bytearray()
        for blk in blocks:
            packed.extend(blk['bitmap'])
        
        return header, bytes(packed)
    
    def decode(self, header, packed):
        block_size = 64
        data = bytearray()
        
        pos = 0
        for blk_idx in range(header['n_blocks']):
            bitmap = packed[pos:pos+32]
            pos += 32
            
            byte_vals = []
            for b in range(256):
                if bitmap[b // 8] & (1 << (b % 8)):
                    byte_vals.append(b)
            
            n = min(block_size, header['original_size'] - len(data))
            for i in range(n):
                if i < len(byte_vals):
                    data.append(byte_vals[i])
                else:
                    data.append(0)
        
        return bytes(data)


# ── 5. Tests ──
def test_encoder(name, encoder, data):
    print(f"\n{'='*60}")
    print(f"  {name}")
    print(f"{'='*60}")
    print(f"  Original: {len(data)} bytes")
    
    enc = encoder()
    header, packed = enc.encode(data)
    decoded = enc.decode(header, packed)
    
    is_lossless = decoded == data
    raw_encoded = len(packed)
    ratio_raw = len(data) / raw_encoded if raw_encoded > 0 else 0
    
    # Try compressing the packed output
    compressed = zlib.compress(packed, 9)
    ratio_compressed = len(data) / len(compressed) if len(compressed) > 0 else 0
    
    # zlib direct on original
    zlib_direct = zlib.compress(data)
    zlib_ratio = len(data) / len(zlib_direct) if len(zlib_direct) > 0 else 0
    
    print(f"  Encoded:  {raw_encoded} bytes (ratio={ratio_raw:.4f}x)")
    print(f"  + zlib:   {len(compressed)} bytes (ratio={ratio_compressed:.4f}x)")
    print(f"  zlib dir: {len(zlib_direct)} bytes (ratio={zlib_ratio:.4f}x)")
    print(f"  Lossless: {'✅' if is_lossless else '❌'} {'' if is_lossless else f'({sum(1 for a,b in zip(decoded,data) if a!=b)} diffs)'}")
    return ratio_raw, ratio_compressed


def main():
    print("=" * 60)
    print("  Prototype v3 — Direct Value↔Position Bijection")
    print("=" * 60)
    print()
    print("  The core idea: POSITION = VALUE, not 'position stores value'.")
    print("  A byte 0xA3 simply occupies cell 0xA3 in the grid.")
    print("  Occupancy bitmap IS the only storage.")
    print()
    print("  Compression = can we encode the occupancy bitmap")
    print("  more compactly than the raw data?")
    print()
    
    data_zeros = b'\x00' * 4096
    data_seq = bytes(range(256)) * 16  # 4096
    text = ("The quick brown fox jumps over the lazy dog. " * 80).encode()[:4096]
    sine = bytes(int(128 + 127 * math.sin(i * 0.01)) & 0xFF for i in range(4096))
    random_data = os.urandom(4096)
    
    for name, data in [
        ("ALL ZEROS", data_zeros),
        ("SEQUENTIAL", data_seq),
        ("ENGLISH TEXT", text),
        ("SINE WAVE", sine),
        ("RANDOM", random_data),
    ]:
        test_encoder(name, DirectEncoder, data)
    
    print()
    print("=" * 60)
    print("  Analysis")
    print("=" * 60)
    print()
    print("  Direct value↔position bitmap encoding:")
    print("  • 64-byte block → 32-byte bitmap (256-bit occupancy)")
    print("  • That's 32/64 = 0.5× BEFORE any compression")
    print("  • Each unique byte value in block sets 1 bit in bitmap")
    print("  • For structured data: few unique values → sparse bitmap → compress!")
    print("  • For random data: many unique values → dense bitmap → no gain")
    print()
    print("  This is the ENTROPY barrier: you can't losslessly encode")
    print("  >256 unique values in 256 bits. That's the math.")
    print()


if __name__ == '__main__':
    main()
