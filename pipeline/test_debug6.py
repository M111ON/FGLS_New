"""Isolate decompress hang — test with explicit out_size."""
import sys
sys.path.insert(0, '.')
import geofield_ctypes as gf

# Test with explicit out_size
data = b'test decompress hang! ' * 200
print('Data: %d bytes' % len(data))
comp, stats = gf.full_compress(data)
if comp:
    print('Compressed: %d bytes, out_size=%d' % (len(comp), stats.total_out))
    print('Decompress with explicit out_size=%d...' % len(data))
    sys.stdout.flush()
    dec = gf.full_decompress(comp, len(data))
    if dec is None:
        print('FAILED')
    else:
        print('OK: %d bytes, match=%s' % (len(dec[0]), dec[0] == data))
    
    print('Decompress without out_size...')
    sys.stdout.flush()
    dec2 = gf.full_decompress(comp)
    if dec2 is None:
        print('FAILED')
    else:
        print('OK: %d bytes, match=%s' % (len(dec2[0]), dec2[0] == data))
