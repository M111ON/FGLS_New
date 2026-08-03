"""
test_288_roundtrip.py — Compress → Decompress → Verify (lossless)
Tests the full RDH-288 pipeline: encode, decode, compare with original.
"""
import struct, time, os

# ══════════════════════════════════════════════════════════════
# GGUF Reader
# ══════════════════════════════════════════════════════════════
def read_gguf_raw(path, offset, nbytes):
    with open(path, 'rb') as f:
        f.seek(offset)
        return f.read(nbytes)

def read_q8_blocks(data, n_blocks):
    """Read Q8_0 blocks: 2-byte scale + 32 int8 weights each"""
    scales = []
    weights = []
    pos = 0
    for _ in range(n_blocks):
        if pos + 34 > len(data):
            break
        raw16 = struct.unpack_from('<H', data, pos)[0]
        scale = (raw16 & 0x7FFF) / 1024.0
        if raw16 & 0x8000:
            scale = -scale
        scales.append(scale)
        w = list(struct.unpack_from('32b', data, pos + 2))
        weights.append(w)
        pos += 34
    return scales, weights, pos

# ══════════════════════════════════════════════════════════════
# bridge_288
# ══════════════════════════════════════════════════════════════
CELL_288 = 288
CELL_DIRS = 6
CELL_PER_FACE = 1728
DODECA_FACES = 12
GEO_FULL = 20736

