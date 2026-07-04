"""
Experiment: organize before compress — test geometric ordering on real model weights.
"""
import gguf, struct, numpy as np, zstandard as zstd, sys, os

MODEL = r'I:\model\SmolLM2-360M-Instruct.Q8_0.gguf'
TENSOR_NAMES = [
    'blk.0.attn_q.weight',     # 979KB
    'blk.0.ffn_down.weight',    # 2.6MB
    'blk.0.attn_k.weight',      # 326KB
]

def zstd_ratio(data, level=9):
    cctx = zstd.ZstdCompressor(level=level)
    compressed = cctx.compress(data)
    return len(compressed) / len(data), len(compressed)

def test_ordering(name, raw_bytes):
    n = len(raw_bytes)
    raw_ratio, raw_sz = zstd_ratio(raw_bytes)
    
    # Strategy 1: sort by byte value
    sorted_bytes = bytes(sorted(raw_bytes))
    sorted_ratio, sorted_sz = zstd_ratio(sorted_bytes)
    
    # Strategy 2: for Q8_0, try separating scale from quantized values
    # Q8_0 block = f32 scale + 32 x i8 weights = 36 bytes/block
    block_sz = 36  # f32(4) + 32*i8
    n_blocks = n // block_sz
    if n_blocks > 0 and n % block_sz == 0:
        scales = bytearray()
        weights = bytearray()
        for b in range(n_blocks):
            off = b * block_sz
            scales.extend(raw_bytes[off:off+4])      # f32 scale
            weights.extend(raw_bytes[off+4:off+36])  # 32 i8 weights
        sep_data = bytes(scales + weights)
        sep_ratio, sep_sz = zstd_ratio(sep_data)
    else:
        sep_ratio = sep_sz = None
    
    # Strategy 3: row-wise delta (for 2D matrices like [rows, cols])
    # If data is structured as rows, diff adjacent rows
    # Try various row sizes
    best_delta_ratio = 1.0
    best_delta_sz = n
    for row_len in [320, 960, 2560]:
        if n % row_len != 0:
            continue
        n_rows = n // row_len
        if n_rows < 2:
            continue
        data_arr = np.frombuffer(raw_bytes, dtype=np.uint8).reshape(n_rows, row_len)
        # Delta-encode: first row stored, then diffs
        delta_arr = np.zeros_like(data_arr)
        delta_arr[0] = data_arr[0]
        delta_arr[1:] = data_arr[1:] - data_arr[:-1]
        delta_bytes = delta_arr.astype(np.int8).tobytes()  
        # Actually int8 can overflow for uint8 diff. Use int16.
        delta_arr_i16 = delta_arr.astype(np.int16)
        delta_bytes = delta_arr_i16.tobytes()
        d_ratio, d_sz = zstd_ratio(delta_bytes)
        if d_ratio < best_delta_ratio:
            best_delta_ratio = d_ratio
            best_delta_sz = d_sz
    
    # Strategy 4: read as f32 directly (if data is float-type)
    try:
        f32_data = np.frombuffer(raw_bytes[:n-(n%4)], dtype=np.float32)
        f32_ratio, f32_sz = zstd_ratio(f32_data.tobytes())
    except:
        f32_ratio = f32_sz = None
    
    return {
        'size': n,
        'raw_ratio': raw_ratio,
        'raw_sz': raw_sz,
        'sorted_ratio': sorted_ratio,
        'sorted_sz': sorted_sz,
        'sep_ratio': sep_ratio,
        'sep_sz': sep_sz,
        'best_delta_ratio': best_delta_ratio,
        'best_delta_sz': best_delta_sz,
        'f32_ratio': f32_ratio,
        'f32_sz': f32_sz,
    }

print(f"{'Tensor':40s} {'Size':>10s}  {'Raw':>8s}  {'Sorted':>8s}  {'Sep':>8s}  {'RowDelta':>8s}  {'f32':>8s}")
print("-"*100)

r = gguf.GGUFReader(MODEL)
for name in TENSOR_NAMES:
    for t in r.tensors:
        if t.name == name:
            data = bytes(t.data)  # raw Q8_0 bytes
            res = test_ordering(name, data)
            sep_str = f"{res['sep_ratio']:.4f}" if res['sep_ratio'] is not None else "N/A"
            f32_str = f"{res['f32_ratio']:.4f}" if res['f32_ratio'] is not None else "N/A"
            print(f"{name:40s} {res['size']:>10d}  {res['raw_ratio']:.4f}  {res['sorted_ratio']:.4f}  "
                  f"{sep_str:>8s}  {res['best_delta_ratio']:.4f}  {f32_str:>8s}")
            break
