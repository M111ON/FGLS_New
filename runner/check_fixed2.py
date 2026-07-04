import struct, os

# Check POGLS file
f = open('test_qwen_nocomp.pogls', 'rb')
sz = os.path.getsize('test_qwen_nocomp.pogls')

hdr = f.read(48)
meta_off = struct.unpack('<Q', hdr[16:24])[0]
meta_cnt = struct.unpack('<I', hdr[24:28])[0]
model_meta_off = struct.unpack('<Q', hdr[32:40])[0]
model_meta_sz = struct.unpack('<I', hdr[40:44])[0]
ENTRY_SZ = 104
data_off = meta_off + meta_cnt * ENTRY_SZ + model_meta_sz
data_sz = sz - data_off

print('File size:', sz)
print('Meta count:', meta_cnt)
print('Data off:', data_off, 'data_sz:', data_sz)

# First meta entry
f.seek(meta_off)
e = f.read(ENTRY_SZ)
name = e[40:104].split(b'\x00')[0].decode()
nbytes = struct.unpack('<I', e[12:16])[0]
print('First tensor:', name, 'nbytes:', nbytes)

# POGLS data at data_off
f.seek(data_off)
pogls_bytes = f.read(32)
print('POGLS first 32 bytes:', ' '.join(f'{b:02x}' for b in pogls_bytes))

# GGUF: skip header + KV + tensor info
with open(r'I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf', 'rb') as gg:
    gg.seek(0, 2)
    gguf_sz = gg.tell()
    gg.seek(16) # magic+version+n_tensors+n_kv

    for _ in range(26):
        klen = struct.unpack('<Q', gg.read(8))[0]
        gg.read(klen)
        vtype = struct.unpack('<I', gg.read(4))[0]
        if vtype <= 12:
            st = {0:1,1:1,2:2,3:2,4:4,5:4,6:4,7:1,8:-1,9:-1,10:8,11:8,12:8}
            sz = st.get(vtype, 4)
            if sz == -1: # arr/str
                if vtype == 8: # str
                    al = struct.unpack('<Q', gg.read(8))[0]
                    gg.read(al)
                else: # array
                    at = struct.unpack('<I', gg.read(4))[0]
                    al = struct.unpack('<Q', gg.read(8))[0]
            else:
                gg.read(sz)

    # Skip tensor infos
    for i in range(291):
        klen = struct.unpack('<Q', gg.read(8))[0]
        gg.read(klen)
        nd = struct.unpack('<I', gg.read(4))[0]
        for j in range(nd): gg.read(8)
        gg.read(4)
        gg.read(8)

    pos_after = gg.tell()
    aligned = (pos_after + 31) // 32 * 32
    gg.seek(aligned)
    gguf_bytes = gg.read(32)
    print('GGUF first 32 bytes:', ' '.join(f'{b:02x}' for b in gguf_bytes))

    match = pogls_bytes == gguf_bytes
    print('DATA MATCH:', match)
    if match:
        print('SUCCESS!')
    else:
        for off in range(max(0,aligned-100), aligned+100):
            gg.seek(off)
            test = gg.read(16)
            if test == pogls_bytes[:16]:
                print('POGLS data found in GGUF at offset', off)
                break

f.close()
