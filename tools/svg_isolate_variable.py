"""Controlled experiment: isolate whether base64 or representation helps compression."""
import hashlib, os, zlib, base64, time

def make_random(size=100000):
    return os.urandom(size)

def make_structured(size=100000):
    pattern = bytes(range(256))
    return (pattern * (size // 256 + 1))[:size]

# === Representation: interleave bytes (simple reordering, no encoding) ===
def repr_interleave(data, stride=4):
    """Reorder bytes: take every Nth byte sequentially. Changes local patterns."""
    buf = bytearray(len(data))
    for s in range(stride):
        for i in range(s, len(data), stride):
            buf[i] = data[(i // stride) * stride + s] if (i // stride) * stride + s < len(data) else 0
    return bytes(buf)

def repr_transpose(data, rows=100):
    """Matrix transpose: write row-major, read column-major."""
    cols = (len(data) + rows - 1) // rows
    buf = bytearray(len(data))
    for i in range(len(data)):
        r, c = i % rows, i // rows
        src_idx = r * cols + c
        if src_idx < len(data):
            buf[i] = data[src_idx]
    return bytes(buf)

def repr_delta(data):
    """Byte delta: each byte = diff from previous. Reversible."""
    out = bytearray(len(data))
    out[0] = data[0]
    for i in range(1, len(data)):
        out[i] = (data[i] - data[i-1]) & 0xFF
    return bytes(out)

def repr_chunk_rotate(data, chunk_sz=64):
    """Rotate bytes within each chunk by 1 position."""
    out = bytearray(data)
    for i in range(0, len(out), chunk_sz):
        chunk = out[i:i+chunk_sz]
        if len(chunk) > 1:
            out[i:i+chunk_sz] = chunk[1:] + chunk[:1]
    return bytes(out)

def repr_hexNibbles_split(data):
    """Split high/low nibbles into separate streams."""
    highs = bytearray(len(data))
    lows = bytearray(len(data))
    for i, b in enumerate(data):
        highs[i] = (b >> 4) & 0x0F
        lows[i] = b & 0x0F
    return bytes(highs) + bytes(lows)

# === Controlled test matrix ===

def test_one(label, data, transform_fn, use_base64):
    """Apply transform → optional base64 → zlib → measure + verify roundtrip."""
    t0 = time.time()
    
    if transform_fn:
        transformed = transform_fn(data)
    else:
        transformed = data
    
    if use_base64:
        feed = base64.b64encode(transformed)
    else:
        feed = transformed
    
    compressed = zlib.compress(feed, 9)
    
    # Roundtrip
    decompressed = zlib.decompress(compressed)
    if use_base64:
        recovered = base64.b64decode(decompressed)
    else:
        recovered = decompressed
    
    if transform_fn and transform_fn.__name__ != 'repr_delta':
        # For reversible transforms, invert
        if transform_fn == repr_interleave:
            # Inverse interleave
            inv = bytearray(len(data))
            for s in range(4):
                for i in range(s, len(data), 4):
                    idx = (i // 4) * 4 + s
                    if idx < len(data):
                        inv[idx] = recovered[i] if i < len(recovered) else 0
            recovered = bytes(inv[:len(data)])
        elif transform_fn == repr_transpose:
            # Inverse transpose
            rows_orig = 100
            cols_orig = (len(data) + rows_orig - 1) // rows_orig
            inv = bytearray(len(data))
            for i in range(len(data)):
                r, c = i % rows_orig, i // rows_orig
                dst = r * cols_orig + c
                if dst < len(data) and i < len(recovered):
                    inv[dst] = recovered[i]
            recovered = bytes(inv[:len(data)])
        elif transform_fn == repr_delta:
            # Inverse delta
            inv = bytearray(len(recovered[:len(data)]))
            inv[0] = recovered[0]
            for i in range(1, len(inv)):
                inv[i] = (inv[i-1] + recovered[i]) & 0xFF
            recovered = bytes(inv)
        elif transform_fn == repr_chunk_rotate:
            # Inverse rotate
            inv = bytearray(recovered[:len(data)])
            for i in range(0, len(inv), 64):
                chunk = inv[i:i+64]
                if len(chunk) > 1:
                    inv[i:i+64] = chunk[-1:] + chunk[:-1]
            recovered = bytes(inv)
        elif transform_fn == repr_hexNibbles_split:
            half = len(recovered) // 2
            highs = recovered[:half]
            lows = recovered[half:2*half]
            inv = bytearray(len(data))
            for i in range(min(len(data), half)):
                inv[i] = ((highs[i] & 0x0F) << 4) | (lows[i] & 0x0F)
            recovered = bytes(inv)
    
    match = (recovered[:len(data)] == data[:len(recovered)])
    elapsed = time.time() - t0
    
    return len(compressed), match, elapsed

# === Main ===

transforms = [
    ("(none - raw bytes)",    None),
    ("interleave (stride=4)", repr_interleave),
    ("matrix transpose",      repr_transpose),
    ("byte delta",            repr_delta),
    ("chunk rotate",          repr_chunk_rotate),
    ("nibble split",          repr_hexNibbles_split),
]

for kind in ['random', 'structured']:
    data = make_random(100000) if kind == 'random' else make_structured(100000)
    print(f'\n{"="*75}')
    print(f'  {kind.upper()} DATA: {len(data):,} bytes')
    print(f'{"="*75}')
    print(f'  {"Transform":<25s} {"no base64":>14s}  {"+base64":>14s}  {"Δ (b64 effect)":>14s}  roundtrip')
    print(f'  {"-"*25} {"-"*14}  {"-"*14}  {"-"*14}  {"-"*8}')
    
    for tname, tfn in transforms:
        sz_raw, match_raw, _ = test_one(tname, data, tfn, False)
        sz_b64, match_b64, _ = test_one(tname, data, tfn, True)
        delta = sz_b64 - sz_raw
        sign = '+' if delta > 0 else ''
        print(f'  {tname:<25s} {sz_raw:>10,} B  {sz_b64:>10,} B  {sign}{delta:>8,} B  '
              f'{"✓" if match_raw and match_b64 else "✗"}')

print(f'\n{"="*75}')
print(f'  ANALYSIS')
print(f'{"="*75}')
print(f'  If Δ is POSITIVE → base64 HURTS compression (adds overhead)')
print(f'  If Δ is NEGATIVE → base64 HELPS compression (reduces size)')
print(f'  If Δ is ~0       → base64 has no effect')
