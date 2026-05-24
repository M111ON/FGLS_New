import os, sys
sys.path.insert(0, '.')
from chunk_anim import encode_to_gpx4, decode_from_gpx4

# T1: small (1 chunk)
data1 = bytes(range(64))
meta1 = encode_to_gpx4(data1, 'test_anim_1.gpx4', keyframe_interval=4)
sz1 = os.path.getsize('test_anim_1.gpx4')
print(f'T1: {sz1}B gpx4, orig=64B, ratio={sz1/64:.1f}x')
result1 = decode_from_gpx4('test_anim_1.gpx4')
assert data1 == result1, 'T1 FAIL'
print('T1: PASS (1 chunk)')

# T2: medium (500B = 8 chunks)
data2 = bytes([i % 256 for i in range(500)])
meta2 = encode_to_gpx4(data2, 'test_anim_2.gpx4', keyframe_interval=2)
nf2 = meta2["n_frames"]
print(f'T2: {len(data2)}B -> {os.path.getsize("test_anim_2.gpx4")}B, frames={nf2}')
result2 = decode_from_gpx4('test_anim_2.gpx4')
assert data2 == result2, 'T2 FAIL'
print('T2: PASS (500B)')

data3 = bytes([(i*7 + i*i) % 256 for i in range(5000)])
meta3 = encode_to_gpx4(data3, 'test_anim_3.gpx4', keyframe_interval=3)
nf3 = meta3["n_frames"]
print(f'T3: {len(data3)}B -> {os.path.getsize("test_anim_3.gpx4")}B, frames={nf3}')
result3 = decode_from_gpx4('test_anim_3.gpx4')
assert data3 == result3, 'T3 FAIL'
print('T3: PASS (5000B)')

data4 = bytes(100000)
meta4 = encode_to_gpx4(data4, 'test_anim_4.gpx4', keyframe_interval=8)
nf4 = meta4["n_frames"]
print(f'T4: {len(data4)}B -> {os.path.getsize("test_anim_4.gpx4")}B, frames={nf4}')
result4 = decode_from_gpx4('test_anim_4.gpx4')
assert data4 == result4, 'T4 FAIL'
print('T4: PASS (100KB zeros)')

print()
print('=== ALL PASS ===')
