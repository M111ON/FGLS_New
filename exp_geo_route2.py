"""
Route-based geometric ordering — train-free, real block format.
Q8_0 = f16 scale (2B) + 32×int8 (32B) = 34B/block
"""
import gguf, struct, numpy as np, zstandard as zstd

MODEL = r'I:\model\SmolLM2-360M-Instruct.Q8_0.gguf'
QK_BLOCK = 34  # f16(2) + 32*i8

def zstd_sz(data):
    return len(zstd.ZstdCompressor(level=9).compress(data))

r = gguf.GGUFReader(MODEL)

# Read gguf tensor metadata to confirm shapes
meta = {}
for t in r.tensors:
    if t.name.startswith('blk.0.'):
        meta[t.name.split('.')[2]] = t.shape

print(f"Tensor shapes from gguf metadata:")
for name, shape in sorted(meta.items()):
    n_elems = shape[0] * shape[1] if len(shape) > 1 else shape[0]
    blocks = n_elems // 32
    expected_bytes = blocks * QK_BLOCK
    print(f"  {name:25s} shape={str(shape):15s} elements={n_elems:>7d} blocks={blocks:>5d} expected_bytes={expected_bytes:,}")

# --- Clean test: row-delta on ONE tensor ---
print("\n--- Per-tensor row-delta (attn_q, layer 0) ---")

tensors = {}
for t in r.tensors:
    tensors[t.name] = (t.shape, bytes(t.data))

# attn_q weight [960, 960]: 28800 blocks × 34 = 979,200 bytes
name = 'blk.0.attn_q.weight'
shape, raw = tensors[name]
n_elems = shape[0] * shape[1]
n_blocks = n_elems // 32
n_rows = shape[0]
blocks_per_row = n_blocks // n_rows
cols = shape[1]

print(f"  Tensor: {name} {shape}")
print(f"  Elements: {n_elems}, Blocks: {n_blocks}")
print(f"  Rows: {n_rows}, Cols: {cols}, Blocks/row: {blocks_per_row}")
print(f"  Bytes: {len(raw)}, Expected({QK_BLOCK}×{n_blocks}): {QK_BLOCK*n_blocks}")

# 1. Single tensor raw
comp_raw_single = zstd_sz(raw)
print(f"\n  Raw single:              {comp_raw_single:>8d}  ratio={comp_raw_single/len(raw):.4f}")

# 2. Row-delta within single tensor
def row_delta_single(data, shape, blocks_per_row, qk_block=QK_BLOCK):
    out = bytearray()
    n_rows = shape[0]
    for i in range(n_rows):
        row_off = i * blocks_per_row * qk_block
        row_bytes = data[row_off:row_off + blocks_per_row * qk_block]
        if i == 0:
            out.extend(row_bytes)  # first row raw
        else:
            prev_off = (i-1) * blocks_per_row * qk_block
            prev = data[prev_off:prev_off + blocks_per_row * qk_block]
            # int16 diff
            for a, b in zip(row_bytes, prev):
                diff = a - b
                out.extend(struct.pack('<h', diff))  # int16
    return bytes(out)

rd = row_delta_single(raw, shape, blocks_per_row)
comp_rd = zstd_sz(rd)
# int16 is 2x raw size for deltas; compute effective ratio against raw equiv
rd_uncomp = len(raw) + (n_rows-1) * (blocks_per_row * QK_BLOCK * 2)  # 2x for int16
print(f"  Row-delta single:         {comp_rd:>8d}  ratio={comp_rd/rd_uncomp:.4f}  save={((1-comp_rd/comp_raw_single))*100:+.1f}%")

# 3. Cross-layer delta: store layer 0 raw, then int16 diffs for 1..31
all_layers = [tensors[f'blk.{l}.attn_q.weight'][1] for l in range(32)]
cat_raw = b''.join(all_layers)
comp_cat = zstd_sz(cat_raw)
ratio_cat = comp_cat / len(cat_raw)

# numpy f16 helpers for Python 3.10
def f16_bytes(f):
    return np.float16(f).tobytes()
def f16_val(b):
    return float(np.frombuffer(b, dtype=np.float16)[0])

# Cross-layer same-width diff (store deltas at Q8_0 block granularity)
# For each block: scale delta as f16, weight delta as int8 (Δ = clamped to -128..127)
out_xl = bytearray(all_layers[0])
for d in all_layers[1:]:
    for b in range(n_blocks):
        bo = b * QK_BLOCK
        s_a = f16_val(all_layers[0][bo:bo+2])
        s_b = f16_val(d[bo:bo+2])
        out_xl.extend(f16_bytes(s_b - s_a))
        for j in range(32):
            wd = d[bo+2+j] - all_layers[0][bo+2+j]
            if wd < -128: wd = -128
            if wd > 127: wd = 127
            out_xl.extend(struct.pack('<b', wd))
comp_xl = zstd_sz(bytes(out_xl))
xl_uncomp = len(all_layers[0]) * len(all_layers)  # same width, all layers
print(f"\n  Cross-layer raw (32 layers): {comp_cat:>8d}  ratio={ratio_cat:.4f}")
print(f"  Cross-layer Q8-delta (f16+i8):{comp_xl:>8d}  ratio={comp_xl/xl_uncomp:.4f}  save={(1-comp_xl/comp_cat)*100:+.1f}%")

