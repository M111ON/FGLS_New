"""
Route-based geometric ordering experiment.
Use model's natural topology (layers → heads → blocks) as geometry.
No training, no face perturbation — pure rearrangement of existing weights.
"""
import gguf, struct, numpy as np, zstandard as zstd, time

MODEL = r'I:\model\SmolLM2-360M-Instruct.Q8_0.gguf'

def zstd_size(data, level=9):
    return len(zstd.ZstdCompressor(level=level).compress(data))

r = gguf.GGUFReader(MODEL)

# Get all tensor names and group by family
tensors = {}
for t in r.tensors:
    tensors[t.name] = bytes(t.data)

# Group by tensor type (across layers)
families = {}
for name, data in tensors.items():
    base = name.split('.')
    if len(base) < 3:
        continue
    if base[0] != 'blk':
        continue
    layer = int(base[1])
    ttype = '.'.join(base[2:])  # e.g. "attn_q.weight"
    families.setdefault(ttype, []).append((layer, data))

print(f"{'Tensor type':40s} {'Count':>5s} {'Size/tensor':>12s} {'Raw ratio':>10s} {'Geo ratio':>10s} {'Savings':>8s}")
print("-"*85)

total_raw_bytes = 0
total_geo_raw = 0
total_comp_raw = 0
total_comp_geo = 0

for ttype, layer_data in sorted(families.items()):
    layer_data.sort()
    raws = [d for _, d in layer_data]
    n = len(raws)
    if n < 2:
        continue
    
    sz = len(raws[0])
    total_raw_bytes += sz * n
    
    # Raw concatenation
    cat_raw = b''.join(raws)
    comp_raw_sz = zstd_size(cat_raw)
    raw_ratio = comp_raw_sz / len(cat_raw)
    
    # --- Geometric ordering strategies ---
    
    # Strategy A: Intra-tensor row-wise delta
    # For Q8_0, each tensor has shape [rows, cols].
    # Q8_0 block size = 36 bytes (f32 scale + 32*i8)
    # The block structure is: each row is ceil(cols/32) blocks
    # Row-wise delta: store row N as delta from row N-1
    
    n_blocks = sz // 36
    # Infer rows/cols: blocks_per_row = cols / 32, n_rows = n_blocks / blocks_per_row
    # For attn_q [960, 960]: cols=960, rows=960, blocks_per_row=30, n_rows=32
    # Actually GGUF stores row-major, so each block of 36 bytes = 32 consecutive elements
    
    geo_data = bytearray()
    
    if ttype.startswith('attn') and 'weight' in ttype:
        # Group by head: for multi-head attention, arrange by head index
        # SmolLM2: n_heads=12 for attn_q, n_kv_heads=4 for attn_k
        if 'q' in ttype:
            n_heads = 12
            head_dim = 64  # 768/12 for SmolLM2... wait, embd=960 for 360M
            # Actually for SmolLM2-360M: embd=960, n_head=12, head_dim=80
            # But let me detect from shape
            pass
        
        # Simple approach: per-block delta across layers
        # Group blocks by position across layers → geometric coherence
        for b in range(min(n_blocks, 1000)):  # limit for speed
            # Extract this block from every layer
            block_vals = bytearray()
            for d in raws:
                off = b * 36
                block_vals.extend(d[off:off+36])
            geo_data.extend(block_vals)
    else:
        # For FFN tensors: per-layer delta is simpler
        base = raws[0]
        geo_data.extend(base)
        for d in raws[1:]:
            # int16 diff (best from previous experiment)
            base_u8 = np.frombuffer(base, dtype=np.uint8)
            d_u8 = np.frombuffer(d, dtype=np.uint8)
            diff = d_u8.astype(np.int16) - base_u8.astype(np.int16)
            geo_data.extend(diff.astype(np.int16).tobytes())
    
    if len(geo_data) > 0:
        comp_geo_sz = zstd_size(bytes(geo_data))
        geo_ratio = comp_geo_sz / len(geo_data)
    else:
        geo_ratio = 1.0
    
    savings = (1 - comp_geo_sz / comp_raw_sz) * 100
    sz_str = f"{sz:,}"
    print(f"{ttype:40s} {n:>5d} {sz_str:>12s} {raw_ratio:.4f}    {geo_ratio:.4f}    {savings:>+7.1f}%")
    
    total_comp_raw += comp_raw_sz
    total_comp_geo += comp_geo_sz

