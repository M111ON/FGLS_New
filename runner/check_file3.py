import struct

# Correct PoglsTensorMeta (104 bytes):
# offset 0: addr (u32, 4)
# offset 4: dtype (u32, 4)
# offset 8: ndim (u32, 4)
# offset 12: nbytes_orig (u32, 4)
# offset 16: comp_type (u32, 4)
# offset 20: comp_nbytes (u32, 4)
# offset 24: dims[4] (u32*4, 16)
# offset 40: name (char[64], 64)
ENTRY_SZ = 104

with open('test_qwen_nocomp.pogls', 'rb') as f:
    # Header
    f.seek(0)
    hdr = f.read(48)
    meta_off = struct.unpack('<Q', hdr[16:24])[0]
    meta_cnt = struct.unpack('<I', hdr[24:28])[0]
    model_meta_off = struct.unpack('<Q', hdr[32:40])[0]
    model_meta_sz = struct.unpack('<I', hdr[40:44])[0]
    
    data_off = meta_off + meta_cnt * ENTRY_SZ + model_meta_sz
    print(f'meta_off={meta_off} meta_cnt={meta_cnt}')
    print(f'model_meta_off={model_meta_off} model_meta_sz={model_meta_sz}')
    print(f'data_off={data_off}')
    
    f.seek(0, 2)
    file_sz = f.tell()
    print(f'file_size={file_sz}')
    print(f'data_section={file_sz - data_off}')
    
    # Read meta entries
    f.seek(meta_off)
    cumsum = 0
    for i in range(meta_cnt):
        entry = f.read(ENTRY_SZ)
        addr = struct.unpack('<I', entry[0:4])[0]
        dtype = struct.unpack('<I', entry[4:8])[0]
        ndim = struct.unpack('<I', entry[8:12])[0]
        nbytes = struct.unpack('<I', entry[12:16])[0]
        comp_type = struct.unpack('<I', entry[16:20])[0]
        comp_nbytes = struct.unpack('<I', entry[20:24])[0]
        name = entry[40:104].split(b'\x00')[0].decode()
        
        sz = comp_nbytes if comp_nbytes > 0 else nbytes
        cumsum += sz
        if i < 5 or i == meta_cnt - 1:
            print(f'  [{i}] addr={addr} dtype={dtype} ndim={ndim} nbytes={nbytes} comp_type={comp_type} comp_nbytes={comp_nbytes} name={name}')
    
    print(f'\ncumsum = {cumsum}')
    print(f'actual data = {file_sz - data_off}')
    print(f'diff = {cumsum - (file_sz - data_off)}')
    
    # Check data content
    f.seek(data_off)
    data_start = f.read(16)
    print(f'first 16 bytes: {" ".join(f"{b:02x}" for b in data_start)}')
    
    # How many tensors have data?
    f.seek(data_off)
    non_zero = 0
    for i in range(meta_cnt):
        sz = (comp_nbytes if comp_nbytes > 0 else nbytes)  # WRONG — use loop variable
        break  # skip for now, redo below
    
    # Count actual tensors with non-zero data in file
    f.seek(data_off)
    # Simple: read data in chunks and check
    data = f.read()
    actual_sz = len(data)
    print(f'actual file data available: {actual_sz}')
    
    # Now verify cumsum against data_offs
    f.seek(meta_off)
    cumsum_check = 0
    for i in range(meta_cnt):
        entry = f.read(ENTRY_SZ)
        nbytes = struct.unpack('<I', entry[12:16])[0]
        comp_type = struct.unpack('<I', entry[16:20])[0]
        comp_nbytes = struct.unpack('<I', entry[20:24])[0]
        sz = comp_nbytes if comp_nbytes > 0 else nbytes
        if cumsum_check + sz > actual_sz:
            print(f'Tensor [{i}] overflow! cumsum_check={cumsum_check} sz={sz} actual_sz={actual_sz}')
            name = entry[40:104].split(b'\x00')[0].decode()
            print(f'  name={name} nbytes={nbytes} comp_type={comp_type} comp_nbytes={comp_nbytes}')
            break
        cumsum_check += sz
    else:
        print(f'All {meta_cnt} tensors fit within data section')
    print(f'cumsum_check = {cumsum_check}')
