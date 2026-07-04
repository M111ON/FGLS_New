import struct, os, sys

# Read POGLS file data
f = open(sys.argv[1], 'rb')
sz = os.path.getsize(sys.argv[1])
hdr = f.read(48)
meta_off = struct.unpack('<Q', hdr[16:24])[0]
meta_cnt = struct.unpack('<I', hdr[24:28])[0]
model_meta_off = struct.unpack('<Q', hdr[32:40])[0]
model_meta_sz = struct.unpack('<I', hdr[40:44])[0]
ENTRY_SZ = struct.calcsize('IIIIII4I64s')  # PoglsTensorMeta (6*uint32 + 4*uint32 + char[64])
data_off = meta_off + meta_cnt * ENTRY_SZ + model_meta_sz
data_sz = sz - data_off

print(f'ENTRY_SZ={ENTRY_SZ}')
print(f'File size={sz}')
print(f'Meta count={meta_cnt}')
print(f'Data off={data_off} data_sz={data_sz}')

# Read all meta entries and compute cumsum
f.seek(meta_off)
for i in range(meta_cnt):
    e = f.read(ENTRY_SZ)
    addr, dtype, ndim, nbytes_orig, comp_type, comp_nbytes = struct.unpack('<IIIIII', e[0:24])
    name = e[40:].split(b'\x00')[0].decode() if len(e) > 40 else ''
    if i < 3 or i > meta_cnt - 3:
        esz = comp_nbytes if comp_nbytes > 0 else nbytes_orig
        print(f'  [{i}] addr={addr} name={name} nbytes={nbytes_orig} comp={comp_nbytes} esz={esz}')

# POGLS data at data_off
f.seek(data_off)
pogls_bytes = f.read(32)
print(f'POGLS first 32 bytes: {" ".join(f"{b:02x}" for b in pogls_bytes)}')

# Now check GGUF using C program approach
# First find where GGUF tensor data starts
gg = open(r'I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf', 'rb')
gg.seek(0, 2)
gguf_sz = gg.tell()
print(f'GGUF size={gguf_sz}')

# Read GGUF header
gg.seek(0)
magic = gg.read(4)
version = struct.unpack('<I', gg.read(4))[0]
n_tensors = struct.unpack('<Q', gg.read(8))[0]
n_kv = struct.unpack('<Q', gg.read(8))[0]
print(f'GGUF: magic={magic} version={version} tensors={n_tensors} kv={n_kv}')

# Quick skip: scan forward to find aligned data start
# After all tensor infos, data is at next 32-byte alignment
# KV pairs have variable size, tensor infos have fixed size
# Let's look for the output.weight tensor offset

# Read all tensor names and offsets
gg.seek(16)  # after header
tensor_offsets = {}
for _ in range(n_kv):
    klen = struct.unpack('<Q', gg.read(8))[0]
    if klen > 1000000:
        print(f'ERROR: bad klen={klen}')
        break
    gg.read(klen)
    vtype = struct.unpack('<I', gg.read(4))[0]
    if vtype == 8:  # string
        sl = struct.unpack('<Q', gg.read(8))[0]
        gg.read(sl)
    elif vtype == 9:  # array
        atype = struct.unpack('<I', gg.read(4))[0]
        alen = struct.unpack('<Q', gg.read(8))[0]
        if atype == 8:
            for _ in range(alen):
                sl = struct.unpack('<Q', gg.read(8))[0]
                gg.read(sl)
        elif atype in (0, 1, 7):
            gg.read(alen)
        elif atype in (2, 3):
            gg.read(alen * 2)
        elif atype in (4, 5, 6):
            gg.read(alen * 4)
        elif atype in (10, 11, 12):
            gg.read(alen * 8)
        else:
            gg.read(alen * 4)
    elif vtype in (0, 1):
        gg.read(1)
    elif vtype in (2, 3):
        gg.read(2)
    elif vtype in (4, 5, 6, 7):
        gg.read(4)
    elif vtype in (10, 11, 12):
        gg.read(8)
    else:
        gg.read(4)

# Now we're at tensor info section
print(f'Start of tensor info at: {gg.tell()}')
for i in range(n_tensors):
    klen = struct.unpack('<Q', gg.read(8))[0]
    name = gg.read(klen).decode()
    nd = struct.unpack('<I', gg.read(4))[0]
    dims = []
    for j in range(nd):
        dims.append(struct.unpack('<Q', gg.read(8))[0])
    dtype = struct.unpack('<I', gg.read(4))[0]
    offset = struct.unpack('<Q', gg.read(8))[0]
    tensor_offsets[name] = offset
    if i < 3 or i > n_tensors - 3:
        print(f'Tensor [{i}]: {name} dims={dims} dtype={dtype} offset={offset}')

pos_after = gg.tell()
aligned = (pos_after + 31) // 32 * 32
print(f'Pos after tensor info: {pos_after}, aligned: {aligned}')

# Now verify: first tensor's offset in GGUF should be aligned
first_tensor_name = list(tensor_offsets.keys())[0]
first_offset = tensor_offsets[first_tensor_name]
print(f'First tensor: {first_tensor_name} at offset {first_offset}')
print(f'Aligned (expected data start): {aligned}')

# Read first 32 bytes of GGUF data
gg.seek(aligned)
gguf_bytes = gg.read(32)
print(f'GGUF first 32 bytes (aligned={aligned}): {" ".join(f"{b:02x}" for b in gguf_bytes)}')

# Compare
match = pogls_bytes == gguf_bytes[:len(pogls_bytes)]
print(f'MATCH: {match}')
if match:
    print('SUCCESS! POGLS data matches GGUF exactly.')
else:
    print(f'MISMATCH! POGLS has offset issue of {aligned - data_off} bytes')
    
    # Try to find POGLS bytes in GGUF
    for off in range(max(0, aligned - 200), aligned + 200):
        gg.seek(off)
        test = gg.read(16)
        if test == pogls_bytes[:16]:
            print(f'POGLS data FOUND in GGUF at offset {off} (diff from aligned: {off - aligned})')
            break
    else:
        # Maybe it's a different tensor (output.weight vs token_embd.weight)
        # output.weight is the first tensor in the cumulative data stream
        output_off = tensor_offsets.get('output.weight')
        print(f'output.weight offset in GGUF: {output_off}')

f.close()
gg.close()
