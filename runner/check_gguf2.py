import struct

f = open(r'I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf', 'rb')
magic = struct.unpack('<I', f.read(4))[0]
ver = struct.unpack('<I', f.read(4))[0]
nt = struct.unpack('<Q', f.read(8))[0]
nk = struct.unpack('<Q', f.read(8))[0]
print(f'magic=0x{magic:08x} ver={ver} nt={nt} nk={nk}')

# Skip KV
for i in range(nk):
    klen = struct.unpack('<Q', f.read(8))[0]
    f.read(klen)
    vtype = struct.unpack('<I', f.read(4))[0]
    if vtype in [0,1]: f.read(1)
    elif vtype in [2,3]: f.read(2)
    elif vtype in [4,5,6]: f.read(4)
    elif vtype == 7: f.read(1)
    elif vtype == 8:
        sl = struct.unpack('<Q', f.read(8))[0]; f.read(sl)
    elif vtype == 9:
        atype = struct.unpack('<I', f.read(4))[0]
        alen = struct.unpack('<Q', f.read(8))[0]
        if atype in [0,1]: f.read(alen)
        elif atype in [2,3]: f.read(alen*2)
        elif atype in [4,5,6]: f.read(alen*4)
        elif atype == 7: f.read(alen)
        elif atype in [10,11,12]: f.read(alen*8)
        elif atype == 8:
            for _ in range(alen):
                slen = struct.unpack('<Q', f.read(8))[0]; f.read(slen)
        else: f.read(alen*4)
    elif vtype in [10,11,12]: f.read(8)
    else: print(f'unknown vtype {vtype}')
    
print(f'pos after KV: {f.tell()}')

# Read first tensor info
klen = struct.unpack('<Q', f.read(8))[0]
name = f.read(klen)
nd = struct.unpack('<I', f.read(4))[0]
ne = 1
for j in range(nd):
    ndim = struct.unpack('<Q', f.read(8))[0]; ne *= ndim
dtype = struct.unpack('<I', f.read(4))[0]
offset = struct.unpack('<Q', f.read(8))[0]
print(f'first tensor: name={name} nd={nd} ne={ne} dtype={dtype} offset={offset}')

# Skip remaining tensor infos
for i in range(1, nt):
    klen = struct.unpack('<Q', f.read(8))[0]
    f.read(klen)
    nd = struct.unpack('<I', f.read(4))[0]
    ne2 = 1
    for j in range(nd):
        ndim = struct.unpack('<Q', f.read(8))[0]; ne2 *= ndim
    dt2 = struct.unpack('<I', f.read(4))[0]
    off2 = struct.unpack('<Q', f.read(8))[0]

pos_after = f.tell()
aligned = (pos_after + 31) // 32 * 32
f.seek(0, 2)
file_sz = f.tell()
print(f'pos_after_tensors = {pos_after}')
print(f'aligned = {aligned}')
print(f'file_size = {file_sz}')
print(f'data_section = {file_sz - aligned}')
print(f'first tensor offset in file: {aligned + offset} (relative) vs {offset} (absolute)')

# Read first tensor data at different offsets
f.seek(aligned + offset)
data1 = f.read(16)
print(f'data at aligned+offset: {" ".join(f"{b:02x}" for b in data1)}')

f.seek(offset)
data2 = f.read(16)
print(f'data at offset (absolute): {" ".join(f"{b:02x}" for b in data2)}')

f.seek(aligned)
data3 = f.read(16)
print(f'data at aligned alone: {" ".join(f"{b:02x}" for b in data3)}')

f.close()
