"""
Full model compression test: Histogram+Codebook on entire SmolLM2-360M Q8_0 GGUF
─────────────────────────────────────────────────────────────────────────────────
Read ALL Q8_0 raw bytes, compress with per-chunk and global codebook.
"""
import struct, time, math
from collections import Counter
from gguf import GGUFReader

GGUF_PATH = "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf"
CHUNK_SIZE = 1048576  # 1MB

# ══════════════════════════════════════════════════════════════════════════════
# Codebook compression
# ══════════════════════════════════════════════════════════════════════════════

def compress_codebook(data_bytes, codebook=None):
    """
    Compress uint8 data using Histogram+Codebook.
    If codebook is None, build from data; otherwise use provided codebook.
    Returns (compressed_size, n_distinct, bits_per_index, header_size, indices_size).
    """
    if codebook is None:
        counter = Counter(data_bytes)
        sorted_vals = sorted(counter.keys())
        codebook = {v: i for i, v in enumerate(sorted_vals)}
    else:
        sorted_vals = sorted(codebook.keys())

    n_distinct = len(sorted_vals)
    if n_distinct <= 1:
        bits_per_index = 1
    else:
        bits_per_index = math.ceil(math.log2(n_distinct))

    header_size = 1 + n_distinct  # 1 byte count + N bytes values
    n_weights = len(data_bytes)
    indices_bits = n_weights * bits_per_index
    indices_size = math.ceil(indices_bits / 8)
    compressed_size = header_size + indices_size

    return compressed_size, n_distinct, bits_per_index, header_size, indices_size


# ══════════════════════════════════════════════════════════════════════════════
# Main
# ══════════════════════════════════════════════════════════════════════════════

print("=" * 80)
print("FULL MODEL COMPRESSION TEST: SmolLM2-360M Q8_0 GGUF")
print("Method: Histogram + Codebook (per-chunk and global)")
print("=" * 80)
print()

# 1. Parse GGUF header
print("Parsing GGUF header...")
t0 = time.time()
r = GGUFReader(GGUF_PATH)
header_time = time.time() - t0
n_tensors = len(r.tensors)
print(f"  Parsed in {header_time:.3f}s — {n_tensors} tensors total")

# 2. Find Q8_0 tensors
q8_tensors = [t for t in r.tensors if t.tensor_type == 8]
print(f"  Q8_0 tensors: {len(q8_tensors)}")
total_q8_elements = 0
total_q8_bytes = 0
for t in q8_tensors:
    total_q8_elements += t.n_elements
    total_q8_bytes += t.n_bytes
print(f"  Total Q8_0 elements: {total_q8_elements:>12,}")
print(f"  Total Q8_0 raw bytes: {total_q8_bytes:>12,} ({total_q8_bytes/1048576:.1f} MB)")
print()

# 3. Read ALL Q8_0 raw bytes into one big buffer
print("Reading ALL Q8_0 raw tensor data...")
t1 = time.time()
all_q8_data = bytearray()
for t in q8_tensors:
    with open(GGUF_PATH, 'rb') as f:
        f.seek(t.data_offset)
        raw = f.read(t.n_bytes)
        all_q8_data.extend(raw)
read_time = time.time() - t1
read_throughput = len(all_q8_data) / 1048576 / read_time if read_time > 0 else 0
print(f"  Read {len(all_q8_data):,} bytes in {read_time:.3f}s ({read_throughput:.1f} MB/s)")
print()

# 4. Global codebook: count distinct values across entire model
print("Building global codebook across ALL Q8_0 weights...")
t2 = time.time()
global_counter = Counter(all_q8_data)
global_sorted_vals = sorted(global_counter.keys())
global_codebook = {v: i for i, v in enumerate(global_sorted_vals)}
global_n_distinct = len(global_sorted_vals)
global_bits = math.ceil(math.log2(global_n_distinct)) if global_n_distinct > 1 else 1
global_header = 1 + global_n_distinct
global_indices_bits = len(all_q8_data) * global_bits
global_indices_size = math.ceil(global_indices_bits / 8)
global_compressed = global_header + global_indices_size
global_time = time.time() - t2

print(f"  Distinct values:  {global_n_distinct}")
print(f"  Bits per index:   {global_bits}")
print(f"  Header:           {global_header:,} bytes")
print(f"  Indices:          {global_indices_size:,} bytes")
print(f"  Compressed total: {global_compressed:,} bytes ({global_compressed/1048576:.1f} MB)")
print(f"  Time:             {global_time:.3f}s")

# Top 10 value distribution
top10 = global_counter.most_common(10)
print(f"  Top 10 values:    {', '.join(f'{v}({c:,})' for v, c in top10)}")
print()

