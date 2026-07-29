#!/usr/bin/env python3
"""Inspect GGUF file header and extract tensor data for GPU benchmark."""
import os, sys, struct, json

model_path = "/content/Qwen3-0.6B-Q4_0.gguf"

with open(model_path, "rb") as f:
    data = f.read()

print(f"File: {model_path} ({len(data)/1e6:.1f} MB)")

# Parse GGUF header
magic = struct.unpack("<I", data[0:4])[0]
version = struct.unpack("<I", data[4:8])[0]
n_tensors = struct.unpack("<Q", data[8:16])[0]
n_metadata = struct.unpack("<Q", data[16:24])[0]

print(f"Magic: 0x{magic:08X} {'OK' if magic == 0x46475547 else 'MISMATCH'}")
print(f"Version: {version}")
print(f"n_tensors: {n_tensors}")
print(f"n_metadata_kv: {n_metadata}")

pos = 24
# Skip metadata
for i in range(n_metadata):
    key_len = struct.unpack("<Q", data[pos:pos+8])[0]
    pos += 8
    key = data[pos:pos+key_len].decode('utf-8', errors='replace')
    pos += key_len
    vtype = struct.unpack("<I", data[pos:pos+4])[0]
    pos += 4
    
    if vtype == 0:  # uint8
        val = data[pos]; pos += 1
    elif vtype == 1:  # int8
        val = struct.unpack("<b", data[pos:pos+1])[0]; pos += 1
    elif vtype == 2:  # uint16
        val = struct.unpack("<H", data[pos:pos+2])[0]; pos += 2
    elif vtype == 3:  # int16
        val = struct.unpack("<h", data[pos:pos+2])[0]; pos += 2
    elif vtype == 4:  # uint32
        val = struct.unpack("<I", data[pos:pos+4])[0]; pos += 4
    elif vtype == 5:  # int32
        val = struct.unpack("<i", data[pos:pos+4])[0]; pos += 4
    elif vtype == 6:  # float32
        val = struct.unpack("<f", data[pos:pos+4])[0]; pos += 4
    elif vtype == 7:  # bool
        val = data[pos]; pos += 1
    elif vtype == 8:  # string
        slen = struct.unpack("<Q", data[pos:pos+8])[0]
        pos += 8
        val = data[pos:pos+slen].decode('utf-8', errors='replace')
        pos += slen
    elif vtype == 9:  # array
        atype = struct.unpack("<I", data[pos:pos+4])[0]; pos += 4
        alen = struct.unpack("<Q", data[pos:pos+8])[0]; pos += 8
        vals = []
        for j in range(alen):
            if atype == 8:  # string
                slen = struct.unpack("<Q", data[pos:pos+8])[0]; pos += 8
                vals.append(data[pos:pos+slen].decode('utf-8', errors='replace'))
                pos += slen
            else:
                vals.append(0); pos += 4
        val = f"[{', '.join(str(v) for v in vals[:5])}{'...' if len(vals) > 5 else ''}]"
    elif vtype == 10:  # uint64
        val = struct.unpack("<Q", data[pos:pos+8])[0]; pos += 8
    elif vtype == 11:  # int64
        val = struct.unpack("<q", data[pos:pos+8])[0]; pos += 8
    elif vtype == 12:  # float64
        val = struct.unpack("<d", data[pos:pos+8])[0]; pos += 8
    elif vtype == 18:  # float16
        val = struct.unpack("<e", data[pos:pos+2])[0]; pos += 2
    elif vtype == 19:  # bf16
        val = 0; pos += 2
    else:
        val = f"unknown type {vtype}"; pos += 4
    
    if i < 10:  # Print first 10 metadata
        print(f"  meta[{i}]: {key} = {val}")

print(f"\nTensor info at pos={pos} (0x{pos:x})")

# Parse tensor info entries
tensors = []
for i in range(n_tensors):
    name_len = struct.unpack("<Q", data[pos:pos+8])[0]
    pos += 8
    name = data[pos:pos+name_len].decode('utf-8', errors='replace')
    pos += name_len
    
    n_dims = struct.unpack("<I", data[pos:pos+4])[0]
    pos += 4
    
    dims = []
    for d in range(n_dims):
        dims.append(struct.unpack("<Q", data[pos:pos+8])[0])
        pos += 8
    
    ggml_type = struct.unpack("<I", data[pos:pos+4])[0]
    pos += 4
    
    tensors.append((name, n_dims, dims, ggml_type))
    
    if i < 5 or i == n_tensors - 1:
        print(f"  [{i}] {name}: dims={dims}, type={ggml_type}")

# Tensor data offsets
data_offset_start = pos
print(f"\nTensor data offsets at pos={pos} (0x{pos:x})")

offsets = []
for i in range(n_tensors):
    offset = struct.unpack("<Q", data[pos:pos+8])[0]
    pos += 8
    offsets.append(offset)

# Calculate tensor data sizes and total
total_data = 0
for i in range(min(10, n_tensors)):
    name, n_dims, dims, ggml_type = tensors[i]
    off = offsets[i]
    if i + 1 < n_tensors:
        size = offsets[i+1] - off
    else:
        size = len(data) - off
    total_data += size
    print(f"  [{i}] {name}: offset={off}, size={size/1e6:.2f} MB")

total_size = sum(offsets[i+1] - offsets[i] for i in range(n_tensors - 1))
if n_tensors > 1:
    total_size += len(data) - offsets[-1]
print(f"\nTotal tensor data: {total_size/1e6:.2f} MB")
print(f"File size: {len(data)/1e6:.2f} MB")
print(f"Tensor data starts at offset: {offsets[0] if offsets else 'N/A'}")

# Write tensor data to a simple binary blob
out_path = "/content/tensor_data.bin"
with open(out_path, "wb") as out:
    for i in range(n_tensors):
        off = offsets[i]
        if i + 1 < n_tensors:
            size = offsets[i+1] - off
        else:
            size = len(data) - off
        out.write(data[off:off+size])

print(f"\nWrote raw tensor data: {out_path} ({total_size/1e6:.2f} MB)")

# Save metadata as JSON
meta_json = {
    "n_tensors": n_tensors,
    "total_bytes": total_size,
    "file_bytes": len(data),
    "tensors": []
}
for i in range(n_tensors):
    name, n_dims, dims, ggml_type = tensors[i]
    off = offsets[i]
    if i + 1 < n_tensors:
        size = offsets[i+1] - off
    else:
        size = len(data) - off
    meta_json["tensors"].append({
        "name": name,
        "dims": dims,
        "type": ggml_type,
        "offset": off,
        "size": size
    })

meta_path = "/content/tensor_meta.json"
with open(meta_path, "w") as f:
    json.dump(meta_json, f, indent=2)
print(f"Wrote metadata: {meta_path}")

print("\n=== DONE ===")