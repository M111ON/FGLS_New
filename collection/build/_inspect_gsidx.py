import struct, os

p = r'I:\FGLS_new\collection\build\qwen_geom_v3.gsidx'
with open(p, 'rb') as f:
    data = f.read()

magic = data[:8]
print(f'magic: {magic}')
print(f'len: {len(data)}')

# Try format: magic(8) + ver(4) + n_keys(8) + n_rows(8) + entries
pos = 8
ver = struct.unpack_from('<I', data, pos)[0]
pos += 4
print(f'version: {ver:08x} ({ver})')

n_keys = struct.unpack_from('<Q', data, pos)[0]
pos += 8
n_rows = struct.unpack_from('<Q', data, pos)[0]
pos += 8
print(f'n_keys={n_keys} n_rows={n_rows}')

while pos < len(data):
    name_len = data[pos]
    pos += 1
    if pos + name_len > len(data):
        break
    name = data[pos:pos+name_len].decode('ascii', errors='replace')
    pos += name_len
    if pos + 14 > len(data):
        break
    zone = data[pos]
    shape = data[pos+1]
    offset = struct.unpack_from('<Q', data, pos+2)[0]
    length = struct.unpack_from('<I', data, pos+10)[0]
    pos += 14
    print(f'  name={name:20s} zone={zone:2d} shape={chr(shape)} off={offset:>12d} len={length:>8d}')

print(f'bytes remaining: {len(data) - pos}')