print("-"*85)
print(f"{'TOTAL':40s} {'':>5s} {'':>12s} {'':>10s} {'':>10s} {(1-total_comp_geo/total_comp_raw)*100:>+7.1f}%")

# --- Specific strategies ---
print("\n\n--- Detailed per-strategy breakdown (attn_q only) ---")
ttype = 'attn_q.weight'
layer_data = families[ttype]
layer_data.sort()
raws = [d for _, d in layer_data]
n = len(raws)
sz = len(raws[0])
n_blocks = sz // 36

# Determine Q8 block structure
# For [960, 960]: 960 cols → 30 blocks per row, 960 rows = 28800 blocks
# But we have 27200 blocks... let me check
print(f"\n  Shape inference: {sz} bytes = {n_blocks} blocks")
print(f"  36 * {n_blocks} = {36*n_blocks} (bytes match)")

# Try different block arrangements
arrangements = {
    'Raw concatenation': b''.join(raws),
}

# S1: Per-block cross-layer (block 0 from all layers, then block 1, etc.)
s1 = bytearray()
for b in range(n_blocks):
    for d in raws:
        s1.extend(d[b*36:(b+1)*36])
arrangements['Block-interleaved across layers'] = bytes(s1)

# S2: Row-wise delta (each row = delta from previous row) within each layer
s2 = bytearray()
n_per_row = 30  # 960 cols / 32 per block
for d in raws:
    # Read as float32 scales + int8 weights
    row_scales = np.zeros(n_per_row, dtype=np.float32)
    row_weights = np.zeros((n_per_row, 32), dtype=np.int8)
    for r in range(n_per_row):
        off = r * 36
        row_scales[r] = struct.unpack('<f', d[off:off+4])[0]
        for j in range(32):
            v = d[off+4+j]
            row_weights[r,j] = v - 256 if v >= 128 else v
    
    # Store first row raw, then deltas
    s2.extend(struct.pack('<f', row_scales[0]))
    s2.extend(row_weights[0].astype(np.int8).tobytes())
    for r in range(1, n_per_row):
        sd = struct.pack('<f', row_scales[r] - row_scales[r-1])
        wd = (row_weights[r].astype(np.int16) - row_weights[r-1].astype(np.int16)).astype(np.int16)
        s2.extend(sd)
        s2.extend(wd.tobytes())
arrangements['Row-delta (per-layer)'] = bytes(s2)

# S3: Head-grouped (for attention: n_heads=12, head_dim=80)
# For SmolLM2: n_embd=960, n_head=12, so each head has 80 dims
# 80 dims / 32 per block = 2.5 blocks... won't align cleanly
# Try: n_head=12, cols_per_head=80, blocks_per_head=3 (96 cols, last 16 padded)
# Actually let me use proper head structure
cols = 960
head_dim = 80
n_heads = cols // head_dim  # 12
if cols % head_dim == 0:
    blocks_per_head = head_dim // 32  # 2.5 → won't work cleanly
    # Try: pack rows by head group
    s3 = bytearray()
    for d in raws:
        # Row order: head0 rows, head1 rows, ..., head11 rows
        # Each row has 30 blocks (960/32). Group rows by head.
        n_rows = n_blocks // (cols // 32)  # rows per tensor
        rows_per_head = n_rows // n_heads
        for h in range(n_heads):
            for r in range(rows_per_head):
                row_idx = h + r * n_heads  # interleave
                row_off = row_idx * (cols // 32) * 36
                s3.extend(d[row_off:row_off + (cols // 32) * 36])
    arrangements['Head-grouped (rows by head)'] = bytes(s3)

for name, data in arrangements.items():
    comp_sz = zstd_size(data)
    ratio = comp_sz / len(data)
    raw_sz_equiv = zstd_size(b''.join(raws))
    if name == 'Raw concatenation':
        base_sz = comp_sz
    savings = (1 - comp_sz / base_sz) * 100
    print(f"  {name:45s} ratio={ratio:.4f}  size={comp_sz/1024:.0f}KB  vs raw={savings:+.1f}%")