# Same-width row-delta within single tensor
def row_delta_same_width(data, shape):
    n_rows = shape[0]
    blocks_per_row = (shape[0] * shape[1] // 32) // shape[0]
    out = bytearray()
    out.extend(data[:blocks_per_row * QK_BLOCK])  # first row raw
    for i in range(1, n_rows):
        prev_off = (i-1) * blocks_per_row * QK_BLOCK
        row_off = i * blocks_per_row * QK_BLOCK
        for b in range(blocks_per_row):
            po = prev_off + b * QK_BLOCK
            ro = row_off + b * QK_BLOCK
            sp = f16_val(data[po:po+2])
            sr = f16_val(data[ro:ro+2])
            out.extend(f16_bytes(sr - sp))
            for j in range(32):
                wd = data[ro+2+j] - data[po+2+j]
                if wd < -128: wd = -128
                if wd > 127: wd = 127
                out.extend(struct.pack('<b', wd))
    return bytes(out)

rd_sw = row_delta_same_width(raw, shape)
comp_rd_sw = zstd_sz(rd_sw)
rd_sw_uncomp = len(raw)
print(f"  Row-delta same-width:         {comp_rd_sw:>8d}  ratio={comp_rd_sw/rd_sw_uncomp:.4f}  save={(1-comp_rd_sw/comp_raw_single)*100:+.1f}%")

# 4. Row-delta PER LAYER then cross-layer diff of row-delta output? Too complex.
# Let's try the simplest useful approach: store as block-interleaved per-head
# For attention: n_heads=12, each head has 80 dims
# Group rows by head (8 rows per head for 960/12=80 dims... no, 960/n_heads=80, and each row has 960 cols)
# Actually for [n_rows, n_cols] = [960, 960], n_cols/n_heads = 960/12 = 80

n_heads = 12
cols_per_head = cols // n_heads  # 80
blocks_per_head = cols_per_head // 32  # 2 (64 cols per 2 blocks, last 16 cols...)

# Hmm, 80/32 = 2.5 blocks per head — not clean. Try head grouping differently.
# Group the columns (last dim) by head, not rows.

# Actually the key is: in [rows, cols], the vector stored at each row position
# is the input → output weight pattern for that output dimension. 
# Output dimensions of the same head should have similar patterns → compress better together.

# Calculate actual Q8 block structure
# Each Q8 block encodes 32 consecutive scalars from ONE row
# blocks_per_row = ceil(960/32) = 30
# Each head spans 80 consecutive columns = 80/32 = 2.5 blocks

# Alternative: arrange by column block across rows (block 0 of every row together, etc.)
# For Q8_0, block 0 = cols 0..31, block 1 = cols 32..63, etc.
# Grouping same column-block across rows should increase compressibility
# because columns within a block are from the same head

block_interleaved = bytearray()
for b in range(blocks_per_row):
    for d in all_layers:
        for r in range(n_rows):
            off = (r * blocks_per_row + b) * QK_BLOCK
            block_interleaved.extend(d[off:off + QK_BLOCK])
comp_bi = zstd_sz(bytes(block_interleaved))
print(f"  Block-interleaved (col-block × rows): {comp_bi:>8d}  ratio={comp_bi/len(block_interleaved):.4f}  save={(1-comp_bi/comp_cat)*100:+.1f}%")

# 5. Head-major ordering: arrange all rows of head 0 together, then head 1, etc.
# Each row has n_heads head-segments. We group by head across rows.
head_major = bytearray()
rows_per_head = n_rows // n_heads
for d in all_layers:
    for h in range(n_heads):
        h_col_start = h * cols_per_head
        h_block_start = h_col_start // 32
        h_blocks = cols_per_head // 32 + (1 if cols_per_head % 32 else 0)
        for r in range(n_rows):
            for b in range(h_blocks):
                block_idx = r * blocks_per_row + h_block_start + b
                if block_idx >= n_blocks: continue
                off = block_idx * QK_BLOCK
                read_sz = min(QK_BLOCK, len(d) - off)
                head_major.extend(d[off:off+read_sz])
comp_hm = zstd_sz(bytes(head_major))
print(f"  Head-major:                    {comp_hm:>8d}  ratio={comp_hm/len(head_major):.4f}  save={(1-comp_hm/comp_cat)*100:+.1f}%")

# --- SUMMARY ---
print(f"\n{'='*60}")
print(f"SUMMARY: {name}")
print(f"{'='*60}")
print(f"{'Strategy':35s} {'Compressed':>10s} {'Ratio':>8s}")
print(f"{'-'*55}")
print(f"{'Raw single tensor':35s} {comp_raw_single:>10d} {comp_raw_single/len(raw):.4f}")
print(f"{'Raw 32 layers concat':35s} {comp_cat:>10d} {ratio_cat:.4f}")
print(f"{'Cross-layer int16 diff':35s} {comp_xl:>10d} {comp_xl/len(out_xl):.4f}")
print(f"{'Rolling row-delta':35s} {comp_rd:>10d} {comp_rd/len(rd):.4f}")
