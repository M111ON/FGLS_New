import os, sys
sys.path.insert(0, '.')
from chunk_anim import *

# debug test: verify frame packing/unpacking
data = bytes([(i*7 + i*i) % 256 for i in range(5000)])

digest = xxh64(data)
total_chunks = (len(data) + CHUNK_SIZE - 1) // CHUNK_SIZE

# Pack chunks
chunks = []
for i in range(0, len(data), CHUNK_SIZE):
    raw = data[i:i+CHUNK_SIZE]
    if len(raw) < CHUNK_SIZE:
        raw = raw.ljust(CHUNK_SIZE, b'\x00')
    chunks.append(raw)

chunk_bytes = b''.join(chunks)
print(f"Total chunks: {total_chunks}, chunk_bytes len: {len(chunk_bytes)}")
print(f"DATA_BYTES per frame: {DATA_BYTES}")
print(f"CHUNKS_PER_FRAME: {CHUNKS_PER_FRAME}")

# What the encoder does:
frame_data_list = []
for i in range(0, len(chunk_bytes), DATA_BYTES):
    fd = chunk_bytes[i:i+DATA_BYTES]
    frame_data_list.append(fd)
    print(f"  Frame {len(frame_data_list)-1}: offset={i}, len={len(fd)}")

print(f"Total frames: {len(frame_data_list)}")

# Encode to GPX4
meta = encode_to_gpx4(data, 'test_debug.gpx4', keyframe_interval=3)
print(f"GPX4: {os.path.getsize('test_debug.gpx4')}B, frames={meta['n_frames']}")

# Decode
result = decode_from_gpx4('test_debug.gpx4')
re_digest = xxh64(result)

print(f"\nOriginal digest: {digest:016X}")
print(f"Recovered digest: {re_digest:016X}")
print(f"Match: {digest == re_digest}")
print(f"Original size: {len(data)}, recovered size: {len(result)}")
print(f"First 20 match: {data[:20] == result[:20]}")
print(f"Last 20 match: {data[-20:] == result[-20:]}")
if data != result:
    # find first difference
    for i in range(min(len(data), len(result))):
        if data[i] != result[i]:
            print(f"First diff at byte {i}: orig={data[i]}, got={result[i]}")
            break
