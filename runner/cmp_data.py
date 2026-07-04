import struct, os

# Read POGLS data section
pogls = open('test_qwen_nocomp.pogls', 'rb')
hdr = pogls.read(48)
meta_off = struct.unpack('<Q', hdr[16:24])[0]
meta_cnt = struct.unpack('<I', hdr[24:28])[0]
model_meta_off = struct.unpack('<Q', hdr[32:40])[0]
model_meta_sz = struct.unpack('<I', hdr[40:44])[0]
ENTRY_SZ = 104
data_off = meta_off + meta_cnt * ENTRY_SZ + model_meta_sz

# Read first 512 bytes of data
pogls.seek(data_off)
pogls_data = pogls.read(2048)

# Read GGUF tensor data start
gguf = open(r'I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf', 'rb')
gguf.seek(0, 2)
gguf_sz = gguf.tell()
gguf.seek(0)

# Parse GGUF header
magic = gguf.read(4)
version = struct.unpack('<I', gguf.read(4))[0]
n_tensors = struct.unpack('<Q', gguf.read(8))[0]
n_kv = struct.unpack('<Q', gguf.read(8))[0]

print(f'GGUF: n_tensors={n_tensors} n_kv={n_kv}')

# Skip KV pairs
for _ in range(n_kv):
    klen = struct.unpack('<Q', gguf.read(8))[0]
    key = gguf.read(klen).decode()
    vtype = struct.unpack('<I', gguf.read(4))[0]
    if vtype == 8:  # string
        slen = struct.unpack('<Q', gguf.read(8))[0]
        val = gguf.read(slen)
    elif vtype == 9:  # array
        atype = struct.unpack('<I', gguf.read(4))[0]
        alen = struct.unpack('<Q', gguf.read(8))[0]
        if atype == 8:  # string array
            for _ in range(alen):
                slen = struct.unpack('<Q', gguf.read(8))[0]
                gguf.read(slen)
        elif atype <= 12:
            esize = {0:1, 1:1, 2:2, 3:2, 4:4, 5:4, 6:4, 7:1, 8:-1, 9:-1, 10:8, 11:8, 12:8}[atype]
            gguf.read(esize * alen)
    elif vtype <= 12:
        sizes = {0:1, 1:1, 2:2, 3:2, 4:4, 5:4, 6:4, 7:1, 10:8, 11:8, 12:8}
        gguf.read(sizes.get(vtype, 4))

# Skip tensor infos
for i in range(n_tensors):
    klen = struct.unpack('<Q', gguf.read(8))[0]
    gguf.read(klen)
    nd = struct.unpack('<I', gguf.read(4))[0]
    for j in range(nd):
        gguf.read(8)  # dim
    gguf.read(4)  # dtype
    gguf.read(8)  # offset

# Align to 32 bytes
pos = gguf.tell()
aligned = (pos + 31) // 32 * 32
gguf.seek(aligned)

# Read first 2048 bytes of GGUF tensor data
gguf_data = gguf.read(2048)

print(f'\nGGUF data starts at file offset: {aligned}')
print(f'POGLS data starts at file offset: {data_off}')

# Compare
match = pogls_data == gguf_data
print(f'First 2048 bytes match: {match}')

if not match:
    # Show differences
    for i in range(min(len(pogls_data), len(gguf_data))):
        if pogls_data[i] != gguf_data[i]:
            print(f'  First diff at byte {i}: POGLS=0x{pogls_data[i]:02x} GGUF=0x{gguf_data[i]:02x}')
            if i < 256:
                print(f'    Context: POGLS={pogls_data[max(0,i-8):i+8].hex()} GGUF={gguf_data[max(0,i-8):i+8].hex()}')
            break
    print(f'POGLS data length: {len(pogls_data)}')
    print(f'GGUF data length:  {len(gguf_data)}')
else:
    print('SUCCESS — data matches!')

pogls.close()
gguf.close()
