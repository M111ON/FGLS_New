import struct, os

# Check POGLS file
with open('test_qwen_nocomp.pogls', 'rb') as f:
    pogls_sz = os.path.getsize('test_qwen_nocomp.pogls')
    
    # Read header
    hdr = f.read(48)
    meta_off = struct.unpack('<Q', hdr[16:24])[0]
    meta_cnt = struct.unpack('<I', hdr[24:28])[0]
    model_meta_off = struct.unpack('<Q', hdr[32:40])[0]
    model_meta_sz = struct.unpack('<I', hdr[40:44])[0]
    ENTRY_SZ = 104
    data_off = meta_off + meta_cnt * ENTRY_SZ + model_meta_sz
    data_sz = pogls_sz - data_off
    
    print(f'POGLS: meta_off={meta_off} meta_cnt={meta_cnt}')
    print(f'model_meta_off={model_meta_off} model_meta_sz={model_meta_sz}')
    print(f'data_off={data_off} data_sz={data_sz}')
    
    # Read meta and compute cumsum
    f.seek(meta_off)
    cumsum = 0
    first_name = None
    for i in range(meta_cnt):
        entry = f.read(ENTRY_SZ)
        nbytes = struct.unpack('<I', entry[12:16])[0]
        comp_type = struct.unpack('<I', entry[16:20])[0]
        comp_nbytes = struct.unpack('<I', entry[20:24])[0]
        name = entry[40:104].split(b'\x00')[0].decode()
        sz = comp_nbytes if comp_nbytes > 0 else nbytes
        cumsum += sz
        if i == 0:
            first_name = name
    
    print(f'cumsum={cumsum}')
    print(f'data_sz == cumsum: {data_sz == cumsum}')
    print(f'first tensor: {first_name}')
    
    # Read first tensor data from POGLS
    f.seek(data_off)
    pogls_first = f.read(16)
    print(f'POGLS first data: {" ".join(f"{b:02x}" for b in pogls_first)}')
    
    # Compare with GGUF
    with open(r'I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf', 'rb') as gg:
        gg.seek(0, 2)
        gguf_sz = gg.tell()
        gg.seek(0)
        gg.read(4); gg.read(4); gg.read(8); gg.read(8)
        # Skip KV
        for _ in range(26):
            klen = struct.unpack('<Q', gg.read(8))[0]
            gg.read(klen)
            vtype = struct.unpack('<I', gg.read(4))[0]
            if vtype in [0,1]: gg.read(1)
            elif vtype in [2,3]: gg.read(2)
            elif vtype in [4,5,6]: gg.read(4)
            elif vtype == 7: gg.read(1)
            elif vtype == 8:
                sl = struct.unpack('<Q', gg.read(8))[0]; gg.read(sl)
            elif vtype == 9:
                atype = struct.unpack('<I', gg.read(4))[0]
                alen = struct.unpack('<Q', gg.read(8))[0]
                if atype in [0,1]: gg.read(alen)
                elif atype in [2,3]: gg.read(alen*2)
                elif atype in [4,5,6]: gg.read(alen*4)
                elif atype == 7: gg.read(alen)
                elif atype in [10,11,12]: gg.read(alen*8)
                elif atype == 8:
                    for _ in range(alen):
                        slen = struct.unpack('<Q', gg.read(8))[0]; gg.read(slen)
                else: gg.read(alen*4)
            elif vtype in [10,11,12]: gg.read(8)
        # Read first tensor info
        gg.read(8); gg.read(struct.unpack('<Q', gg.read(8))[0])  # klen + name (already consumed klen)
        # Actually re-read properly
        gg.seek(5931189)
        # Read all 291 tensor infos
        for i in range(291):
            klen = struct.unpack('<Q', gg.read(8))[0]
            gg.read(klen)
            nd = struct.unpack('<I', gg.read(4))[0]
            for j in range(nd): gg.read(8)
            dt = struct.unpack('<I', gg.read(4))[0]
            off = struct.unpack('<Q', gg.read(8))[0]
        pos_after = gg.tell()
        aligned = (pos_after + 31) // 32 * 32
        gg.seek(aligned)
        gguf_first = gg.read(16)
        print(f'GGUF  first data: {" ".join(f"{b:02x}" for b in gguf_first)}')
    
    # Compare
    match = pogls_first == gguf_first
    print(f'\nMATCH: {match}')
    print('SUCCESS!' if match else 'MISMATCH!')
    
    if not match:
        # Try to find POGLS data in GGUF
        gg = open(r'I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf', 'rb')
        target = pogls_first
        for off in range(aligned - 100, aligned + 100):
            gg.seek(off)
            test = gg.read(16)
            if test == target:
                print(f'POGLS data found in GGUF at offset {off}')
                break
        gg.close()
