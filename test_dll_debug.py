import sys
sys.path.insert(0, 'tools')
from geopixel_pipeline import pipeline_encode, pipeline_decode

data = open('collection/wallet_seed_c.dll', 'rb').read()
result = pipeline_encode(data)
dec = pipeline_decode(result.encoded)

decoded = dec["data"]
print(f"Original size: {len(data)}")
print(f"Encoded size:  {len(result.encoded)}")
print(f"Decoded size:  {len(decoded)}")

if len(data) != len(decoded):
    print(f"Length mismatch: {len(data)} vs {len(decoded)}")

for i in range(min(len(data), len(decoded))):
    if data[i] != decoded[i]:
        print(f"First diff at byte {i}: orig=0x{data[i]:02x} decoded=0x{decoded[i]:02x}")
        # Show surrounding context
        start = max(0, i - 4)
        end = min(len(data), i + 8)
        print(f"  orig[{start}:{end}]: {data[start:end].hex()}")
        print(f"  dec [{start}:{end}]: {decoded[start:end].hex()}")
        break
else:
    print("No diff found")
