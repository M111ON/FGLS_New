import struct

# POGLS header layout (PoglsStoreHeader):
# offset 0: magic (u32)
# offset 4: version (u32)
# offset 8: n_tensors (u32)
# offset 12: flags (u32)
# offset 16: tensor_meta_off (u64)
# offset 24: tensor_meta_count (u32)
# offset 28: _pad0 (u32)
# offset 32: model_meta_off (u64)
# offset 40: model_meta_sz (u32)

with open('test_qwen_nocomp.pogls', 'rb') as f:
    f.seek(0, 2)
    pogls_sz = f.tell()
    f.seek(0)
    
    hdr = f.read(48)
    magic = struct.unpack('<I', hdr[0:4])[0]
    ver = struct.unpack('<I', hdr[4:8])[0]
    nt = struct.unpack('<I', hdr[8:12])[0]
    flags = struct.unpack('<I', hdr[12:16])[0]
    meta_off = struct.unpack('<Q', hdr[16:24])[0]
    meta_cnt = struct.unpack('<I', hdr[24:28])[0]
    pad0 = struct.unpack('<I', hdr[28:32])[0]
    model_meta_off = struct.unpack('<Q', hdr[32:40])[0]
    model_meta_sz = struct.unpack('<I', hdr[40:44])[0]
    
    entry_sz = 104
    
    print(f'magic=0x{magic:08x} ver={ver}')
    print(f'n_tensors={nt}')
    print(f'meta_off={meta_off} meta_cnt={meta_cnt}')
    print(f'model_meta_off={model_meta_off} model_meta_sz={model_meta_sz}')
    
    data_off = meta_off + meta_cnt * entry_sz + model_meta_sz
    print(f'computed data_off = {data_off}')
    print(f'file_size = {pogls_sz}')
    print(f'data_section = {pogls_sz - data_off}')
    
    # Read meta entries
    f.seek(meta_off)
    cumsum = 0
    for i in range(meta_cnt):
        entry = f.read(entry_sz)
        if len(entry) < entry_sz:
            print(f'ERROR: short read at meta[{i}]')
            break
        # nbytes_orig at offset 8 (u64)
        nbytes_orig = struct.unpack('<Q', entry[8:16])[0]
        # comp_nbytes at offset 20? Let me check struct layout
        # dram_addr at 0: u64 (8 bytes)
        # nbytes_orig at 8: u64 (8 bytes)
        # comp_nbytes at 16: u32 (4 bytes)
        # comp_type at 20: u16 (2 bytes)
        # tensor_type at 22: u16 (2 bytes)
        # n_dims at 24: u32 (4 bytes)
        # dims[4] at 28: u32*4 (16 bytes)
        # name at 44: char[64]
        nbytes_orig = struct.unpack('<Q', entry[8:16])[0]
        comp_nbytes = struct.unpack('<I', entry[16:20])[0]
        comp_type = struct.unpack('<H', entry[20:22])[0]
        name = entry[44:108].split(b'\x00')[0].decode()
        
        sz = comp_nbytes if comp_nbytes > 0 else nbytes_orig
        cumsum += sz
        if i < 3 or (meta_cnt - i) <= 2:
            print(f'  meta[{i}]: {name} nbytes={nbytes_orig} comp={comp_nbytes} type={comp_type}')
    
    print(f'\ncumsum = {cumsum}')
    
    # Check how many bytes exist from data_off
    f.seek(data_off)
    data = f.read()
    actual_sz = len(data)
    print(f'actual data section = {actual_sz}')
    print(f'cumsum - actual = {cumsum - actual_sz}')
    
    # Check first tensor's data (first 4 bytes)
    print(f'first 8 bytes at data_off: {" ".join(f"{b:02x}" for b in data[:8])}')
    
    # Check GGUF model_meta magic
    f.seek(model_meta_off)
    mm_magic = f.read(4)
    print(f'model meta magic: {" ".join(f"{b:02x}" for b in mm_magic)}')
    print(f'= GGUF? {mm_magic == b"GGUF"}')
    
    # Where does model_meta end?
    mm_end = model_meta_off + model_meta_sz
    print(f'model_meta end: {mm_end}')
    print(f'data_off: {data_off}')
    print(f'match: {mm_end == data_off}')
