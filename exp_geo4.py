"""
Experiment 4: Full model scale — base + delta for ALL 32 layers of attn_q
Compare: raw vs base+delta compression
"""
import gguf, struct, numpy as np, zstandard as zstd, time

MODEL = r'I:\model\SmolLM2-360M-Instruct.Q8_0.gguf'
BLOCK = 36

def zstd_ratio(data, level=9):
    return len(zstd.ZstdCompressor(level=level).compress(data)) / len(data)

r = gguf.GGUFReader(MODEL)

# Collect ALL attn_q weights (layers 0..31)
names = [f'blk.{l}.attn_q.weight' for l in range(32)]
raws = []
for n in names:
    for t in r.tensors:
        if t.name == n:
            raws.append(bytes(t.data))
            break

n_tensors = len(raws)
n_blocks = len(raws[0]) // BLOCK
print(f"Tensors: {n_tensors}, Blocks/tensor: {n_blocks}, Bytes total: {n_tensors * len(raws[0])}")

# 1. Raw concatenation
cat_raw = b''.join(raws)
t0 = time.time()
raw_ratio = zstd_ratio(cat_raw)
t1 = time.time()
print(f"\n{'Strategy':50s} {'Ratio':>8s} {'Time':>8s}")
print("-"*68)
print(f"{'Raw concatenation (32 layers)':50s} {raw_ratio:.4f} {t1-t0:.3f}s")

# 2. Base + delta: store layer 0 raw, then deltas for 1..31
t0 = time.time()
deltas = bytearray()
base = raws[0]
deltas.extend(base)
for i in range(1, n_tensors):
    # Simple byte-level delta: XOR between layers
    xor_data = bytes(a ^ b for a, b in zip(base, raws[i]))
    deltas.extend(xor_data)
delta_ratio = zstd_ratio(bytes(deltas))
t1 = time.time()
print(f"{'Base(l0) + XOR delta(l1..l31)':50s} {delta_ratio:.4f} {t1-t0:.3f}s")

# 3. F32 scale extraction + base/delta
t0 = time.time()
all_scales = []
all_weights = []
for d in raws:
    s = np.frombuffer(d, dtype=np.float32, count=n_blocks)
    w = np.frombuffer(d, offset=4, dtype=np.int8, count=n_blocks*32)
    all_scales.append(s)
    all_weights.append(w)

# Store: base_scales + scale_deltas + all_weights as one chunk
scales_cat = np.concatenate([all_scales[0]] + [(all_scales[i] - all_scales[0]).astype(np.float32) for i in range(1, n_tensors)])
weights_cat = np.concatenate(all_weights).astype(np.int8).tobytes()
sep_data = scales_cat.tobytes() + weights_cat
sep_ratio = zstd_ratio(sep_data)
t1 = time.time()
print(f"{'Base_scales + delta_scales + all_weights':50s} {sep_ratio:.4f} {t1-t0:.3f}s")

# 4. Even simpler: just use int16 for deltas (wider range than XOR)
t0 = time.time()
base_bytes = np.frombuffer(base, dtype=np.uint8)
combined = bytearray(base)
for i in range(1, n_tensors):
    d_bytes = np.frombuffer(raws[i], dtype=np.uint8)
    diff = d_bytes.astype(np.int16) - base_bytes.astype(np.int16)
    combined.extend(diff.astype(np.int16).tobytes())
diff16_ratio = zstd_ratio(bytes(combined))
t1 = time.time()
print(f"{'Base(l0) + int16 diff(l1..l31)':50s} {diff16_ratio:.4f} {t1-t0:.3f}s")

# 5. What if we use delta per-block (more memory, but better compression)?
# Q8_0 per-block: 36 bytes. Store base, then for each block in other layers, store delta as (scale_diff_f32 + weight_diff_i16)
t0 = time.time()
combined2 = bytearray(base)
for i in range(1, n_tensors):
    for b in range(n_blocks):
        bo = b * BLOCK
        # base
        bs = struct.unpack('<f', base[bo:bo+4])[0]
        bw = list(base[bo+4:bo+36])
        # delta
        ds = struct.unpack('<f', raws[i][bo:bo+4])[0]
        dw = list(raws[i][bo+4:bo+36])
        # store scale delta as f32, weight deltas as i16
        sd = struct.pack('<f', ds - bs)
        combined2.extend(sd)
        for wb, wd in zip(bw, dw):
            combined2.extend(struct.pack('<h', wd - wb))
# 4 + 32*2 = 68 bytes per block instead of 36
perblock_ratio = zstd_ratio(bytes(combined2))
t1 = time.time()
print(f"{'Base + per-block delta (f32+i16*32)':50s} {perblock_ratio:.4f} {t1-t0:.3f}s")

# 6. Baseline: total raw size vs what delta storage would be
raw_size = n_tensors * len(base)
delta_size = len(base) + (n_tensors - 1) * n_blocks * 68  # per-block delta overhead
print(f"\n--- Absolute sizes ---")
print(f"Raw total: {raw_size:,} bytes")
print(f"Base + delta size: {delta_size:,} bytes ({(n_tensors-1)*n_blocks*68/raw_size*100:.1f}% overhead before compression)")

# 7. Check delta sparsity: % of XOR bytes that are 0
zero_count = 0
total_count = 0
for i in range(1, min(5, n_tensors)):
    xor = bytes(a ^ b for a, b in zip(base, raws[i]))
    zero_count += sum(1 for b in xor if b == 0)
    total_count += len(xor)
print(f"\nXOR delta sparsity (vs l0): {zero_count/total_count*100:.1f}% zeros (first 5 layers)")
