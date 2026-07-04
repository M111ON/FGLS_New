"""
Experiment 2: Geo_jump interleave + cross-layer correlation
Test if organizing tensor blocks in geometric order improves compression.
"""
import gguf, struct, numpy as np, zstandard as zstd

MODEL = r'I:\model\SmolLM2-360M-Instruct.Q8_0.gguf'

def zstd_ratio(data, level=9):
    cctx = zstd.ZstdCompressor(level=level)
    return len(cctx.compress(data)) / len(data)

r = gguf.GGUFReader(MODEL)

# Collect attn_q from layers 0..2
tensors = []
for t in r.tensors:
    if 'attn_q.weight' in t.name:
        layer = int(t.name.split('.')[1])
        if layer < 3:
            tensors.append((layer, bytes(t.data)))

tensors.sort(key=lambda x: x[0])
raws = [d for _, d in tensors]

# 1. Raw individual (average)
indiv_ratios = [zstd_ratio(d) for d in raws]
avg_indiv = sum(indiv_ratios) / len(indiv_ratios)

# 2. Concatenated sequential: layer0 + layer1 + layer2
cat_data = b''.join(raws)
cat_ratio = zstd_ratio(cat_data)

# 3. Interleaved by Q8_0 block (36 bytes = f32 scale + 32*i8 weights)
# Each tensor has n_blocks blocks. Interleave: block0_l0, block0_l1, block0_l2, block1_l0, ...
BLOCK = 36
n_blocks = [len(d) // BLOCK for d in raws]
assert all(n == n_blocks[0] for n in n_blocks), "different block counts"

interleaved = bytearray()
for b in range(n_blocks[0]):
    for d in raws:
        off = b * BLOCK
        interleaved.extend(d[off:off+BLOCK])
interleaved = bytes(interleaved)
inter_ratio = zstd_ratio(interleaved)

# 4. Per-block scale array (float32) - should compress well if scales are similar across layers
# Extract all scales from all 3 layers
scales = []
for d in raws:
    for b in range(n_blocks[0]):
        s = struct.unpack('<f', d[b*BLOCK:b*BLOCK+4])[0]
        scales.append(s)
scale_arr = np.array(scales, dtype=np.float32)
scale_ratio = zstd_ratio(scale_arr.tobytes())

# 5. Delta between layer layers: store layer0, then deltas layer1 - layer0, layer2 - layer0
# Reinterpret Q8_0 blocks as (scale, i8[32]) structs
def blocks_to_arrays(data):
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

base_s, base_w = blocks_to_arrays(raws[0])
delta_stream = raws[0]  # start with layer 0 raw
for d in raws[1:]:
    s, w = blocks_to_arrays(d)
    # Store scale delta as f32, weight delta as i16 (avoid overflow)
    sd = (s - base_s).astype(np.float32)
    wd = (w.astype(np.int16) - base_w.astype(np.int16)).astype(np.int16)
    delta_stream += sd.tobytes() + wd.tobytes()

delta_ratio = zstd_ratio(delta_stream)

# 6. Frustum ordering: group blocks by geometric similarity pattern
# In the triplet world, adjacent layers share similar weight patterns
# Try: sort blocks by their sum (proxy for geometric "position intensity")
frustum_data = bytearray()
for d in raws:
    blocks = []
    for b in range(n_blocks[0]):
        off = b * BLOCK
        s = struct.unpack('<f', d[off:off+4])[0]
        w_sum = sum(d[off+4:off+36])
        blocks.append((s, w_sum, d[off:off+36]))
    # Sort blocks by scale magnitude (geometric intensity proxy)
    blocks.sort(key=lambda x: abs(x[0]))
    frustum_data.extend(b''.join(b[2] for b in blocks))
frustum_ratio = zstd_ratio(bytes(frustum_data))

print(f"{'Strategy':45s} {'Ratio':>8s}")
print("-"*60)
print(f"{'Individual (avg of 3 layers)':45s} {avg_indiv:.4f}")
print(f"{'Concatenated sequential':45s} {cat_ratio:.4f}")
print(f"{'Interleaved by Q8 block':45s} {inter_ratio:.4f}")
print(f"{'Scale array (f32)':45s} {scale_ratio:.4f}")
print(f"{'Base + delta (lyr0 + diff)':45s} {delta_ratio:.4f}")
print(f"{'Frustum-sorted by scale':45s} {frustum_ratio:.4f}")

# Also show per-tensor individual ratios
print(f"\nIndividual ratios: {[f'{r:.4f}' for r in indiv_ratios]}")
