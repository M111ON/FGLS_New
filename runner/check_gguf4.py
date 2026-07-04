import struct

f = open(r'I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf', 'rb')

# Skip header
f.read(4)  # magic
f.read(4)  # version
n_tensors = struct.unpack('<Q', f.read(8))[0]
n_kv = struct.unpack('<Q', f.read(8))[0]
print(f'nt={n_tensors} nk={n_kv}')

# Skip KV
for i in range(n_kv):
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
    else: print(f'  unknown vtype {vtype}')

pos_kv = f.tell()
print(f'pos_after_kv = {pos_kv}')

# Read ALL tensor info entries
total_size = 0
prev_pos = pos_kv
name_lengths = []
for i in range(n_tensors):
    start_pos = f.tell()
    klen_bytes = f.read(8)
    klen = struct.unpack('<Q', klen_bytes)[0]
    name = f.read(klen)
    nd = struct.unpack('<I', f.read(4))[0]
    for j in range(nd):
        f.read(8)
    dt = struct.unpack('<I', f.read(4))[0]
    off = struct.unpack('<Q', f.read(8))[0]
    entry_sz = f.tell() - prev_pos
    name_lengths.append(len(name))
    total_size += entry_sz
    entry_bytes = f.tell() - start_pos
    if i < 3 or i == n_tensors - 1:
        print(f'  [{i}] name={name} nd={nd} entry={entry_bytes} klen={klen}')
    prev_pos = f.tell()

pos_after_tensors = f.tell()
print(f'\npos_after_tensors = {pos_after_tensors}')
print(f'total tensor info bytes = {pos_after_tensors - pos_kv}')
print(f'sum of name lengths = {sum(name_lengths)}')
print(f'avg name length = {sum(name_lengths)/len(name_lengths):.2f}')
print(f'max name length = {max(name_lengths)}')
print(f'min name length = {min(name_lengths)}')

# Check: what would the C code's pos_after_tensors be?
# The C loop reads: 8(klen) + klen(name) + 4(ndim) + nd*8(dims) + 4(dtype) + 8(offset)
# = 24 + klen + nd*8

expected = pos_kv
for i in range(n_tensors):
    # Skip back and read each entry properly
    pass  # Already computed above

# Let me compute what the C code would compute (assuming the SAME positions)
print(f'\nIf C code also has pos_after_tensors = {pos_after_tensors}:')
aligned = (pos_after_tensors + 31) // 32 * 32
print(f'  aligned = {aligned}')
print(f'  data_sec_off should be = {aligned}')

# But if we align DOWN instead of UP:
aligned_down = pos_after_tensors // 32 * 32
print(f'  aligned DOWN = {aligned_down}')
diff = pos_after_tensors - aligned_down
print(f'  offset from aligned down = {diff}')

f.close()
