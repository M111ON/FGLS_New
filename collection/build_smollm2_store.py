"""
build_smollm2_store.py — Build geometry store + TW capture for SmolLM2-360M
Extracts tensors directly from local GGUF file, computes TW capture results.
"""
import os, struct, json, time, hashlib, argparse, numpy as np

p = argparse.ArgumentParser()
p.add_argument("--gguf", default="SmolLM2-360M-Instruct.F16.gguf", help="GGUF model path")
p.add_argument("--out", default="build/", help="output directory")
p.add_argument("--model-name", default="smollm2-360m", help="model identifier for geo_key")
args = p.parse_args()

GGUF_PATH  = args.gguf
OUT_DIR    = os.path.join(args.out, "smollm2_tensors_raw")
GSIDX_PATH = os.path.join(args.out, "smollm2_geom.gsidx")
GSDAT_PATH = os.path.join(args.out, "smollm2_geom.gsdat")

POGLS_FNV_PRIME  = 0x00000100000001B3
POGLS_FNV_OFFSET = 0xCBF29CE484222325
POGLS_GEO_MAGIC  = 0x00120090024005A0
FIBO = [1,1,2,3,5,8,13,21,34,55,89,144,233,377,610,987]
FRAME_CYCLE  = 1440
FRAME_FACE_SZ = 120

