"""Test with correct out_size (original size, not compressed size)."""
import sys
sys.path.insert(0, '.')
import geofield_ctypes as gf

data = b'test decompress hang! ' * 200
print('Data: %d bytes' % len(data))
comp, stats = gf.full_compress(data)
print('Compressed: %d bytes, total_out=%d' % (len(comp), stats.total_out))

# Correct: out_size = original data length
print('Decompress with out_size=%d (original size)...' % len(data))
sys.stdout.flush()
dec = gf.full_decompress(comp, len(data))
if dec is None:
    print('FAILED')
else:
    print('OK: %d bytes, match=%s' % (len(dec[0]), dec[0] == data))
