import struct, sys
path = sys.argv[1]
with open(path, "rb") as f:
    data = f.read()
n_tensors = struct.unpack("<Q", data[8:16])[0]
n_kv = struct.unpack("<Q", data[16:24])[0]
pos = 24
print(f"tensors={n_tensors} kv={n_kv}")
for _ in range(n_kv):
    klen = struct.unpack("<Q", data[pos:pos+8])[0]; pos += 8 + klen
    vtype = struct.unpack("<I", data[pos:pos+4])[0]; pos += 4
    if vtype == 8: slen = struct.unpack("<Q", data[pos:pos+8])[0]; pos += 8 + slen
    elif vtype == 9:
        at = struct.unpack("<I", data[pos:pos+4])[0]; pos += 4
        al = struct.unpack("<Q", data[pos:pos+8])[0]; pos += 8
        for _ in range(al):
            if at == 8: sl = struct.unpack("<Q", data[pos:pos+8])[0]; pos += 8 + sl
            else: pos += 4
    elif vtype in (10,11,12): pos += 8
    elif vtype in (18,19): pos += 2
    elif vtype in (0,1,7): pos += 1
    elif vtype in (2,3): pos += 2
    else: pos += 4
for i in range(min(n_tensors, 400)):
    nlen = struct.unpack("<Q", data[pos:pos+8])[0]; pos += 8
    name = data[pos:pos+nlen].decode("utf-8",errors="replace"); pos += nlen
    nd = struct.unpack("<I", data[pos:pos+4])[0]; pos += 4
    dims = [struct.unpack("<Q", data[pos+j*8:pos+j*8+8])[0] for j in range(nd)]
    pos += nd * 8
    tt = struct.unpack("<I", data[pos:pos+4])[0]; pos += 4
    ne = 1
    for d in dims: ne *= d
    tn = {0:"F32",1:"F16",2:"Q4_0",8:"Q8_0",10:"Q6_K"}.get(tt, f"t{tt}")
    print(f"  [{i:3d}] {name:45s} {tn:6s} {str(dims):30s} elems={ne:>12,}")
