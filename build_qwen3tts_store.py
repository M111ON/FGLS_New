"""Build Qwen3-TTS geometry store from GGUF.
Same pipeline as build_smollm2_store.py.
"""
import os, struct, json, time, hashlib

GGUF_PATH = "I:/model/qwen3-tts-12hz-0.6b-base-q8_0.gguf"
OUT_DIR = "build/qwen3tts_tensors_raw"
GSIDX_PATH = "build/qwen3tts_geom.gsidx"
GSDAT_PATH = "build/qwen3tts_geom.gsdat"

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

GGML_DTYPE_SIZES = {
    0: 4, 1: 2, 2: 18, 3: 18, 8: 34, 10: 38, 11: 50,
    24: 1, 25: 2, 26: 4, 27: 8, 28: 8, 30: 2,
}
GGML_DTYPE_NAMES = {
    0:"F32",1:"F16",2:"Q4_0",3:"Q4_1",8:"Q8_0",10:"Q2_K",11:"Q3_K",
    24:"I8",25:"I16",26:"I32",27:"I64",28:"F64",30:"BF16",
}

def tensor_role(name):
    nl = name.lower()
    if 'tok_embd' in nl: return 'embedding'
    if 'output' in nl: return 'output'
    if 'attn' in nl or 'wq' in nl or 'wk' in nl or 'wv' in nl or 'wo' in nl: return 'attention'
    if 'ffn' in nl or 'ff' in nl or 'mlp' in nl or 'w1' in nl or 'w2' in nl or 'w3' in nl: return 'ffn'
    if 'norm' in nl or 'rms' in nl: return 'norm'
    if 'time' in nl or 'freq' in nl: return 'position'
    if 'adain' in nl or 'cond' in nl: return 'condition'
    if 'code' in nl or 'predictor' in nl: return 'predictor'
    if 'enc' in nl or 'dec' in nl or 'head' in nl: return 'codec'
    return 'other'

# Parse GGUF
with open(GGUF_PATH, 'rb') as f:
    magic = f.read(4)
    assert magic == b'GGUF', f'bad magic: {magic}'
    version = struct.unpack('<I', f.read(4))[0]
    n_tensors = struct.unpack('<Q', f.read(8))[0]
    n_kv = struct.unpack('<Q', f.read(8))[0]
    print(f'GGUF v{version}, {n_tensors} tensors, {n_kv} KV entries')

    # skip KV
    for i in range(n_kv):
        klen = struct.unpack('<Q', f.read(8))[0]
        f.read(klen)
        vtype = struct.unpack('<I', f.read(4))[0]
        if vtype == 8:
            slen = struct.unpack('<Q', f.read(8))[0]; f.read(slen)
        elif vtype == 9:
            atype = struct.unpack('<I', f.read(4))[0]
            alen = struct.unpack('<Q', f.read(8))[0]
            if atype == 8:
                for j in range(alen):
                    sl = struct.unpack('<Q', f.read(8))[0]; f.read(sl)
            else:
                sz = {0:1,1:1,2:2,3:2,4:4,5:4,6:4,7:1,10:8,11:8,12:8}.get(atype, 4)
                f.read(alen * sz)
        elif vtype in (0,7): f.read(1)
        elif vtype in (4,5,6): f.read(4)
        elif vtype in (10,11): f.read(8)
        elif vtype == 12: f.read(8)
        else: raise ValueError(f'unknown vtype {vtype} at KV {i}')

    data_start = f.tell()
    print(f'Tensor info starts at: {data_start}')

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
        bs = GGML_DTYPE_SIZES.get(t['dtype'], 4)
        dtype = t['dtype']
        if dtype in (2,3,8):  # Q4_0, Q4_1, Q8_0
            nb = (sz + 31) // 32
            t['nbytes'] = nb * bs
        elif dtype in (10,11):  # Q2_K, Q3_K
            nb = (sz + 255) // 256
            t['nbytes'] = nb * bs
        elif dtype in (0,1,24,25,26,27,28,30):
            t['nbytes'] = sz * bs
        else:
            t['nbytes'] = sz * 4

    tensor_info_end = f.tell()

