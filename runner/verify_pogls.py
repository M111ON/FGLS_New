"""Verify POGLS file structure by checking the actual data at key positions."""

import struct, os

ENTRY_SZ = 104

pogls_path = 'test_qwen_nocomp.pogls'
pogls_sz = os.path.getsize(pogls_path)
print(f'POGLS file size: {pogls_sz}')

with open(pogls_path, 'rb') as f:
    # Read header
    hdr = f.read(48)
    meta_off = struct.unpack('<Q', hdr[16:24])[0]
    meta_cnt = struct.unpack('<I', hdr[24:28])[0]
    model_meta_off = struct.unpack('<Q', hdr[32:40])[0]
    model_meta_sz = struct.unpack('<I', hdr[40:44])[0]
    data_off = meta_off + meta_cnt * ENTRY_SZ + model_meta_sz
    
    print(f'header: meta_off={meta_off} meta_cnt={meta_cnt}')
    print(f'model_meta_off={model_meta_off} model_meta_sz={model_meta_sz}')
    print(f'data_off={data_off}')
    
    # Read all meta entries and compute data_offs
    f.seek(meta_off)
    data_offs = []
    total_sz = 0
    for i in range(meta_cnt):
        entry = f.read(ENTRY_SZ)
        nbytes = struct.unpack('<I', entry[12:16])[0]
        comp_type = struct.unpack('<I', entry[16:20])[0]
        comp_nbytes = struct.unpack('<I', entry[20:24])[0]
        name = entry[40:104].split(b'\x00')[0].decode()
        sz = comp_nbytes if comp_nbytes > 0 else nbytes
        data_offs.append(total_sz)
        total_sz += sz
    
    # Check the actual data section size
    f.seek(data_off)
    data = f.read()
    actual_data_sz = len(data)
    print(f'\nTotal from meta: {total_sz}')
    print(f'Actual data section size: {actual_data_sz}')
    print(f'Difference: {total_sz - actual_data_sz}')
    
    # Find the LAST tensor with valid data
    for i in range(meta_cnt - 1, -1, -1):
        off = data_offs[i]
        sz = total_sz - off  # doesn't matter, we use it differently
    # Actually let's just find the boundary
    last_valid = -1
    for i in range(meta_cnt):
        off = data_offs[i]
        # What is this tensor's expected data size?
        entry_start = meta_off + i * ENTRY_SZ
        f.seek(entry_start + 12)  # nbytes_orig
        nbytes = struct.unpack('<I', f.read(4))[0]
        f.seek(entry_start + 16)
        comp_type = struct.unpack('<I', f.read(4))[0]
        f.seek(entry_start + 20)
        comp_nbytes = struct.unpack('<I', f.read(4))[0]
        sz = comp_nbytes if comp_nbytes > 0 else nbytes
        
        if off + sz <= actual_data_sz:
            last_valid = i
    
    print(f'\nLast valid tensor index: {last_valid} / {meta_cnt}')
    
    if last_valid < meta_cnt - 1:
        print(f'First overflow tensor: {last_valid + 1}')
        # Show overflow tensor info
        idx = last_valid + 1
        entry_start = meta_off + idx * ENTRY_SZ
        f.seek(entry_start)
        entry = f.read(ENTRY_SZ)
        name = entry[40:104].split(b'\x00')[0].decode()
        nbytes = struct.unpack('<I', entry[12:16])[0]
        comp_nbytes = struct.unpack('<I', entry[20:24])[0]
        sz = comp_nbytes if comp_nbytes > 0 else nbytes
        print(f'  name={name} nbytes={nbytes} comp_nbytes={comp_nbytes}')
        print(f'  data_off={data_offs[idx]} sz={sz}')
        print(f'  would read at file position {data_off + data_offs[idx]}')
        
        # What's ACTUALLY at that position?
        f.seek(data_off + data_offs[idx])
        actual_content = f.read(min(16, actual_data_sz - data_offs[idx]))
        print(f'  available: {len(actual_content)} bytes')
        print(f'  content: {" ".join(f"{b:02x}" for b in actual_content)}')
        
        # And what's at the end of the data section?
        f.seek(data_off + actual_data_sz - 16)
        print(f'  last 16 bytes of data: {" ".join(f"{b:02x}" for b in f.read(16))}')
        
        # Check model meta integrity  
        f.seek(model_meta_off)
        mmagic = f.read(4)
        print(f'\nmodel_meta starts with: {" ".join(f"{b:02x}" for b in mmagic)} = {"GGUF" if mmagic == b"GGUF" else "???"}')
        
        # Read end of model_meta
        f.seek(model_meta_off + model_meta_sz - 16)
        mm_end = f.read(16)
        print(f'model_meta last 16 bytes: {" ".join(f"{b:02x}" for b in mm_end)}')
        
        # What's immediately after model_meta?
        f.seek(model_meta_off + model_meta_sz)
        after_mm = f.read(min(32, actual_data_sz))
        print(f'after model_meta (first 32 bytes of data section):')
        print(f'  {" ".join(f"{b:02x}" for b in after_mm)}')
        
        # Now check the first tensor's actual data
        f.seek(data_off)
        first_tensor = f.read(min(32, actual_data_sz))
        print(f'\nfirst tensor at data_off:')
        print(f'  {" ".join(f"{b:02x}" for b in first_tensor)}')
        
        # Compare: the first 32 bytes should match the GGUF's first tensor data
        # Let's read from GGUF
        print(f'\nComparing with GGUF...')
        import struct as s
        with open(r'I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf', 'rb') as gg:
            gg.read(4); gg.read(4); gg.read(8); gg.read(8)  # skip header
            # Skip KV
            for j in range(26):
                klen = s.unpack('<Q', gg.read(8))[0]
                gg.read(klen)
                # Using gguf_index.h type mapping
                vtype = s.unpack('<I', gg.read(4))[0]
                if vtype in [0,1]: gg.read(1)
                elif vtype in [2,3]: gg.read(2)
                elif vtype in [4,5,6]: gg.read(4)
                elif vtype == 7: gg.read(1)
                elif vtype == 8:
                    sl = s.unpack('<Q', gg.read(8))[0]; gg.read(sl)
                elif vtype == 9:
                    atype = s.unpack('<I', gg.read(4))[0]
                    alen = s.unpack('<Q', gg.read(8))[0]
                    if atype in [0,1]: gg.read(alen)
                    elif atype in [2,3]: gg.read(alen*2)
                    elif atype in [4,5,6]: gg.read(alen*4)
                    elif atype == 7: gg.read(alen)
                    elif atype in [10,11,12]: gg.read(alen*8)
                    elif atype == 8:
                        for _ in range(alen):
                            slen = s.unpack('<Q', gg.read(8))[0]; gg.read(slen)
                    else: gg.read(alen*4)
                elif vtype in [10,11,12]: gg.read(8)
                else: print(f'  unknown vtype {vtype}')
            
            # Read first tensor info
            klen = s.unpack('<Q', gg.read(8))[0]
            gg.read(klen)
            nd = s.unpack('<I', gg.read(4))[0]
            ne = 1
            for _ in range(nd):
                ndim = s.unpack('<Q', gg.read(8))[0]; ne *= ndim
            dt = s.unpack('<I', gg.read(4))[0]
            off = s.unpack('<Q', gg.read(8))[0]
            
            # Skip remaining tensor infos
            for _ in range(1, 291):
                klen = s.unpack('<Q', gg.read(8))[0]
                gg.read(klen)
                nd = s.unpack('<I', gg.read(4))[0]
                ne2 = 1
                for _ in range(nd):
                    ndim = s.unpack('<Q', gg.read(8))[0]; ne2 *= ndim
                dt2 = s.unpack('<I', gg.read(4))[0]
                off2 = s.unpack('<Q', gg.read(8))[0]
            
            # alignment
            pos = gg.tell()
            aligned = (pos + 31) // 32 * 32
            gg.seek(aligned)
            
            # First tensor data
            first_gguf = gg.read(32)
            print(f'GGUF first tensor data: {" ".join(f"{b:02x}" for b in first_gguf)}')
            
            # Second tensor
            gg.read(144643072 - 32)
            # Actually let's check the LAST tensor too
            pass
