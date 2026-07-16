"""Test geopipeline with real GGUF model tensor data."""
import subprocess, sys, os, struct

MODEL = r"I:\model\SmolLM2-360M-Instruct.Q8_0.gguf"
PIPELINE = r"pipeline/geopipeline_cli.exe"

def read_gguf_tensors(path):
    """Parse GGUF header and return tensor list."""
    tensors = []
    with open(path, "rb") as f:
        magic = struct.unpack("<I", f.read(4))[0]
        assert magic == 0x46554747, f"Bad magic: {magic:#x}"
        version = struct.unpack("<I", f.read(4))[0]
        n_tensors = struct.unpack("<Q", f.read(8))[0]
        n_kv = struct.unpack("<Q", f.read(8))[0]
        # skip KV metadata
        for _ in range(n_kv):
            klen = struct.unpack("<Q", f.read(8))[0]
            f.seek(klen, 1)
            vtype = struct.unpack("<I", f.read(4))[0]
            if vtype == 9:  # array
                arr_type = struct.unpack("<I", f.read(4))[0]
                narr = struct.unpack("<Q", f.read(8))[0]
                esz = [1,1,2,2,4,4,4,1,0,0,8,8,8]
                if arr_type == 8:
                    for _ in range(narr):
                        slen = struct.unpack("<Q", f.read(8))[0]
                        f.seek(slen, 1)
                elif arr_type < 13:
                    f.seek(esz[arr_type] * narr, 1)
            else:
                esizes = {0:1,1:1,2:2,3:2,4:4,5:4,6:4,7:1,8:-1,10:8,11:8,12:8}
                if vtype == 8:
                    slen = struct.unpack("<Q", f.read(8))[0]
                    f.seek(slen, 1)
                elif vtype in esizes:
                    f.seek(esizes[vtype], 1)
                else:
                    raise ValueError(f"Unknown vtype {vtype}")
        # tensor info
        for i in range(n_tensors):
            nlen = struct.unpack("<Q", f.read(8))[0]
            name = f.read(nlen).decode("utf-8")
            n_dims = struct.unpack("<I", f.read(4))[0]
            dims = []
            for _ in range(n_dims):
                dims.append(struct.unpack("<q", f.read(8))[0])
            dtype = struct.unpack("<I", f.read(4))[0]
            data_off = struct.unpack("<Q", f.read(8))[0]
            # compute size (simplified for Q8_0)
            tensors.append((i, name, dims, dtype, data_off))
    return tensors

# compute size for GGML types
GGML_TYPE_SZ = {
    0: (4, 1),    # F32
    1: (2, 1),    # F16
    8: (34, 32),  # Q8_0: 2B scale + 32B values (34B block)
    2: (18, 32),  # Q4_0
    3: (20, 32),  # Q4_1
    6: (22, 32),  # Q5_0
    7: (24, 32),  # Q5_1
    10: (84, 256), # Q2_K
    11: (110, 256), # Q3_K
    12: (144, 256), # Q4_K
    13: (176, 256), # Q5_K
    14: (210, 256), # Q6_K
}

