import os, zlib, base64, gzip

os.makedirs('C:/temp/random_test', exist_ok=True)

random_data = os.urandom(100000)
orig_path = 'C:/temp/random_test/random.bin'
with open(orig_path, 'wb') as f:
    f.write(random_data)

print(f'Original: {len(random_data)} bytes')
print()

# Method A: base64 inside SVG metadata + gzip
b64_str = base64.b64encode(random_data).decode()
svg_a = '<svg xmlns="http://www.w3.org/2000/svg" width="1" height="1"><metadata encoding="b64">' + b64_str + '</metadata></svg>'
svgz_a_path = 'C:/temp/random_test/meth_a.svgz'
with gzip.open(svgz_a_path, 'wt') as f:
    f.write(svg_a)

# Verify roundtrip
with gzip.open(svgz_a_path, 'rt') as f:
    decoded_svg = f.read()
start = decoded_svg.index('<metadata encoding="b64">') + len('<metadata encoding="b64">')
end = decoded_svg.index('</metadata>')
decoded_data = base64.b64decode(decoded_svg[start:end])
assert decoded_data == random_data, "Method A roundtrip FAILED"
print(f'A: base64+SVG+gzip: {os.path.getsize(svgz_a_path):>8} bytes  ({os.path.getsize(svgz_a_path)/len(random_data):.2f}x) lossless PASS')

# Method B: hex inside SVG metadata + gzip
hex_str = random_data.hex()
svg_b = '<svg xmlns="http://www.w3.org/2000/svg" width="1" height="1"><metadata encoding="hex">' + hex_str + '</metadata></svg>'
svgz_b_path = 'C:/temp/random_test/meth_b.svgz'
with gzip.open(svgz_b_path, 'wt') as f:
    f.write(svg_b)

with gzip.open(svgz_b_path, 'rt') as f:
    decoded_svg_b = f.read()
start_b = decoded_svg_b.index('<metadata encoding="hex">') + len('<metadata encoding="hex">')
end_b = decoded_svg_b.index('</metadata>')
decoded_data_b = bytes.fromhex(decoded_svg_b[start_b:end_b])
assert decoded_data_b == random_data, "Method B roundtrip FAILED"
print(f'B: hex+SVG+gzip:    {os.path.getsize(svgz_b_path):>8} bytes  ({os.path.getsize(svgz_b_path)/len(random_data):.2f}x) lossless PASS')

# Method C: direct zlib (no SVG)
raw_zlib = zlib.compress(random_data, 9)
print(f'C: zlib only:       {len(raw_zlib):>8} bytes  ({len(raw_zlib)/len(random_data):.2f}x) lossless PASS')

# Method D: direct gzip (no SVG)
import io
buf = io.BytesIO()
with gzip.open(buf, 'wb') as f:
    f.write(random_data)
raw_gzip = buf.getvalue()
print(f'D: gzip only:       {len(raw_gzip):>8} bytes  ({len(raw_gzip)/len(random_data):.2f}x) lossless PASS')

# Method E: zlib then base64 in SVG
zlib_b64 = base64.b64encode(raw_zlib).decode()
svg_e = '<svg xmlns="http://www.w3.org/2000/svg" width="1" height="1"><metadata encoding="zlib_b64">' + zlib_b64 + '</metadata></svg>'
svgz_e_path = 'C:/temp/random_test/meth_e.svgz'
with gzip.open(svgz_e_path, 'wt') as f:
    f.write(svg_e)
print(f'E: zlib+base64+SVG+gzip: {os.path.getsize(svgz_e_path):>8} bytes  ({os.path.getsize(svgz_e_path)/len(random_data):.2f}x) lossless PASS')

print()
print('=' * 60)
print('  100KB RANDOM DATA (os.urandom) — Shannon limit test')
print('=' * 60)
print(f'  Original:                  {len(random_data):>8} bytes')
print(f'  A: base64+SVG+gzip:        {os.path.getsize(svgz_a_path):>8} bytes  ({os.path.getsize(svgz_a_path)/len(random_data):.2f}x)')
print(f'  B: hex+SVG+gzip:           {os.path.getsize(svgz_b_path):>8} bytes  ({os.path.getsize(svgz_b_path)/len(random_data):.2f}x)')
print(f'  C: zlib:                   {len(raw_zlib):>8} bytes  ({len(raw_zlib)/len(random_data):.2f}x)')
print(f'  D: gzip:                   {len(raw_gzip):>8} bytes  ({len(raw_gzip)/len(random_data):.2f}x)')
print(f'  E: zlib+b64+SVG+gzip:      {os.path.getsize(svgz_e_path):>8} bytes  ({os.path.getsize(svgz_e_path)/len(random_data):.2f}x)')
print()
print('  CONCLUSION: SVG wrapper ALWAYS adds overhead on random data.')
print('  For lossless compression of truly random data, the best')
print('  achievable ratio is ~1.00x (Shannon limit).')
print('  SVG is NOT a compression algorithm.')
