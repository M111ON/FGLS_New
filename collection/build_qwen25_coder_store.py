"""
build_qwen25_coder_store.py — Build geometry store for Qwen2.5-Coder-1.5B
Extracts tensors directly from local GGUF file.
"""
import os, struct, json, hashlib, argparse

p = argparse.ArgumentParser()
p.add_argument("--gguf", default="qwen2.5-coder-1.5b-instruct.F16.gguf", help="GGUF model path")
p.add_argument("--out", default="build/", help="output directory")
p.add_argument("--model-name", default="qwen25-coder-3b", help="model identifier for geo_key")
args = p.parse_args()

GGUF_PATH  = args.gguf
OUT_DIR    = os.path.join(args.out, "qwen25_coder_tensors_raw")
GSIDX_PATH = os.path.join(args.out, "qwen25_coder_geom.gsidx")
GSDAT_PATH = os.path.join(args.out, "qwen25_coder_geom.gsdat")

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

with open(GGUF_PATH, 'rb') as f:
    magic = f.read(4)
    assert magic == b'GGUF', f'bad magic'
    version = struct.unpack('<I', f.read(4))[0]
    n_tensors = struct.unpack('<Q', f.read(8))[0]
    n_kv = struct.unpack('<Q', f.read(8))[0]
    print(f'GGUF v{version}, {n_tensors} tensors, {n_kv} KV entries')

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
        elif vtype in (0,7):
            f.read(1)
        elif vtype in (4,5,6):
            f.read(4)
        elif vtype in (10,11):
            f.read(8)
        elif vtype == 12:
            f.read(8)
        else:
            raise ValueError(f'unknown vtype {vtype} at KV {i}')

    data_start = f.tell()
    print(f'KV data ends at: {data_start}')

    tensors = []
    for i in range(n_tensors):
        nlen = struct.unpack('<Q', f.read(8))[0]
        tname = f.read(nlen).decode('utf-8', errors='replace')
        ndims = struct.unpack('<I', f.read(4))[0]
        shape = [struct.unpack('<Q', f.read(8))[0] for _ in range(ndims)]
        dtype = struct.unpack('<I', f.read(4))[0]
        offset = struct.unpack('<Q', f.read(8))[0]
        tensors.append({'name': tname, 'shape': shape, 'dtype': dtype, 'offset': offset})

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

    tensor_info_end = f.tell()

print(f'Tensor info: {len(tensors)} tensors')
for t in tensors[:5]:
    dtn = GGML_DTYPE_NAMES.get(t['dtype'], f'dtype_{t["dtype"]}')
    print(f'  {t["name"]:40s} shape={str(t["shape"]):20s} {dtn:6s} nbytes={t["nbytes"]:,}')

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
        f.seek(file_offset)
        with open(out_path, 'wb') as out:
            remaining = nbytes
            while remaining > 0:
                chunk = f.read(min(4*1024*1024, remaining))
                if not chunk: break
                out.write(chunk)
                remaining -= len(chunk)
        geo = tensor_geo(name, t['shape'], args.model_name)
        index.append({**t, 'nbytes': nbytes, 'path': out_path, **geo})
        if (i+1) % 50 == 0 or (i+1) == len(tensors):
            dtn = GGML_DTYPE_NAMES.get(t['dtype'], f'dtype_{t["dtype"]}')
            print(f'  [{i+1:3d}/{len(tensors)}] {name} -> {dtn} {nbytes//1024}KB zone={geo["zone"]}')

with open(GSIDX_PATH, 'w') as f:
    json.dump(index, f, indent=2)
print(f'\ngsidx: {GSIDX_PATH} ({len(index)} entries)')

with open(GSDAT_PATH, 'wb') as f:
    for e in index:
        f.write(struct.pack('<QBBH',
            e['geo_key'] & 0xFFFFFFFFFFFFFFFF,
            e['zone'] & 0xFF,
            e['face'] & 0xFF,
            min(e['nbytes'] // 1024, 0xFFFF),
        ))
gsz = os.path.getsize(GSDAT_PATH)
print(f'gsdat: {GSDAT_PATH} ({gsz} bytes, {len(index)} recs x {gsz//len(index)}B)')

total_bytes = sum(e['nbytes'] for e in index)
print(f'\nTotal: {len(index)} tensors, {total_bytes/1024**3:.2f} GB')
face_count = {}
for e in index:
    face_count[e['face']] = face_count.get(e['face'], 0) + 1
print('Face dist:')
for face in sorted(face_count):
    print(f'  face {face:2d} {"#"*face_count[face]} {face_count[face]}')
print('\nDone')
