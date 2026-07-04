import struct

# Check GGUF file
with open(r'I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf', 'rb') as f:
    f.seek(0, 2)
    gguf_sz = f.tell()

# Check POGLS file
with open('test_qwen_nocomp.pogls', 'rb') as f:
    f.seek(0, 2)
    pogls_sz = f.tell()
    
    # Read header
    f.seek(0)
    hdr = f.read(128)
    
    # POGLS header fields (tensor_meta_off at offset 8, model_meta fields at 24)
    meta_off = struct.unpack('<Q', hdr[8:16])[0]
    meta_cnt = struct.unpack('<I', hdr[16:20])[0]
    model_meta_off = struct.unpack('<Q', hdr[24:32])[0]
    model_meta_sz = struct.unpack('<I', hdr[32:36])[0]
    
    entry_sz = 104  # sizeof(PoglsTensorMeta)
    data_off = meta_off + meta_cnt * entry_sz + model_meta_sz
    
    print(f'GGUF file size: {gguf_sz}')
    print(f'POGLS file size: {pogls_sz}')
    print(f'POGLS meta_off={meta_off} meta_cnt={meta_cnt} model_meta_off={model_meta_off} model_meta_sz={model_meta_sz}')
    print(f'POGLS computed data_off = {data_off}')
    print(f'POGLS data section = {pogls_sz - data_off}')
    
    # Read POGLS meta cumsum
    f.seek(meta_off)
    cumsum = 0
    for i in range(meta_cnt):
        entry = f.read(entry_sz)
        # nbytes_orig at offset 8 (uint64)
        # comp_nbytes at offset 20 (uint32)
        nbytes_orig = struct.unpack('<Q', entry[8:16])[0]
        comp_nbytes = struct.unpack('<I', entry[20:24])[0]
        sz = comp_nbytes if comp_nbytes > 0 else nbytes_orig
        cumsum += sz
    
    print(f'POGLS meta cumsum: {cumsum}')
    expected_file_sz = data_off + cumsum
    print(f'Expected file size (data_off + cumsum): {expected_file_sz}')
    print(f'Actual file size: {pogls_sz}')
    print(f'Difference: {expected_file_sz - pogls_sz}')
    
    # Check: does the data section actually contain cumsum bytes?
    # Seek to data_off and read the first few thousand bytes
    f.seek(data_off)
    first_byte = f.read(1)
    print(f'Data section first byte: 0x{first_byte[0]:02x}')
    
    # Try reading from data_off to end
    f.seek(data_off)
    data = f.read()
    actual_data_sz = len(data)
    print(f'Actual data section size: {actual_data_sz}')
    print(f'Cumsum used: {cumsum}')
    print(f'cumsum - actual_data = {cumsum - actual_data_sz}')
    
    # Check if file has bytes beyond model_meta
    f.seek(model_meta_off)
    mm_bytes = f.read(min(100, model_meta_sz))
    print(f'\nModel meta first 4 bytes: {mm_bytes[:4].hex()}')
    
    # Check if GGUF magic is at model_meta_off (should be if meta blob is valid)
    print(f'GGUF magic at model_meta: {mm_bytes[:4] == b"GGUF"}')
    
    # Where does model_meta end?
    mm_end = model_meta_off + model_meta_sz
    print(f'model_meta end: {mm_end}')
    print(f'data_off: {data_off}')
    print(f'model_meta_end == data_off: {mm_end == data_off}')
