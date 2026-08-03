"""
REAL GGUF Compression Test — actual byte counts on actual data
=============================================================
Reads real Q8_0 weight data from SmolLM2-360M-Instruct.Q8_0.gguf,
applies bridge_288 mapping, and measures ACTUAL compressed output size.

Methods tested:
  1. Histogram+Codebook (v2 winner) — variable-length indices
  2. 288-cell bitmap per block — which cells are active
  3. Raw baseline for comparison
"""
import struct, os, sys, math
from collections import Counter
from datetime import datetime

GGUF_PATH = "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf"
CHUNK_SIZE = 32768  # 32KB

# ── GGUF parsing (streaming from file handle) ──
GGUF_MAGIC = 0x46554747  # 'GGUF'
GGML_TYPE_Q8_0 = 8
GGML_Q8_0_BLOCK_SIZE = 34  # 2 scale + 32 weights
GGML_Q8_0_WEIGHTS_PER_BLOCK = 32

class GGUFReader:
    """Streaming GGUF parser — reads from file handle, no bulk buffer"""
    def __init__(self, path):
        self.fp = open(path, 'rb')
        self.tensors = []

    def _read_u8(self):
        return struct.unpack('B', self.fp.read(1))[0]

    def _read_u16(self):
        return struct.unpack('<H', self.fp.read(2))[0]

    def _read_u32(self):
        return struct.unpack('<I', self.fp.read(4))[0]

    def _read_i32(self):
        return struct.unpack('<i', self.fp.read(4))[0]

    def _read_u64(self):
        return struct.unpack('<Q', self.fp.read(8))[0]

    def _read_str(self):
        length = self._read_u64()
        data = self.fp.read(length)
        return data.decode('utf-8', errors='replace')

    def _skip_value(self, vtype):
        """Skip a GGUF metadata value"""
        if vtype == 0: self._read_u8()        # uint8
        elif vtype == 1: self.fp.read(1)      # int8
        elif vtype == 2: self.fp.read(2)      # uint16
        elif vtype == 3: self.fp.read(2)      # int16
        elif vtype == 4: self.fp.read(4)      # uint32
        elif vtype == 5: self.fp.read(4)      # int32
        elif vtype == 6: self.fp.read(4)      # float32
        elif vtype == 7: self.fp.read(1)      # bool
        elif vtype == 8: self._read_str()     # string
        elif vtype == 9:                      # array
            arr_type = self._read_u32()
            arr_len = self._read_u64()
            for _ in range(arr_len):
                self._skip_value(arr_type)
        elif vtype == 10: self.fp.read(8)     # uint64
        elif vtype == 11: self.fp.read(8)     # int64
        elif vtype == 12: self.fp.read(8)     # float64
        else:
            raise ValueError(f"Unknown GGUF value type: {vtype}")

    def parse(self):
        """Parse full GGUF header and tensor info"""
        magic = self._read_u32()
        assert magic == GGUF_MAGIC, f"Bad GGUF magic: 0x{magic:08X}"

        version = self._read_u32()
        tensor_count = self._read_u64()
        kv_count = self._read_u64()

        print(f"  GGUF v{version}: {tensor_count} tensors, {kv_count} metadata keys")

        # Skip KV metadata
        for i in range(kv_count):
            key = self._read_str()
            vtype = self._read_u32()
            self._skip_value(vtype)

        # Read tensor info
        for i in range(tensor_count):
            name = self._read_str()
            n_dims = self._read_u32()
            dims = [self._read_u64() for _ in range(n_dims)]
            dtype = self._read_u32()
            t_offset = self._read_u64()

            n_weights = 1
            for d in dims:
                n_weights *= d

            if dtype == GGML_TYPE_Q8_0:
                n_blocks = (n_weights + GGML_Q8_0_WEIGHTS_PER_BLOCK - 1) // GGML_Q8_0_WEIGHTS_PER_BLOCK
                size_bytes = n_blocks * GGML_Q8_0_BLOCK_SIZE
            else:
                size_bytes = n_weights

            self.tensors.append({
                'name': name, 'dims': dims, 'type': dtype,
                'offset': t_offset, 'size_bytes': size_bytes,
                'n_weights': n_weights
            })

        # tensor_data_start = current file position
        self.tensor_data_start = self.fp.tell()
        return self.tensor_data_start

    def close(self):
        self.fp.close()

