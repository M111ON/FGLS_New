import struct

# The writer says data_sec_off = 5947712
# My Python says aligned = 5947744 (from pos_after_tensors=5947741)
# Let's check BOTH positions

writer_data_off = 5947712
python_aligned = 5947744

f = open(r'I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf', 'rb')
f.seek(0, 2)
file_sz = f.tell()

# Check data at writer's reported data_off
f.seek(writer_data_off)
d1 = f.read(16)
print(f'Data at {writer_data_off} (writer): {" ".join(f"{b:02x}" for b in d1)}')

# Check data at my calculation
f.seek(python_aligned)
d2 = f.read(16)
print(f'Data at {python_aligned} (python): {" ".join(f"{b:02x}" for b in d2)}')

# The POGLS file has this first tensor data
pogls_start = bytes.fromhex('46f172420c059dad690cfaefe5f80604b6ed2061041de7c5f3fdc9600bf311e8')

# Is this from the GGUF at some offset?
# Search for it near data_sec_off
print(f'\nSearching for POGLS first data in GGUF...')
for off in range(writer_data_off - 100, writer_data_off + 100):
    f.seek(off)
    test = f.read(16)
    if test == pogls_start:
        print(f'  FOUND at GGUF offset {off}!')
        break
    i = test.find(pogls_start[:8])
    if i >= 0:
        f.seek(off + i)
        test2 = f.read(16)
        if test2 == pogls_start:
            print(f'  FOUND at GGUF offset {off+i}!')
            break
else:
    print('  Not found in +/-100 range near data_off')
    
    # Try different ranges
    # Maybe data was written at tensor_meta_off position
    for guess_off in [331904, 362168, 0, writer_data_off - 5947744]:
        f.seek(guess_off)
        test = f.read(16)
        if test == pogls_start:
            print(f'  FOUND at GGUF offset {guess_off}!')
            break
    
    # Or maybe the WRITER read from wrong position
    # (e.g., if pos_after_tensors was computed wrong)
    print(f'\nGGUF file size: {file_sz}')
    print(f'GGUF data section at {writer_data_off}: size = {file_sz - writer_data_off}')
    print(f'GGUF data section at {python_aligned}: size = {file_sz - python_aligned}')
    
    # What if the tensor DATA starts BEFORE pos_after_tensors?
    # Check if the GGUF has tensor info embedded in data section
    # Actual tensor info starts at offset 5931189 (pos after KV)
    # Let me check the LAST tensor's data in GGUF at aligned
    # Actually, let me check: is GGUF[5947712] part of the last tensor's metadata?
    f.seek(writer_data_off - 200)
    before = f.read(200)
    print(f'\n200 bytes before {writer_data_off}:')
    for i in range(0, 200, 16):
        seg = before[i:i+16]
        print(f'  {writer_data_off-200+i}: {" ".join(f"{b:02x}" for b in seg)}')
    
    # Is there tensor info near this position?
    # Let me trace the actual tensor info end more carefully
    f.seek(5931189)  # pos_after_kv
    # Read all 291 tensor info entries manually and track position
    pos = 5931189
    for i in range(291):
        klen_bytes = f.read(8)
        klen = struct.unpack('<Q', klen_bytes)[0]
        f.read(klen)
        nd = struct.unpack('<I', f.read(4))[0]
        for j in range(nd):
            f.read(8)
        dt = struct.unpack('<I', f.read(4))[0]
        off = struct.unpack('<Q', f.read(8))[0]
        pos = f.tell()
    
    print(f'\nActual pos_after_tensors = {pos}')
    print(f'Expected = ~5947741')
    print(f'Difference from expected: {pos - 5947741}')
    
    aligned2 = (pos + 31) // 32 * 32
    print(f'Recomputed aligned: {aligned2}')
    
    # Check data at this aligned
    f.seek(aligned2)
    real_data = f.read(16)
    print(f'Data at recomputed aligned: {" ".join(f"{b:02x}" for b in real_data)}')
    
    # Compare with writer
    f.seek(writer_data_off)
    writer_d = f.read(16)
    print(f'Data at writer data_off: {" ".join(f"{b:02x}" for b in writer_d)}')
    
f.close()