def tensor_size(dims, dtype):
    if dtype not in GGML_TYPE_SZ:
        return 0
    tsz, blck = GGML_TYPE_SZ[dtype]
    n_elems = 1
    for d in dims:
        n_elems *= d
    return (n_elems // blck) * tsz

def main():
    print(f"Model: {MODEL}")
    tensors = read_gguf_tensors(MODEL)
    print(f"Total tensors: {len(tensors)}")
    
    # Find interesting tensors: medium size, not too big
    candidates = []
    for idx, name, dims, dtype, off in tensors:
        sz = tensor_size(dims, dtype)
        if sz == 0:
            sz = dims[0] * (dims[1] if len(dims) > 1 else 1) * 4  # rough est
        candidates.append((sz, idx, name, dims, dtype, off))
    
    candidates.sort(reverse=True)
    print(f"\nTop 10 tensors by size:")
    for sz, idx, name, dims, dtype, off in candidates[:10]:
        print(f"  [{idx}] {name}: {sz:,} bytes, dims={dims}, dtype={dtype}")
    
    # Pick a few test tensors
    test_tensors = []
    # 1. Pick the largest attention weight (not embedding — too large)
    for sz, idx, name, dims, dtype, off in candidates:
        if "attn" in name.lower() and "weight" in name.lower() and sz < 5_000_000:
            test_tensors.append((idx, name, sz))
        if len(test_tensors) >= 2:
            break
    if not test_tensors:
        # fallback: medium sized tensor
        for sz, idx, name, dims, dtype, off in candidates:
            if 100_000 < sz < 5_000_000:
                test_tensors.append((idx, name, sz))
                break
    # 2. Also test with the smallest non-trivial tensor
    for sz, idx, name, dims, dtype, off in reversed(candidates):
        if sz > 4096 and sz < 500_000 and name not in [t[1] for t in test_tensors]:
            test_tensors.append((idx, name, sz))
            break
    
    print(f"\nTest tensors selected:")
    for idx, name, sz in test_tensors:
        print(f"  [{idx}] {name}: {sz:,} bytes")
    
    results = []
    for idx, name, sz in test_tensors:
        # Read tensor data
        binpath = f"pipeline/test_{name.replace('/', '_').replace('.', '_')}.bin"
        with open(MODEL, "rb") as f:
            # need data_offset
            # re-read to get data_offset... 
            pass
        
        # Use C tool for accurate extraction
        print(f"\n--- Testing: {name} ({sz:,} bytes) ---")
        
        # Extract using a simple python approach: read tensor by seeking
        with open(MODEL, "rb") as f:
            # Find the tensor's data offset
            dims, dtype, data_off = [], 0, 0
            for ti in tensors:
                if ti[0] == idx:
                    _, _, dims, dtype, data_off = ti
                    break
            
            f.seek(0)
            magic = struct.unpack("<I", f.read(4))[0]
            version = struct.unpack("<I", f.read(4))[0]
            n_tensors_2 = struct.unpack("<Q", f.read(8))[0]
            n_kv_2 = struct.unpack("<Q", f.read(8))[0]
            # skip KV metadata
            for _ in range(n_kv_2):
                klen = struct.unpack("<Q", f.read(8))[0]
                f.seek(klen, 1)
                vtype = struct.unpack("<I", f.read(4))[0]
                if vtype == 9:
                    arr_type = struct.unpack("<I", f.read(4))[0]
                    narr = struct.unpack("<Q", f.read(8))[0]
                    esz = [1,1,2,2,4,4,4,1,0,0,8,8,8]
                    if arr_type == 8:
                        for _ in range(narr):
                            slen = struct.unpack("<Q", f.read(8))[0]
                            f.seek(slen, 1)
                    elif arr_type < 13:
                        f.seek(esz[arr_type] * narr, 1)
                else:
                    esizes = {0:1,1:1,2:2,3:2,4:4,5:4,6:4,7:1,8:-1,10:8,11:8,12:8}
                    if vtype == 8:
                        slen = struct.unpack("<Q", f.read(8))[0]
                        f.seek(slen, 1)
                    elif vtype in esizes:
                        f.seek(esizes[vtype], 1)
                    else:
                        raise ValueError(f"Unknown vtype {vtype}")
            
            # Now reading tensor infos — we're at the start of tensor infos
            tensor_info_end = f.tell()
            for ti in range(n_tensors_2):
                nlen = struct.unpack("<Q", f.read(8))[0]
                f.seek(nlen, 1)  # skip name
                n_dims = struct.unpack("<I", f.read(4))[0]
                f.seek(n_dims * 8, 1)  # skip dims
                f.seek(4, 1)  # skip dtype
                f.seek(8, 1)  # skip offset
                
            data_start = f.tell()
            # align to 32
            pad = (32 - (data_start % 32)) % 32
            data_start += pad
            
            # Now seek to our tensor's data
            tsz = tensor_size(dims, dtype)
            abs_offset = data_start + data_off
            f.seek(abs_offset)
            tensor_data = f.read(tsz)
        
        bin_sz = len(tensor_data)
        print(f"  Extracted {bin_sz:,} bytes (expected {sz:,})")
        
        with open(binpath, "wb") as f:
            f.write(tensor_data)
        
        # Run geopipeline encode
        gpx5path = binpath.replace(".bin", ".gpx5")
        decpath = binpath.replace(".bin", "_dec.bin")
        
        enc = subprocess.run([PIPELINE, "encode", binpath, gpx5path],
                           capture_output=True, text=True)
        print(f"  Encode: {enc.stdout}")
        
        # Get encoded size
        gpx5_sz = os.path.getsize(gpx5path) if os.path.exists(gpx5path) else 0
        
        # Decode
        dec = subprocess.run([PIPELINE, "decode", gpx5path, decpath],
                           capture_output=True, text=True)
        print(f"  Decode: {dec.stdout}")
        
        # Compare
        dec_sz = os.path.getsize(decpath) if os.path.exists(decpath) else 0
        match = os.path.exists(decpath) and dec_sz == bin_sz
        
        if match:
            with open(binpath, "rb") as f1, open(decpath, "rb") as f2:
                match = f1.read() == f2.read()
        
        ratio = bin_sz / gpx5_sz if gpx5_sz > 0 else 0
        results.append((name, bin_sz, gpx5_sz, ratio, match))
        
        status = "PASS" if match else "FAIL"
        print(f"  Result: {status} (ratio={ratio:.4f}x)")
        
        # Clean up
        for p in [binpath, gpx5path, decpath]:
            if os.path.exists(p):
                os.remove(p)
    
    print(f"\n{'='*60}")
    print(f"FINAL RESULTS:")
    print(f"{'='*60}")
    for name, insz, outsz, ratio, match in results:
        status = "PASS" if match else "FAIL"
        print(f"  {name[:50]:50s} {insz:>8,} -> {outsz:>8,} ({ratio:.4f}x) [{status}]")

if __name__ == "__main__":
    main()
