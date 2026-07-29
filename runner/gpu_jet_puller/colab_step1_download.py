#!/usr/bin/env python3
"""Step 1: Download + extract GGUF model to raw tensor blob."""
import os, sys, subprocess, struct, time, json

MODEL = "/content/Qwen3-0.6B-Q4_0.gguf"
RAW = "/content/tensor_data.bin"

def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)

# Download if needed
if not (os.path.isfile(MODEL) and os.path.getsize(MODEL) > 1000000):
    url = "https://huggingface.co/Qwen/Qwen3-0.6B-GGUF/resolve/main/Qwen3-0.6B-Q8_0.gguf"
    log(f"wget {url.split('/')[-1]}...")
    r = subprocess.run(["wget", "-O", MODEL, url, "-q", "--no-check-certificate"],
                      capture_output=True, text=True, timeout=600)
    if r.returncode != 0 or not os.path.getsize(MODEL) > 1000000:
        log(f"wget failed (exit={r.returncode}), trying alt...")
        url2 = "https://huggingface.co/unsloth/Qwen3-0.6B-GGUF/resolve/main/Qwen3-0.6B-Q8_0.gguf"
        r = subprocess.run(["wget", "-O", MODEL, url2, "-q", "--no-check-certificate"],
                          capture_output=True, text=True, timeout=600)
    
    sz = os.path.getsize(MODEL) / 1e6
    log(f"Downloaded: {sz:.1f} MB")
else:
    log(f"Model exists ({os.path.getsize(MODEL)/1e6:.1f} MB)")

# Extract to raw binary using manual parser
log("Extracting tensors...")
with open(MODEL, "rb") as f:
    data = f.read()

n_tensors = struct.unpack("<Q", data[8:16])[0]
n_meta = struct.unpack("<Q", data[16:24])[0]
pos = 24

for _ in range(n_meta):
    klen = struct.unpack("<Q", data[pos:pos+8])[0]
    pos += 8 + klen
    vtype = struct.unpack("<I", data[pos:pos+4])[0]
    pos += 4
    if vtype == 8:  # string
        slen = struct.unpack("<Q", data[pos:pos+8])[0]
        pos += 8 + slen
    elif vtype == 9:  # array
        atype = struct.unpack("<I", data[pos:pos+4])[0]
        pos += 4
        alen = struct.unpack("<Q", data[pos:pos+8])[0]
        pos += 8
        for _ in range(alen):
            if atype == 8:
                slen = struct.unpack("<Q", data[pos:pos+8])[0]
                pos += 8 + slen
            else:
                pos += 4
    elif vtype in (10, 11, 12):  # 64-bit
        pos += 8
    elif vtype in (18, 19):  # 16-bit float
        pos += 2
    elif vtype in (0, 1, 7):  # uint8, int8, bool = 1 byte
        pos += 1
    elif vtype in (2, 3):  # uint16, int16 = 2 bytes
        pos += 2
    else:  # 32-bit (4, 5, 6) and unknown
        pos += 4

# Parse tensor info entries
names = []
dims_list = []
types = []
for _ in range(n_tensors):
    nlen = struct.unpack("<Q", data[pos:pos+8])[0]
    pos += 8
    name = data[pos:pos+nlen].decode("utf-8", errors="replace")
    pos += nlen
    names.append(name)
    nd = struct.unpack("<I", data[pos:pos+4])[0]
    pos += 4
    dd = [struct.unpack("<Q", data[pos+d*8:pos+d*8+8])[0] for d in range(nd)]
    dims_list.append(dd)
    pos += nd * 8
    tt = struct.unpack("<I", data[pos:pos+4])[0]
    pos += 4
    types.append(tt)

# Read data offsets
offsets = []
for _ in range(n_tensors):
    offsets.append(struct.unpack("<Q", data[pos:pos+8])[0])
    pos += 8

total = 0
with open(RAW, "wb") as out:
    for i in range(n_tensors):
        sz = offsets[i+1] - offsets[i] if i+1 < n_tensors else len(data) - offsets[i]
        out.write(data[offsets[i]:offsets[i]+sz])
        total += sz

log(f"Extracted {total/1e6:.1f} MB ({n_tensors} tensors)")
log("DONE")