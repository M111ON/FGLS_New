import struct

# Check GGUF tensor offsets vs cumsum
gguf = open(r'I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf', 'rb')
gguf.seek(0, 2)
gguf_sz = gguf.tell()
gguf.seek(0)

# Parse GGUF header
gguf.read(4)  # magic
version = struct.unpack('<I', gguf.read(4))[0]
n_tensors = struct.unpack('<Q', gguf.read(8))[0]
n_kv = struct.unpack('<Q', gguf.read(8))[0]

print(f'GGUF: version={version} n_tensors={n_tensors} n_kv={n_kv}')

# Skip KV pairs
for _ in range(n_kv):
    klen = struct.unpack('<Q', gguf.read(8))[0]
    gguf.read(klen)
    vtype = struct.unpack('<I', gguf.read(4))[0]
    if vtype == 8:
        slen = struct.unpack('<Q', gguf.read(8))[0]
        gguf.read(slen)
    elif vtype == 9:
        atype = struct.unpack('<I', gguf.read(4))[0]
        alen = struct.unpack('<Q', gguf.read(8))[0]
        if atype == 8:
            for _ in range(alen):
                slen = struct.unpack('<Q', gguf.read(8))[0]
                gguf.read(slen)
        else:
            esize = {0:1,1:1,2:2,3:2,4:4,5:4,6:4,7:1,10:8,11:8,12:8}[atype]
            gguf.read(esize * alen)
    else:
        sizes = {0:1,1:1,2:2,3:2,4:4,5:4,6:4,7:1,10:8,11:8,12:8}
        gguf.read(sizes.get(vtype, 4))

# Print first/last tensor info offsets
for i in range(n_tensors):
    klen = struct.unpack('<Q', gguf.read(8))[0]
    name = gguf.read(klen).decode()
    nd = struct.unpack('<I', gguf.read(4))[0]
    dims = [struct.unpack('<Q', gguf.read(8))[0] for _ in range(nd)]
    dtype = struct.unpack('<I', gguf.read(4))[0]
    offset = struct.unpack('<Q', gguf.read(8))[0]
    if i < 5 or i > n_tensors - 5:
        print(f'  [{i}] {name} offset={offset} dims={dims} dtype={dtype}')

# Data start = aligned position after tensor infos
pos = gguf.tell()
aligned = (pos + 31) // 32 * 32
print(f'Tensor info end at {pos}, aligned data start at {aligned}')

# Also compute expected cumsum from POGLS
ENTRY_SZ = 104
pogls = open('test_qwen_nocomp.pogls', 'rb')
hdr = pogls.read(48)
meta_off = struct.unpack('<Q', hdr[16:24])[0]
meta_cnt = struct.unpack('<I', hdr[24:28])[0]
pogls.seek(meta_off)

cumsum = 0
for i in range(meta_cnt):
    e = pogls.read(ENTRY_SZ)
    nbytes = struct.unpack('<I', e[12:16])[0]
    comp_nbytes = struct.unpack('<I', e[20:24])[0]
    step = comp_nbytes if comp_nbytes > 0 else nbytes
    cumsum += step
    name = e[40:104].split(b'\x00')[0].decode()

print(f'\nPOGLS cumsum: {cumsum}')
print(f'GGUF data section: {gguf_sz} - {aligned} = {gguf_sz - aligned}')
print(f'GGUF cumsum from tensor offsets: {cumsum}')

# Verify first tensor: offset=0 in GGUF?
print(f'\nFirst tensor data in GGUF at: {aligned}')
print(f'First tensor offset from info: {0 if n_tensors > 0 else "N/A"}')
print(f'Match: {aligned == (aligned + 0)}')

# Check: does GGUF offset 0 point to the data start?
gguf.seek(aligned)
gguf_first = gguf.read(32)
pogls_data_off = 6309912
pogls.seek(pogls_data_off)
pogls_first = pogls.read(32)
print(f'\nGGUF first data: {gguf_first.hex()[:64]}')
print(f'POGLS first data: {pogls_first.hex()[:64]}')
print(f'Match: {gguf_first == pogls_first}')

gguf.close()
pogls.close()