# 5. Per-chunk codebook compression
print("Compressing in 1MB chunks with per-chunk codebook...")
t3 = time.time()
n_chunks = math.ceil(len(all_q8_data) / CHUNK_SIZE)
chunk_results = []

total_perchunk_compressed = 0
for i in range(n_chunks):
    start = i * CHUNK_SIZE
    end = min(start + CHUNK_SIZE, len(all_q8_data))
    chunk = all_q8_data[start:end]

    comp_size, n_dist, bpi, hdr_sz, idx_sz = compress_codebook(chunk)
    chunk_results.append({
        'chunk': i, 'offset_mb': start / 1048576, 'size': len(chunk),
        'n_distinct': n_dist, 'bits': bpi, 'compressed': comp_size,
        'header': hdr_sz, 'indices': idx_sz
    })
    total_perchunk_compressed += comp_size

perchunk_time = time.time() - t3
raw_size = len(all_q8_data)

print(f"  Chunks processed:           {n_chunks}")
print(f"  Per-chunk compressed total: {total_perchunk_compressed:,} bytes ({total_perchunk_compressed/1048576:.1f} MB)")
print(f"  Time: {perchunk_time:.3f}s ({raw_size/1048576/perchunk_time:.1f} MB/s)")
print()

# 6. Print chunk detail table
print("─" * 110)
print(f"{'Chunk':>5} {'Offset':>8} {'Size':>10} {'Distinct':>8} {'Bits':>4} {'Header':>8} {'Indices':>12} {'Compressed':>12} {'Ratio':>6}")
print("─" * 110)
for r in chunk_results:
    ratio = r['size'] / r['compressed'] if r['compressed'] > 0 else 0
    print(f"{r['chunk']:>5} {r['offset_mb']:>7.1f}M {r['size']:>10,} {r['n_distinct']:>8} {r['bits']:>4} {r['header']:>8,} {r['indices']:>12,} {r['compressed']:>12,} {ratio:>5.2f}x")
print("─" * 110)
print()

# 7. Summary table
print("=" * 80)
print("SUMMARY")
print("=" * 80)
print(f"{'Method':<30} {'Compressed':>14} {'Ratio':>8} {'Time':>8} {'Throughput':>12}")
print("─" * 80)

ratio_pc = raw_size / total_perchunk_compressed if total_perchunk_compressed > 0 else 0
ratio_gc = raw_size / global_compressed if global_compressed > 0 else 0

print(f"{'Raw (no compression)':<30} {raw_size:>13,}B {'1.00x':>7} {'─':>7} {'─':>11}")
print(f"{'Per-chunk codebook':<30} {total_perchunk_compressed:>13,}B {ratio_pc:>7.2f}x {perchunk_time:>7.3f}s {raw_size/1048576/perchunk_time:>10.1f} MB/s")
print(f"{'Global codebook':<30} {global_compressed:>13,}B {ratio_gc:>7.2f}x {global_time:>7.3f}s {raw_size/1048576/global_time:>10.1f} MB/s")
print("─" * 80)
print()

# 8. Per-tensor breakdown
print("─" * 80)
print("PER-TENSOR BREAKDOWN")
print("─" * 80)
print(f"{'Tensor':<45} {'Size':>12} {'Distinct':>8} {'Bits':>4} {'Compressed':>12} {'Ratio':>6}")
print("─" * 80)
tensor_total_compressed = 0
for t in q8_tensors:
    with open(GGUF_PATH, 'rb') as f:
        f.seek(t.data_offset)
        raw = f.read(t.n_bytes)
    comp_size, n_dist, bpi, _, _ = compress_codebook(raw)
    ratio = len(raw) / comp_size if comp_size > 0 else 0
    tensor_total_compressed += comp_size
    print(f"  {t.name:<43} {len(raw):>12,} {n_dist:>8} {bpi:>4} {comp_size:>12,} {ratio:>5.2f}x")
print("─" * 80)
if tensor_total_compressed > 0:
    print(f"  {'TOTAL':<43} {raw_size:>12,} {'':>8} {'':>4} {tensor_total_compressed:>12,} {raw_size/tensor_total_compressed:.2f}x")
else:
    print(f"  {'TOTAL':<43} {raw_size:>12,} {'':>8} {'':>4} {'0':>12} {'N/A':>6}")
print()

# 9. Savings analysis
pc_savings = raw_size - total_perchunk_compressed
gc_savings = raw_size - global_compressed
print(f"Savings (per-chunk): {pc_savings:,} bytes ({pc_savings/1048576:.1f} MB)")
print(f"Savings (global):    {gc_savings:,} bytes ({gc_savings/1048576:.1f} MB)")
total_time = header_time + read_time + global_time + perchunk_time
print(f"Total time:          {total_time:.3f}s")
print()
print("Done.")