def fibo_addr(seed):
    h = POGLS_FNV_OFFSET
    for i in range(8):
        h ^= (seed >> (i * 8)) & 0xFF
        h = (h * POGLS_FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    for i in range(16):
        h ^= (FIBO[i] * (seed >> (i & 7))) & 0xFFFFFFFFFFFFFFFF
        h = ((h << 13) | (h >> 51)) & 0xFFFFFFFFFFFFFFFF
    h ^= POGLS_GEO_MAGIC
    h = (h * POGLS_FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    h ^= h >> 33
    return h

def tensor_geo(name, shape, model_name):
    h = hashlib.sha256(f"{model_name}:{name}:{tuple(shape)}".encode()).digest()
    seed = struct.unpack_from('<Q', h)[0]
    geo_key = fibo_addr(seed)
    enc = geo_key % FRAME_CYCLE
    face = enc // FRAME_FACE_SZ
    zone = face
    role = 'attention' if 'attn' in name else \
           'ffn' if 'ffn' in name or 'mlp' in name else \
           'norm' if 'norm' in name else \
           'embedding' if 'embd' in name or 'output' in name else 'other'
    return {'geo_key': geo_key, 'enc': enc, 'face': face, 'zone': zone, 'role': role}

GGML_DTYPE_SIZES = {
    0: 4, 1: 2, 2: 18, 3: 18, 4: 18, 5: 18, 6: 22, 7: 22,
    8: 34, 9: 34, 10: 38, 11: 50, 12: 144, 13: 176, 14: 210, 15: 210,
    16: 18, 17: 18, 18: 38, 19: 18, 20: 22, 21: 50, 22: 38,
    23: 38, 24: 1, 25: 2, 26: 4, 27: 8,
    28: 8, 29: 24, 30: 2,
    31: 18, 32: 18, 33: 18, 34: 18, 35: 18, 36: 22,
}
GGML_DTYPE_NAMES = {
    0:"F32",1:"F16",2:"Q4_0",3:"Q4_1",6:"Q5_0",7:"Q5_1",
    8:"Q8_0",9:"Q8_1",10:"Q2_K",11:"Q3_K",12:"Q4_K",
    13:"Q5_K",14:"Q6_K",15:"Q8_K",24:"I8",25:"I16",26:"I32",27:"I64",
    28:"F64",30:"BF16",
}

# header type codes (GGUF v3)
# 0=u8,1=i8,2=u16,3=i16,4=u32,5=i32,6=f32,7=bool,8=str,9=arr,10=u64,11=i64,12=f64

with open(GGUF_PATH, 'rb') as f:
    magic = f.read(4)
    assert magic == b'GGUF', f'bad magic'
    version = struct.unpack('<I', f.read(4))[0]
    n_tensors = struct.unpack('<Q', f.read(8))[0]
    n_kv = struct.unpack('<Q', f.read(8))[0]
    print(f'GGUF v{version}, {n_tensors} tensors, {n_kv} KV entries')

    # skip KV pairs
    for i in range(n_kv):
        k_len = struct.unpack('<Q', f.read(8))[0]
        f.read(k_len)
        vtype = struct.unpack('<I', f.read(4))[0]
        if vtype == 8:
            slen = struct.unpack('<Q', f.read(8))[0]
            f.read(slen)
        elif vtype == 9:
            atype = struct.unpack('<I', f.read(4))[0]
            alen = struct.unpack('<Q', f.read(8))[0]
            if atype == 8:
                for j in range(alen):
                    sl = struct.unpack('<Q', f.read(8))[0]
                    f.read(sl)
            else:
                sz = {0:1,1:1,2:2,3:2,4:4,5:4,6:4,7:1,10:8,11:8,12:8}.get(atype, 4)
                f.read(alen * sz)
        elif vtype in (0,7): f.read(1)
        elif vtype in (4,5,6): f.read(4)
        elif vtype in (10,11): f.read(8)
        elif vtype == 12: f.read(8)
        else: raise ValueError(f'unknown vtype {vtype} at KV {i}')

    data_start = f.tell()
    print(f'KV data ends at: {data_start} (tensor info begins)')

    # read tensor info
    tensors = []
    for i in range(n_tensors):
        nlen = struct.unpack('<Q', f.read(8))[0]
        tname = f.read(nlen).decode('utf-8', errors='replace')
        ndims = struct.unpack('<I', f.read(4))[0]
        shape = [struct.unpack('<Q', f.read(8))[0] for _ in range(ndims)]
        dtype = struct.unpack('<I', f.read(4))[0]
        offset = struct.unpack('<Q', f.read(8))[0]
        tensors.append({'name': tname, 'shape': shape, 'dtype': dtype, 'offset': offset})

    # compute nbytes
    for t in tensors:
        sz = 1
        for d in t['shape']: sz *= d
        block_sz = GGML_DTYPE_SIZES.get(t['dtype'], 4)
        if t['dtype'] in (2,3,6,7,8,9,16,17,31,32,33,34,35):
            nb = (sz + 31) // 32
            t['nbytes'] = nb * block_sz
        elif t['dtype'] in (10,11,12,13,14,15,18,21,22,23,29):
            nb = (sz + 255) // 256
            t['nbytes'] = nb * block_sz
        elif t['dtype'] in (0,1,24,25,26,27,28,30):
            t['nbytes'] = sz * block_sz
        else:
            t['nbytes'] = sz * 4

    # compute data offset: after tensor info ends
    tensor_info_end = f.tell()

print(f'Tensor info: {len(tensors)} tensors, info ends at offset {tensor_info_end}')
print(f'First 5 tensors:')
for t in tensors[:5]:
    dtype_name = GGML_DTYPE_NAMES.get(t['dtype'], f'dtype_{t["dtype"]}')
    print(f'  {t["name"]:40s} shape={str(t["shape"]):20s} {dtype_name:6s} offset={t["offset"]:>8,} nbytes={t["nbytes"]:,}')

# extract raw blobs + build store
os.makedirs(OUT_DIR, exist_ok=True)
index = []
print(f'\nExtracting {len(tensors)} tensors to {OUT_DIR}/')

data_offset = tensor_info_end
with open(GGUF_PATH, 'rb') as f:
    for i, t in enumerate(tensors):
        name = t['name']
        nbytes = t['nbytes']
        file_offset = data_offset + t['offset']

        safe_name = name.replace('/', '_')
        out_path = os.path.join(OUT_DIR, f'{safe_name}.qdat')
        qtype_path = os.path.join(OUT_DIR, f'{safe_name}.qtype')

        f.seek(file_offset)
        with open(out_path, 'wb') as out:
            remaining = nbytes
            while remaining > 0:
                chunk = f.read(min(4*1024*1024, remaining))
                if not chunk: break
                out.write(chunk)
                remaining -= len(chunk)

        with open(qtype_path, 'wb') as qt:
            qt.write(struct.pack('<B', t['dtype']))

        geo = tensor_geo(name, t['shape'], args.model_name)
        index.append({**t, 'nbytes': nbytes, 'path': out_path, **geo})

        if (i+1) % 50 == 0 or (i+1) == len(tensors):
            dtn = GGML_DTYPE_NAMES.get(t['dtype'], f'dtype_{t["dtype"]}')
            print(f'  [{i+1:3d}/{len(tensors)}] {name} → {dtn} {nbytes//1024}KB zone={geo["zone"]}')

# write gsidx JSON
with open(GSIDX_PATH, 'w') as f:
    json.dump(index, f, indent=2)
print(f'\ngsidx: {GSIDX_PATH} ({len(index)} entries)')

# write gsdat binary
with open(GSDAT_PATH, 'wb') as f:
    for e in index:
        f.write(struct.pack('<QBBH',
            e['geo_key'] & 0xFFFFFFFFFFFFFFFF,
            e['zone'] & 0xFF,
            e['face'] & 0xFF,
            min(e['nbytes'] // 1024, 0xFFFF),
        ))
gsz = os.path.getsize(GSDAT_PATH)
print(f'gsdat: {GSDAT_PATH} ({gsz} bytes, {len(index)} recs x 14B)')

total_bytes = sum(e['nbytes'] for e in index)
print(f'\nTotal: {len(index)} tensors, {total_bytes/1024**3:.2f} GB')
face_count = {}
for e in index:
    face_count[e['face']] = face_count.get(e['face'], 0) + 1
print('Face dist:')
for face in sorted(face_count):
    print(f'  face {face:2d} {"█"*face_count[face]} {face_count[face]}')

# ── TW Capture: compute zone/slot/resid for each tensor ────────
print('\n[TW Capture] Computing Triangle Wheel capture for each tensor...')

TW_SCALE = 207360
TW_N_SECTORS = 10
TW_SLOTS_PER = 6
TW_N_SLOTS = 60

# Boundary directions matching tw_capture_int.h
TW_BOUNDARY_DIR = [
    (0, 207360), (121883, 167758), (197211, 64078), (197211, -64078),
    (121883, -167758), (0, -207360), (-121883, -167758), (-197211, -64078),
    (-197211, 64078), (-121883, 167758),
]

# 6 slot centroids per sector (x10 sectors), matching C header
TW_SLOT_LOCAL = [
    [(0, 238464), (-26937, 222912), (-26937, 191808), (0, 176256), (26937, 191808), (26937, 222912)],
    [(140166, 192922), (109232, 196172), (90949, 171009), (103601, 142594), (134534, 139342), (152817, 164507)],
    [(226792, 73689), (203678, 94502), (174097, 84890), (167629, 54466), (190744, 33653), (220326, 43265)],
    [(226792, -73689), (220326, -43265), (190744, -33653), (167629, -54466), (174097, -84890), (203678, -94502)],
    [(140166, -192922), (152817, -164507), (134534, -139342), (103601, -142594), (90949, -171009), (109232, -196172)],
    [(0, -238464), (26937, -222912), (26937, -191808), (0, -176256), (-26937, -191808), (-26937, -222912)],
    [(-140166, -192922), (-109232, -196172), (-90949, -171009), (-103601, -142594), (-134534, -139342), (-152817, -164507)],
    [(-226792, -73689), (-203678, -94502), (-174097, -84890), (-167629, -54466), (-190744, -33653), (-220326, -43265)],
    [(-226792, 73689), (-220326, 43265), (-190744, 33653), (-167629, 54466), (-174097, 84890), (-203678, 94502)],
    [(-140166, 192922), (-152817, 164507), (-134534, 139342), (-103601, 142594), (-90949, 171009), (-109232, 196172)],
]

def _tw_cross(ax, ay, bx, by):
    return ax * by - ay * bx

def _tw_dequant_tensor(raw_bytes, dtype, max_vals=4096):
    """Dequantize tensor raw bytes → flat float list.
    dtype 0 = F32, dtype 8 = Q8_0."""
    if dtype == 0:
        # F32: raw bytes are float32
        n = min(len(raw_bytes) // 4, max_vals)
        arr = struct.unpack(f'<{n}f', raw_bytes[:n*4])
        return [0.0 if (x != x or abs(x) > 1e10) else x for x in arr]
    elif dtype == 8:
        # Q8_0: blocks of 34 bytes (2B f16 scale + 32 int8)
        n_blocks = len(raw_bytes) // 34
        out = []
        for b in range(n_blocks):
            if len(out) >= max_vals: break
            block = raw_bytes[b*34:(b+1)*34]
            scale_bits = struct.unpack('<H', block[0:2])[0]
            scale_arr = np.frombuffer(struct.pack('<H', scale_bits), dtype=np.float16)
            scale_f = float(scale_arr[0])
            if scale_f == 0.0 or not (scale_f > -1e10 and scale_f < 1e10):
                scale_f = 1.0
            for i in range(32):
                if len(out) >= max_vals: break
                q = block[2 + i]
                if q >= 128: q -= 256
                out.append(q * scale_f)
        return out
    else:
        return []

def _tw_capture_float(sig_x, sig_y):
    """TW capture from float signature → (zone, slot, resid_x, resid_y, drain, drain_zone, drain_slot)"""
    vx = int(sig_x * TW_SCALE)
    vy = int(sig_y * TW_SCALE)

    # find sector via cross-product sign test
    cross = [_tw_cross(bx, by, vx, vy) for (bx, by) in TW_BOUNDARY_DIR]
    sector = 0
    mincross = -1
    for k in range(TW_N_SECTORS):
        kn = (k + 1) % TW_N_SECTORS
        if cross[k] <= 0 and cross[kn] >= 0:
            sector = k
        a = abs(cross[k])
        if mincross < 0 or a < mincross:
            mincross = a

    # pick nearest slot within sector
    best = 0
    bd = -1
    for j in range(TW_SLOTS_PER):
        cx, cy = TW_SLOT_LOCAL[sector][j]
        dx = vx - cx
        dy = vy - cy
        d = dx*dx + dy*dy
        if bd < 0 or d < bd:
            bd = d
            best = j

    slot = sector * TW_SLOTS_PER + best
    rx = vx - TW_SLOT_LOCAL[sector][best][0]
    ry = vy - TW_SLOT_LOCAL[sector][best][1]

    # drain test
    vmag2 = vx*vx + vy*vy
    lhs = mincross * mincross * 1000 * 1000
    rhs = vmag2 * TW_SCALE * TW_SCALE * 9 * 9
    drain = 1 if (vmag2 > 0 and lhs < rhs) else 0
    drain_zone = 0
    drain_slot = 0
    if drain:
        cross_prev = _tw_cross(TW_BOUNDARY_DIR[sector][0], TW_BOUNDARY_DIR[sector][1], vx, vy)
        cross_next = _tw_cross(TW_BOUNDARY_DIR[(sector+1)%TW_N_SECTORS][0],
                                TW_BOUNDARY_DIR[(sector+1)%TW_N_SECTORS][1], vx, vy)
        secondary = (sector - 1) % TW_N_SECTORS if abs(cross_prev) < abs(cross_next) else (sector + 1) % TW_N_SECTORS
        drain_zone = secondary
        bd2 = -1
        bj2 = 0
        for j in range(TW_SLOTS_PER):
            cx, cy = TW_SLOT_LOCAL[secondary][j]
            dx = vx - cx
            dy = vy - cy
            d = dx*dx + dy*dy
            if bd2 < 0 or d < bd2:
                bd2 = d
                bj2 = j
        drain_slot = secondary * TW_SLOTS_PER + bj2

    return {'zone': sector, 'slot': slot, 'resid_x': rx, 'resid_y': ry,
            'drain': drain, 'drain_zone': drain_zone, 'drain_slot': drain_slot,
            'sig_x': sig_x, 'sig_y': sig_y}

# Dequantize and compute TW for each tensor
tw_index = []
tw_zone_counts = [0]*TW_N_SECTORS
tw_slot_counts = [0]*TW_N_SLOTS
for e in index:
    qdat_path = e['path']
    with open(qdat_path, 'rb') as f:
        raw = f.read()
    vals = _tw_dequant_tensor(raw, e['dtype'], max_vals=4096)
    n = len(vals)
    if n < 2:
        tw_result = {'zone': 0, 'slot': 0, 'resid_x': 0, 'resid_y': 0,
                     'drain': 0, 'drain_zone': 0, 'drain_slot': 0,
                     'sig_x': 0.0, 'sig_y': 0.0}
    else:
        half = n // 2
        sig_x = sum(vals[:half]) / half
        sig_y = sum(vals[half:]) / (n - half)
        tw_result = _tw_capture_float(sig_x, sig_y)
    tw_index.append({'name': e['name'], **tw_result})
    tw_zone_counts[tw_result['zone']] += 1
    tw_slot_counts[tw_result['slot']] += 1

# Write TW index
TWIDX_PATH = os.path.join(args.out, "smollm2_tw.twidx")
with open(TWIDX_PATH, 'w') as f:
    json.dump(tw_index, f, indent=2)
print(f'\nTW Index: {TWIDX_PATH} ({len(tw_index)} entries)')
print(f'Tensor count: {len(tw_index)}')
print(f'Zone distribution: {tw_zone_counts}')
used_slots = sum(1 for c in tw_slot_counts if c > 0)
print(f'Active slots: {used_slots}/{TW_N_SLOTS}')
import math
total = len(tw_index)
entropy = 0
for c in tw_zone_counts:
    if c > 0:
        p = c / total
        entropy -= p * math.log2(p)
print(f'Zone entropy: {entropy:.4f} bits (max {math.log2(TW_N_SECTORS):.4f})')

print('\n✓ SmolLM2 store ready')
print(f'  blobs: {OUT_DIR}/')
print(f'  gsidx: {GSIDX_PATH}')
print(f'  gsdat: {GSDAT_PATH}')
print(f'  twidx: {TWIDX_PATH}')
