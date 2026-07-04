import struct

f = open(r'I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf', 'rb')
magic = struct.unpack('<I', f.read(4))[0]
ver = struct.unpack('<I', f.read(4))[0]
n_tensors = struct.unpack('<Q', f.read(8))[0]
n_kv = struct.unpack('<Q', f.read(8))[0]
print(f'GGUF: nt={n_tensors} nk={n_kv}')

# Skip KV pairs using the gguf_index.h type mapping
for i in range(n_kv):
    klen = struct.unpack('<Q', f.read(8))[0]
    f.read(klen)
    vtype = struct.unpack('<I', f.read(4))[0]
    if vtype in [0, 1]: f.read(1)
    elif vtype in [2, 3]: f.read(2)
    elif vtype in [4, 5, 6]: f.read(4)
    elif vtype == 7: f.read(1)
    elif vtype == 8:
        sl = struct.unpack('<Q', f.read(8))[0]; f.read(sl)
    elif vtype == 9:
        atype = struct.unpack('<I', f.read(4))[0]
        alen = struct.unpack('<Q', f.read(8))[0]
        if atype in [0, 1]: f.read(alen)
        elif atype in [2, 3]: f.read(alen * 2)
        elif atype in [4, 5, 6]: f.read(alen * 4)
        elif atype == 7: f.read(alen)
        elif atype in [10, 11, 12]: f.read(alen * 8)
        elif atype == 8:
            for j in range(alen):
                slen = struct.unpack('<Q', f.read(8))[0]; f.read(slen)
        else:
            f.read(alen * 4)
    elif vtype in [10, 11, 12]: f.read(8)
    else:
        print(f'UNKNOWN vtype {vtype} at KV[{i}] key_was={f.tell()-12-klen}')

pos_kv = f.tell()
print(f'pos_after_kv = {pos_kv}')

# Read tensor info
offsets = {}
ts_map = {0:4, 1:2, 2:18, 3:20, 6:22, 7:24, 8:34, 9:36}
bs_map = {0:1, 1:1, 2:32, 3:32, 6:32, 7:32, 8:32, 9:32}
cumsum = 0

for i in range(n_tensors):
    klen = struct.unpack('<Q', f.read(8))[0]
    name = f.read(klen).decode()
    nd = struct.unpack('<I', f.read(4))[0]
    ne = 1
    for j in range(nd):
        ndim = struct.unpack('<Q', f.read(8))[0]; ne *= ndim
    dt = struct.unpack('<I', f.read(4))[0]
    off = struct.unpack('<Q', f.read(8))[0]
    
    # Count offsets
    if off not in offsets:
        offsets[off] = {'size': 0, 'names': []}
    offsets[off]['names'].append(name)
    
    ts = ts_map.get(dt, 4)
    bs = bs_map.get(dt, 1)
    sz = (ne // bs) * ts
    cumsum += sz
    offsets[off]['size'] = sz

pos_tensors = f.tell()
aligned = (pos_tensors + 31) // 32 * 32
f.seek(0, 2)
file_sz = f.tell()
f.close()

print(f'pos_after_tensors = {pos_tensors}')
print(f'aligned_data_off = {aligned}')
print(f'file_size = {file_sz}')
print(f'data_section = {file_sz - aligned}')
print(f'cumsum = {cumsum}')
print(f'diff = {cumsum - (file_sz - aligned)}')
print(f'unique_offsets = {len(offsets)} / {n_tensors}')

# List shared tensors
shared = {k:v for k,v in offsets.items() if len(v['names']) > 1}
print(f'shared_offsets = {len(shared)}')
for off, info in shared.items():
    print(f'  off={off} size={info["size"]} names={info["names"]}')
