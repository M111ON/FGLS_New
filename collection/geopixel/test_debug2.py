import sys, zstd
sys.path.insert(0, '.')
from chunk_anim import *

data = bytes([(i*7 + i*i) % 256 for i in range(5000)])
meta = encode_to_gpx4(data, 't3.gpx4', keyframe_interval=3)

gpx = Gpx4File()
gpx.read('t3.gpx4')

dctx = zstd.ZstdDecompressor()
for l in gpx.layers:
    name = l['name']
    if name in ('F000', 'D001'):
        blob = l['data']
        try:
            rgb = dctx.decompress(blob)
            print(f'{name}: compressed={len(blob)}B, decompressed={len(rgb)}B')
        except Exception as e:
            print(f'{name}: DECOMPRESS ERROR: {e}')
            try:
                rgb = dctx.decompress(blob, max_output_size=4096)
                print(f'  retry: {len(rgb)}B')
            except Exception as e2:
                print(f'  retry also failed: {e2}')

gpx.close()

print()
print('Metadata from frame 0 via decode_from_gpx4:')
try:
    result = decode_from_gpx4('t3.gpx4')
    print(f'OK: {len(result)}B, match={data == result}')
except Exception as e:
    print(f'DECODE ERROR: {e}')
    import traceback
    traceback.print_exc()
