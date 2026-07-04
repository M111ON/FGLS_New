import struct

f = open(r'I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf', 'rb')
data = f.read(1000)

# Header
magic = struct.unpack('<I', data[0:4])[0]
ver = struct.unpack('<I', data[4:8])[0]
nt = struct.unpack('<Q', data[8:16])[0]
nk = struct.unpack('<Q', data[16:24])[0]
print(f'magic=0x{magic:08x} ver={ver} nt={nt} nk={nk}')

pos = 24
for i in range(nk):
    klen = struct.unpack('<Q', data[pos:pos+8])[0]; pos += 8
    key = data[pos:pos+klen].decode(); pos += klen
    ktype = struct.unpack('<I', data[pos:pos+4])[0]; pos += 4
    
    if ktype == 6:  # string
        slen = struct.unpack('<Q', data[pos:pos+8])[0]; pos += 8
        sval = data[pos:pos+slen].decode() if slen < 100 else f'<{slen} bytes>'
        pos += slen
        print(f'  [{i}] {key} (string) = {sval}')
    elif ktype == 7:  # array
        arr_type = struct.unpack('<I', data[pos:pos+4])[0]; pos += 4
        arr_len = struct.unpack('<Q', data[pos:pos+8])[0]; pos += 8
        if arr_type == 6:  # string array
            print(f'  [{i}] {key} (string[{arr_len}])')
            pos += arr_len * 16  # rough estimate
        else:
            print(f'  [{i}] {key} (array type={arr_type} len={arr_len})')
            pos += arr_len * 4
    elif ktype == 4:  # float32
        val = struct.unpack('<f', data[pos:pos+4])[0]; pos += 4
        print(f'  [{i}] {key} (float32) = {val}')
    elif ktype == 0:  # uint8
        val = data[pos]; pos += 1
        print(f'  [{i}] {key} (uint8) = {val}')
    elif ktype == 1:  # int8
        val = struct.unpack('<b', data[pos:pos+1])[0]; pos += 1
        print(f'  [{i}] {key} (int8) = {val}')
    elif ktype == 3:  # int32
        val = struct.unpack('<I', data[pos:pos+4])[0]; pos += 4
        print(f'  [{i}] {key} (int32) = {val}')
    elif ktype == 5:  # bool
        val = data[pos]; pos += 1
        print(f'  [{i}] {key} (bool) = {val}')
    else:
        print(f'  [{i}] {key} (type={ktype}) UNKNOWN')
        break

print(f'\nReading complete. pos_after_kv = {pos}')
print(f'Remaining: {len(data) - pos} bytes read')

# Now compute tensor info sizes
if pos < 1000:
    ts_map = {0:4, 1:2, 2:18, 3:20, 6:22, 7:24, 8:34, 9:36}
    bs_map = {0:1, 1:1, 2:32, 3:32, 6:32, 7:32, 8:32, 9:32}
    
    f.seek(pos)
    cumsum = 0
    for i in range(min(291, 100)):
        klen = struct.unpack('<Q', f.read(8))[0]
        f.read(klen)
        nd = struct.unpack('<I', f.read(4))[0]
        ne = 1
        for j in range(nd):
            ndim = struct.unpack('<Q', f.read(8))[0]; ne *= ndim
        dt = struct.unpack('<I', f.read(4))[0]
        off = struct.unpack('<Q', f.read(8))[0]
        ts = ts_map.get(dt, 4)
        bs = bs_map.get(dt, 1)
        sz = (ne // bs) * ts
        cumsum += sz
    
    f.seek(0, 2)
    file_sz = f.tell()
    print(f'cumsum (first 100 tensors) = {cumsum}')
    print(f'file_size = {file_sz}')

f.close()