# Print model info
print(f'\nModel: Qwen3-TTS 0.6B (qwen3tts architecture)')
print(f'Tensors: {len(tensors)}')
total_params = 0
for t in tensors:
    n = 1
    for d in t['shape']: n *= d
    total_params += n
print(f'Total params: {total_params/1e6:.1f}M')
dtype_counts = {}
for t in tensors:
    dn = GGML_DTYPE_NAMES.get(t['dtype'], f'dtype_{t["dtype"]}')
    dtype_counts[dn] = dtype_counts.get(dn, 0) + 1
print(f'Dtype distribution: {dtype_counts}')

# Extract + build store
os.makedirs(OUT_DIR, exist_ok=True)
index = []
print(f'\nExtracting {len(tensors)} tensors...')

data_offset = tensor_info_end
with open(GGUF_PATH, 'rb') as f:
    for i, t in enumerate(tensors):
        name = t['name']
        nbytes = t['nbytes']
        file_offset = data_offset + t['offset']

        safe = name.replace('/', '_').replace('\\', '_').replace('.', '_')
        out_path = os.path.join(OUT_DIR, f'{safe}.qdat')

        f.seek(file_offset)
        with open(out_path, 'wb') as out:
            remaining = nbytes
            while remaining > 0:
                chunk = f.read(min(4*1024*1024, remaining))
                if not chunk: break
                out.write(chunk)
                remaining -= len(chunk)

        # Geo key from name + shape
        h = hashlib.sha256(f"qwen3-tts-0.6b:{name}:{tuple(t['shape'])}".encode()).digest()
        seed = struct.unpack_from('<Q', h)[0]
        geo_key = fibo_addr(seed)
        enc = geo_key % FRAME_CYCLE
        face = enc // FRAME_FACE_SZ
        zone = face
        role = tensor_role(name)

        index.append({**t, 'nbytes': nbytes, 'path': out_path,
                      'geo_key': geo_key, 'enc': enc, 'face': face,
                      'zone': zone, 'role': role})

        if (i+1) % 50 == 0 or (i+1) == len(tensors):
            dn = GGML_DTYPE_NAMES.get(t['dtype'], f'dtype_{t["dtype"]}')
            print(f'  [{i+1:3d}/{len(tensors)}] {name[:45]:45s} {dn:6s} zone={zone} role={role}')

# Write gsidx
with open(GSIDX_PATH, 'w') as f:
    json.dump(index, f, indent=2)
print(f'\ngsidx: {GSIDX_PATH} ({len(index)} entries)')

# Write gsdat
with open(GSDAT_PATH, 'wb') as f:
    for e in index:
        f.write(struct.pack('<QBBH',
            e['geo_key'] & 0xFFFFFFFFFFFFFFFF,
            e['zone'] & 0xFF,
            e['face'] & 0xFF,
            min(e['nbytes'] // 1024, 0xFFFF),
        ))
gsz = os.path.getsize(GSDAT_PATH)
print(f'gsdat: {GSDAT_PATH} ({gsz} bytes)')

total_bytes = sum(e['nbytes'] for e in index)
print(f'\nTotal: {len(index)} tensors, {total_bytes/1024**3:.3f} GB')

role_counts = {}
for e in index:
    r = e['role']
    role_counts[r] = role_counts.get(r, 0) + 1
print(f'Role distribution: {dict(sorted(role_counts.items()))}')

# Zone distribution
zone_counts = [0]*12
for e in index:
    z = e['zone']
    if z < len(zone_counts): zone_counts[z] += 1
print(f'Zone distribution:')
for z in range(12):
    if zone_counts[z] > 0:
        print(f'  z{z:2d}: {"█"*zone_counts[z]} {zone_counts[z]}')

print(f'\n✓ Qwen3-TTS store ready')
print(f'  blobs: {OUT_DIR}/')
print(f'  gsidx: {GSIDX_PATH}')
print(f'  gsdat: {GSDAT_PATH}')