# ── bridge_288 mapping ──
def bridge_288(value):
    """Map uint8 (0-255) → (face, dir, cell) via flat_key"""
    flat_key = (value * 81) % 20736
    face = flat_key // 1728       # 0-11
    direction = (flat_key // 288) % 6  # 0-5
    cell = flat_key % 288         # 0-287
    return face, direction, cell

# ── Compression method 1: Histogram + Codebook ──
def compress_histogram_codebook(weights):
    """
    Histogram+Codebook compression.
    - Build codebook: distinct value → index
    - Store codebook header: [n_distinct:1B][values:N bytes]
    - Store encoded indices: variable-length (ceil(log2(n_distinct)) bits each)
    Returns: compressed bytes, stats
    """
    distinct = sorted(set(weights))
    n_distinct = len(distinct)

    # Build reverse map
    codebook = {v: i for i, v in enumerate(distinct)}

    # Bits per index
    bits_per_idx = max(1, math.ceil(math.log2(n_distinct))) if n_distinct > 1 else 1

    # Codebook header: 1 byte count + N bytes values
    header_bytes = 1 + n_distinct

    # Encoded indices: ceil(n_weights * bits_per_idx / 8)
    n_weights = len(weights)
    total_index_bits = n_weights * bits_per_idx
    index_bytes = (total_index_bits + 7) // 8

    compressed_size = header_bytes + index_bytes

    # Build actual compressed bytes for verification
    out = bytearray()
    out.append(n_distinct)
    for v in distinct:
        out.append(v)

    # Pack indices as bits
    bit_buffer = 0
    bits_in_buffer = 0
    for w in weights:
        idx = codebook[w]
        bit_buffer = (bit_buffer << bits_per_idx) | idx
        bits_in_buffer += bits_per_idx
        while bits_in_buffer >= 8:
            bits_in_buffer -= 8
            out.append((bit_buffer >> bits_in_buffer) & 0xFF)

    # Flush remaining bits
    if bits_in_buffer > 0:
        out.append((bit_buffer << (8 - bits_in_buffer)) & 0xFF)

    return bytes(out), n_distinct, bits_per_idx, header_bytes, index_bytes

# ── Compression method 2: 288-cell bitmap per block ──
def compress_288_bitmap(blocks):
    """
    288-cell bitmap compression.
    For each Q8_0 block:
    - Map 32 weights via bridge_288 → 32 (face, dir, cell) tuples
    - Build 288-bit bitmap (36 bytes): which cells are active
    - Store active cell values
    Returns: compressed bytes, total active cells
    """
    out = bytearray()
    total_active_cells = 0

    for block in blocks:
        weights = list(block[2:34])

        # Map to 288-cell space
        active_cells = set()
        cell_values = {}
        for w in weights:
            face, direction, cell = bridge_288(w)
            flat_cell = face * 24 + direction * 4 + (cell // 72)
            flat_cell = flat_cell % 288
            active_cells.add(flat_cell)
            if flat_cell not in cell_values:
                cell_values[flat_cell] = w

        # 288-bit bitmap = 36 bytes
        bitmap = bytearray(36)
        sorted_cells = sorted(active_cells)
        for c in sorted_cells:
            byte_idx = c // 8
            bit_idx = c % 8
            bitmap[byte_idx] |= (1 << bit_idx)

        # Active values
        active_values = bytearray()
        for c in sorted_cells:
            active_values.append(cell_values[c])

        # Total per block: 36 (bitmap) + 1 (n_active) + N (values)
        out.extend(bitmap)
        out.append(len(sorted_cells))
        out.extend(active_values)

        total_active_cells += len(sorted_cells)

    return bytes(out), total_active_cells

# ── Compression method 3: Histogram+Codebook on 288 cell IDs ──
def compress_288_histogram_per_block(blocks):
    """
    Per-block: map weights → 288 cells, then histogram compress
    the active cell list.
    """
    out = bytearray()
    total_active = 0

    for block in blocks:
        weights = list(block[2:34])

        # Map to cells
        cells = []
        for w in weights:
            face, direction, cell = bridge_288(w)
            flat_cell = face * 24 + direction * 4 + (cell // 72)
            flat_cell = flat_cell % 288
            cells.append(flat_cell)

        distinct_cells = sorted(set(cells))
        n_distinct = len(distinct_cells)
        total_active += n_distinct

        # Codebook
        cell_codebook = {c: i for i, c in enumerate(distinct_cells)}

        # Header: [n_distinct:1B][cell_ids: 2B each]
        header = bytearray()
        header.append(n_distinct)
        for c in distinct_cells:
            header.extend(struct.pack('<H', c))

        # Encoded indices
        bits_per_idx = max(1, math.ceil(math.log2(n_distinct))) if n_distinct > 1 else 1
        n_idx_bytes = (32 * bits_per_idx + 7) // 8

        block_total = len(header) + n_idx_bytes
        out.extend(header)

        # Pack indices
        bit_buf = 0
        bits_in = 0
        for c in cells:
            idx = cell_codebook[c]
            bit_buf = (bit_buf << bits_per_idx) | idx
            bits_in += bits_per_idx
            while bits_in >= 8:
                bits_in -= 8
                out.append((bit_buf >> bits_in) & 0xFF)
        if bits_in > 0:
            out.append((bit_buf << (8 - bits_in)) & 0xFF)

    return bytes(out), total_active

# ── Main ──
print("=" * 80)
print("REAL GGUF Compression Test — Actual Byte Counts")
print(f"File: {GGUF_PATH}")
print(f"Date: {datetime.now().isoformat()}")
print("=" * 80)

# Parse GGUF header
print("\n[1] Parsing GGUF header...")
reader = GGUFReader(GGUF_PATH)
tensor_data_start = reader.parse()
tensors = reader.tensors
print(f"  Tensor data starts at offset: {tensor_data_start} bytes")

# Show first few tensors
print(f"\n  First 5 tensors:")
for i, t in enumerate(tensors[:5]):
    print(f"    [{i}] {t['name']}: dims={t['dims']}, type={t['type']}, "
          f"offset={t['offset']}, size={t['size_bytes']} bytes")

# Find first Q8_0 tensor
first_q8 = None
for t in tensors:
    if t['type'] == GGML_TYPE_Q8_0:
        first_q8 = t
        break

if first_q8:
    print(f"\n  First Q8_0 tensor: {first_q8['name']}")
    print(f"    dims={first_q8['dims']}, offset={first_q8['offset']}, "
          f"size={first_q8['size_bytes']} bytes ({first_q8['size_bytes']/1024/1024:.2f} MB)")
    data_offset = first_q8['offset']
    print(f"    File data offset: {data_offset} bytes ({data_offset/1024/1024:.2f} MB)")
else:
    print("\n  WARNING: No Q8_0 tensor found, using raw offsets")
    data_offset = tensor_data_start

# ── Test offsets ──
test_offsets = [
    ("Embedding (1MB)", 1 * 1024 * 1024),
    ("Early layers (5MB)", 5 * 1024 * 1024),
    ("Mid model (10MB)", 10 * 1024 * 1024),
    ("Later layers (50MB)", 50 * 1024 * 1024),
    ("Near end (100MB)", 100 * 1024 * 1024),
    ("Deep layers (150MB)", 150 * 1024 * 1024),
    ("Final layers (200MB)", 200 * 1024 * 1024),
]

print("\n[2] Running compression tests on real data...")
print("-" * 80)
print(f"{'Offset':<22} {'Raw':>8} {'Hist+CB':>10} {'Ratio':>6} {'Bitmap':>10} {'Ratio':>6} {'Distinct':>9} {'Bits/idx':>8}")
print("-" * 80)

results = []

with open(GGUF_PATH, 'rb') as f:
    for label, offset in test_offsets:
        f.seek(offset, 0)
        raw_data = f.read(CHUNK_SIZE)
        actual_read = len(raw_data)

        if actual_read < GGML_Q8_0_BLOCK_SIZE:
            print(f"  {label}: insufficient data ({actual_read} bytes), skipping")
            continue

        n_blocks = actual_read // GGML_Q8_0_BLOCK_SIZE
        aligned_read = n_blocks * GGML_Q8_0_BLOCK_SIZE
        blocks = [raw_data[i*GGML_Q8_0_BLOCK_SIZE:(i+1)*GGML_Q8_0_BLOCK_SIZE]
                  for i in range(n_blocks)]

        # Extract all weight values
        all_weights = []
        for block in blocks:
            all_weights.extend(list(block[2:34]))
        n_weights = len(all_weights)

        # Method 1: Histogram + Codebook
        compressed_hc, n_distinct, bits_per_idx, hdr_sz, idx_sz = \
            compress_histogram_codebook(all_weights)
        ratio_hc = len(compressed_hc) / actual_read

        # Method 2: 288-cell bitmap
        compressed_bm, total_active = compress_288_bitmap(blocks)
        ratio_bm = len(compressed_bm) / actual_read

        # Method 3: 288 histogram per block
        compressed_288h, total_active_288 = compress_288_histogram_per_block(blocks)
        ratio_288h = len(compressed_288h) / actual_read

        distinct_vals = sorted(set(all_weights))

        print(f"{label:<22} {actual_read:>7}B {len(compressed_hc):>9}B {ratio_hc:>5.2f}x "
              f"{len(compressed_bm):>9}B {ratio_bm:>5.2f}x "
              f"{n_distinct:>8} {bits_per_idx:>7}")

        results.append({
            'label': label,
            'raw': actual_read,
            'hist_codebook': len(compressed_hc),
            'hist_ratio': ratio_hc,
            'bitmap': len(compressed_bm),
            'bitmap_ratio': ratio_bm,
            'hist_288': len(compressed_288h),
            'hist_288_ratio': ratio_288h,
            'distinct': n_distinct,
            'bits_per_idx': bits_per_idx,
            'n_blocks': n_blocks,
            'n_weights': n_weights,
            'total_active_cells': total_active,
        })

print("-" * 80)

# ── Detailed breakdown for first chunk ──
if results:
    r = results[0]
    print(f"\n[3] Detailed breakdown ({r['label']}):")
    print(f"  Input: {r['raw']} bytes ({r['n_blocks']} Q8_0 blocks, {r['n_weights']} weights)")
    print(f"  Distinct values: {r['distinct']}")
    print(f"  Bits per codebook index: {r['bits_per_idx']}")
    print(f"  Histogram+Codebook:")
    print(f"    Codebook header: {1 + r['distinct']} bytes (1 count + {r['distinct']} values)")
    print(f"    Index stream: {r['hist_codebook'] - 1 - r['distinct']} bytes")
    print(f"    Total: {r['hist_codebook']} bytes ({r['hist_ratio']:.3f}x of raw)")
    print(f"  288-Cell Bitmap:")
    print(f"    Active cells per block: {r['total_active_cells'] / r['n_blocks']:.1f}")
    print(f"    Total: {r['bitmap']} bytes ({r['bitmap_ratio']:.3f}x of raw)")
    print(f"  288-Cell Histogram (per-block):")
    print(f"    Total: {r['hist_288']} bytes ({r['hist_288_ratio']:.3f}x of raw)")

# ── Bridge_288 mapping visualization ──
print(f"\n[4] bridge_288 mapping (first 10 weight values from first chunk):")
if results:
    with open(GGUF_PATH, 'rb') as f:
        f.seek(test_offsets[0][1], 0)
        sample = f.read(GGML_Q8_0_BLOCK_SIZE)
    weights = list(sample[2:34])
    print(f"  {'Value':>5} {'Hex':>4} {'Face':>4} {'Dir':>3} {'Cell':>4} {'FlatKey':>7}")
    for w in weights[:10]:
        face, direction, cell = bridge_288(w)
        flat_key = (w * 81) % 20736
        print(f"  {w:>5} 0x{w:02X} {face:>4} {direction:>3} {cell:>4} {flat_key:>7}")

# ── Summary ──
print(f"\n[5] Summary:")
if results:
    best_hc = min(results, key=lambda x: x['hist_ratio'])
    best_bm = min(results, key=lambda x: x['bitmap_ratio'])
    best_288h = min(results, key=lambda x: x['hist_288_ratio'])
    avg_hc = sum(r['hist_ratio'] for r in results) / len(results)
    avg_bm = sum(r['bitmap_ratio'] for r in results) / len(results)
    avg_288h = sum(r['hist_288_ratio'] for r in results) / len(results)

    print(f"  Histogram+Codebook (global):")
    print(f"    Best ratio:  {best_hc['hist_ratio']:.3f}x ({best_hc['label']})")
    print(f"    Average:     {avg_hc:.3f}x")
    print(f"  288-Cell Bitmap:")
    print(f"    Best ratio:  {best_bm['bitmap_ratio']:.3f}x ({best_bm['label']})")
    print(f"    Average:     {avg_bm:.3f}x")
    print(f"  288-Cell Histogram (per-block):")
    print(f"    Best ratio:  {best_288h['hist_288_ratio']:.3f}x ({best_288h['label']})")
    print(f"    Average:     {avg_288h:.3f}x")

    print(f"\n  Baseline: Q8_0 raw = 34 bytes/block = 3.375 bits/weight")
    print(f"  Theoretical minimum (uniform 256 values): {math.log2(256)/8:.3f}x = 1.000x")
    print(f"  Note: Our ratios are output_bytes / input_bytes")
    print(f"  Ratio < 1.0 = compression; > 1.0 = expansion")

reader.close()
print(f"\nDone. {datetime.now().isoformat()}")