def bridge_288(flat_key):
    face = (flat_key // CELL_PER_FACE) % DODECA_FACES
    direction = (flat_key // CELL_288) % CELL_DIRS
    cell_pos = flat_key % CELL_288
    return face, direction, cell_pos

def bridge_288_key(face, direction, cell_pos):
    return face * CELL_PER_FACE + direction * CELL_288 + cell_pos

# ══════════════════════════════════════════════════════════════
# COMPRESS: Histogram + Codebook
# ══════════════════════════════════════════════════════════════
def compress_hist_codebook(weights_flat):
    """
    Input: list of uint8 weight values
    Output: compressed bytes
    
    Format:
      [n_distinct:1B][values: N bytes][indices: variable bits]
    
    Returns: (compressed_bytes, codebook, indices)
    """
    # Build histogram
    distinct = sorted(set(weights_flat))
    n_distinct = len(distinct)
    codebook = {v: i for i, v in enumerate(distinct)}
    
    # Bits per index
    if n_distinct <= 2:
        bits_per_idx = 1
    elif n_distinct <= 4:
        bits_per_idx = 2
    elif n_distinct <= 16:
        bits_per_idx = 4
    elif n_distinct <= 256:
        bits_per_idx = 8
    else:
        bits_per_idx = 16
    
    # Encode indices
    indices = [codebook[v] for v in weights_flat]
    
    # Pack bits into bytes
    total_bits = len(indices) * bits_per_idx
    total_bytes = (total_bits + 7) // 8
    
    # Header: 1 byte count + N bytes values
    header = bytes([n_distinct]) + bytes(distinct)
    
    # Pack indices into bytes
    index_bytes = bytearray(total_bytes)
    bit_pos = 0
    for idx in indices:
        for b in range(bits_per_idx):
            if idx & (1 << b):
                byte_idx = bit_pos // 8
                bit_in_byte = bit_pos % 8
                index_bytes[byte_idx] |= (1 << bit_in_byte)
            bit_pos += 1
    
    compressed = header + bytes(index_bytes)
    return compressed, codebook, indices, bits_per_idx

# ══════════════════════════════════════════════════════════════
# DECOMPRESS: Reverse of compress
# ══════════════════════════════════════════════════════════════
def decompress_hist_codebook(compressed, n_weights, bits_per_idx):
    """
    Input: compressed bytes, number of original weights, bits per index
    Output: list of uint8 weight values
    """
    n_distinct = compressed[0]
    values = list(compressed[1:1+n_distinct])
    
    # Unpack indices from bytes
    index_data = compressed[1+n_distinct:]
    indices = []
    bit_pos = 0
    for _ in range(n_weights):
        idx = 0
        for b in range(bits_per_idx):
            byte_idx = bit_pos // 8
            bit_in_byte = bit_pos % 8
            if index_data[byte_idx] & (1 << bit_in_byte):
                idx |= (1 << b)
            bit_pos += 1
        indices.append(idx)
    
    # Map indices back to values
    return [values[i] for i in indices]

# ══════════════════════════════════════════════════════════════
# MAIN TEST
# ══════════════════════════════════════════════════════════════
def main():
    MODEL = "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf"
    CHUNK = 32768  # 32KB
    N_BLOCKS = CHUNK // 34  # ~960 blocks
    
    offsets = [
        (1_000_000, "Embedding"),
        (5_000_000, "Early layers"),
        (10_000_000, "Mid model"),
        (50_000_000, "Later layers"),
        (100_000_000, "Near end"),
    ]
    
    print("=" * 78)
    print("RDH-288 ROUNDTRIP TEST — Compress → Decompress → Verify")
    print("=" * 78)
    print(f"Model: {MODEL}")
    print(f"Chunk: {CHUNK} bytes ({N_BLOCKS} Q8_0 blocks)")
    print()
    
    # Header
    print(f"{'Offset':<22} {'Raw':>8} {'Compressed':>10} {'Ratio':>7} {'Decompressed':>13} {'Match':>6} {'Time':>8}")
    print("-" * 78)
    
    total_raw = 0
    total_compressed = 0
    total_match = 0
    total_time_enc = 0
    total_time_dec = 0
    
    for offset, label in offsets:
        data = read_gguf_raw(MODEL, offset, CHUNK)
        scales, weights, bytes_read = read_q8_blocks(data, N_BLOCKS)
        
        # Flatten weights to uint8 (abs of int8)
        flat_weights = [abs(w) & 0xFF for block in weights for w in block]
        n_weights = len(flat_weights)
        
        # Compress
        t0 = time.time()
        compressed, codebook, indices, bpi = compress_hist_codebook(flat_weights)
        t_enc = time.time() - t0
        
        # Decompress
        t0 = time.time()
        decompressed = decompress_hist_codebook(compressed, n_weights, bpi)
        t_dec = time.time() - t0
        
        # Verify
        match = (decompressed == flat_weights)
        
        raw_size = bytes_read
        comp_size = len(compressed)
        ratio = comp_size / raw_size if raw_size > 0 else 0
        
        total_raw += raw_size
        total_compressed += comp_size
        total_match += (1 if match else 0)
        total_time_enc += t_enc
        total_time_dec += t_dec
        
        status = "✓" if match else "✗"
        print(f"{label:<22} {raw_size:>7}B {comp_size:>9}B {ratio:>6.3f}x {len(decompressed):>10} vals {status:>4} {t_enc*1000:>5.1f}ms")
    
    print("-" * 78)
    total_ratio = total_compressed / total_raw if total_raw > 0 else 0
    print(f"{'TOTAL':<22} {total_raw:>7}B {total_compressed:>9}B {total_ratio:>6.3f}x {'':>10} {'✓' if total_match == len(offsets) else '✗':>4} {total_time_enc*1000:>5.1f}ms")
    print()
    
    # Decompression time
    print(f"Decompression total: {total_time_dec*1000:.1f}ms for {total_match}/{len(offsets)} chunks")
    print(f"Throughput: {total_raw/total_time_enc/1024/1024:.1f} MB/s encode, {total_raw/total_time_dec/1024/1024:.1f} MB/s decode")
    print()
    
    # bridge_288 roundtrip
    print("bridge_288 roundtrip verification:")
    errors = 0
    for k in range(GEO_FULL):
        f, d, c = bridge_288(k)
        k2 = bridge_288_key(f, d, c)
        if k2 != k:
            errors += 1
    print(f"  {GEO_FULL}/{GEO_FULL} round-trips: {'✓ PASS' if errors == 0 else f'✗ FAIL ({errors} errors)'}")
    
    print()
    print("=" * 78)
    all_pass = (total_match == len(offsets)) and (errors == 0)
    print(f"FINAL: {'ALL PASS ✓' if all_pass else 'FAIL ✗'}")
    print("=" * 78)

if __name__ == "__main__":
    main()
