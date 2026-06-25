import struct

with open('/mnt/i/model/LFM2.5-1.2B-Instruct-Q4_K_M.gguf', 'rb') as f:
    h = f.read(24)
    magic, ver, nt, nkv = struct.unpack('<IIQQ', h)
    print(f'magic=0x{magic:08X} ver={ver} ntensors={nt} nkv={nkv}')

    for i in range(min(nkv, 20)):
        klen = struct.unpack('<Q', f.read(8))[0]
        if klen > 10000:
            print(f'  BAD klen={klen} at pos {f.tell()-8}')
            break
        key = f.read(klen).decode('utf-8', errors='replace')
        vtype = struct.unpack('<I', f.read(4))[0]
        if vtype == 0:
            f.read(1)
        elif vtype == 1:
            f.read(1)
        elif vtype in (2,3):
            f.read(2)
        elif vtype in (4,5,6):
            f.read(4)
        elif vtype == 7:
            f.read(1)
        elif vtype in (10,11,12):
            f.read(8)
        elif vtype == 8:
            sl = struct.unpack('<Q', f.read(8))[0]
            f.read(sl)
        elif vtype == 9:
            at = struct.unpack('<I', f.read(4))[0]
            al = struct.unpack('<Q', f.read(8))[0]
            es = {0:1,1:1,2:2,3:2,4:4,5:4,6:4,7:1,10:8,11:8,12:8}.get(at, 4)
            f.read(es * al)
        else:
            f.read(8)
    print(f'  (read {i+1} KV entries)')

    # Align to 32 bytes after KV section end
    pos = f.tell()
    align = (32 - (pos % 32)) % 32
    f.read(align)
    print(f'  tensor info starts at {f.tell()}')

    type_names = {0:'F32',1:'F16',2:'Q4_0',3:'Q4_1',6:'Q5_0',7:'Q5_1',
                  8:'Q8_0',9:'Q8_1',10:'Q2_K',11:'Q3_K',12:'Q4_K',
                  13:'Q5_K',14:'Q6_K',15:'Q8_K'}
    types = {}
    for i in range(min(nt, 5)):
        klen = struct.unpack('<Q', f.read(8))[0]
        if klen > 1000:
            print(f'  BAD tensor name len={klen} at pos {f.tell()-8}')
            break
        name = f.read(klen).decode('utf-8', errors='replace')
        ndim = struct.unpack('<I', f.read(4))[0]
        dims = struct.unpack('<' + 'Q'*ndim, f.read(8*ndim))
        dtype = struct.unpack('<I', f.read(4))[0]
        offset = struct.unpack('<Q', f.read(8))[0]
        tname = type_names.get(dtype, f'UNKNOWN({dtype})')
        sz = 1
        for d in dims:
            sz *= d
        print(f'  [{i}] {name}: ndim={ndim} dims={dims} type={tname} sz={sz}')
