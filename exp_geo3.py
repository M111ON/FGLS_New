"""
Experiment 3: Scale/weight separation + deep delta analysis
"""
import gguf, struct, numpy as np, zstandard as zstd

MODEL = r'I:\model\SmolLM2-360M-Instruct.Q8_0.gguf'
BLOCK = 36  # f32 scale + 32*i8 weights

def zstd_ratio(data, level=9):
    return len(zstd.ZstdCompressor(level=level).compress(data)) / len(data)

def get_tensor(r, name):
    for t in r.tensors:
        if t.name == name:
            return bytes(t.data)
    return None

def extract_q8_blocks(data):
    """Split Q8_0 into scale array and weight array."""
    n = len(data) // BLOCK
    scales = np.zeros(n, dtype=np.float32)
    weights = np.zeros((n, 32), dtype=np.int8)
    for i in range(n):
        off = i * BLOCK
        scales[i] = struct.unpack('<f', data[off:off+4])[0]
        for j in range(32):
            v = data[off+4+j]
            weights[i,j] = v - 256 if v >= 128 else v
    return scales, weights

r = gguf.GGUFReader(MODEL)

# Get attn_q from layers 0, 4, 8 (spread across the model)
names = [f'blk.{l}.attn_q.weight' for l in [0, 4, 8, 16, 24]]
raws = []
for n in names:
    d = get_tensor(r, n)
    if d:
        raws.append(d)
        print(f"  {n}: {len(d)} bytes = {len(d)//BLOCK} blocks")

print(f"\n{'Strategy':55s} {'Ratio':>8s}")
print("-"*65)

# 1. Raw average
ratios_raw = [zstd_ratio(d) for d in raws]
print(f"{'Raw individual (avg)':55s} {sum(ratios_raw)/len(ratios_raw):.4f}")

# 2. Base(layer0) + deltas
base_s, base_w = extract_q8_blocks(raws[0])
combined = raws[0]
for d in raws[1:]:
    s, w = extract_q8_blocks(d)
    sd = (s - base_s).astype(np.float32)
    wd = (w.astype(np.int16) - base_w.astype(np.int16)).astype(np.int16)
    combined += sd.tobytes() + wd.tobytes()
print(f"{'Base(l0) + delta(l4,l8,l16,l24)':55s} {zstd_ratio(combined):.4f}")

# 3. Scales only (all layers together, separated from weights)
all_scales = []
for d in raws:
    s, _ = extract_q8_blocks(d)
    all_scales.append(s)
scales_cat = np.concatenate(all_scales).astype(np.float32).tobytes()
print(f"{'Scales separated (float32, all layers)':55s} {zstd_ratio(scales_cat):.4f}")

# 4. Weights only (all layers together, no scales)
all_weights = []
for d in raws:
    _, w = extract_q8_blocks(d)
    all_weights.append(w.flatten().astype(np.int8).tobytes())
weights_cat = b''.join(all_weights)
print(f"{'Weights only (int8, all layers)':55s} {zstd_ratio(weights_cat):.4f}")

# 5. What if we store base scales + delta scales separately?
# Scales are f32 values that change slowly across blocks
# Separately compress scale channel and weight channel
scale_deltas = base_s.copy()  # first copy base scales
weight_all = base_w.copy()
for d in raws[1:]:
    s, w = extract_q8_blocks(d)
    scale_deltas = np.concatenate([scale_deltas, (s - base_s).astype(np.float32)])
    weight_all = np.concatenate([weight_all, w], axis=0)

# Encode: base_scales + scale_deltas + weights_separated
sep_data = base_s.astype(np.float32).tobytes()  # scales: base
for d in raws[1:]:
    s, _ = extract_q8_blocks(d)
    sep_data += (s - base_s).astype(np.float32).tobytes()  # scale deltas
sep_data += weight_all.flatten().astype(np.int8).tobytes()  # all weights together
print(f"{'Base_scales + scale_deltas + weights':55s} {zstd_ratio(sep_data):.4f}")

# 6. Int8 weight value distribution
all_w_vals = np.concatenate([extract_q8_blocks(d)[1].flatten() for d in raws])
hist, _ = np.histogram(all_w_vals, bins=256, range=(-128, 128))
nonzero = np.count_nonzero(hist)
print(f"\nWeight distribution: {len(all_w_vals)} samples, {nonzero}/256 bins populated")
print(f"  Unique values: {len(np.unique(all_w_vals))}")
print(f"  Mean: {all_w_vals.mean():.2f}, Std: {all_w_vals.std():.2f}")

# 7. Scale distribution
all_sc = np.array([extract_q8_blocks(d)[0] for d in raws])
print(f"  Scale mean: {all_sc.mean():.4f}, Scale std: {all_sc.std():.4f}")
