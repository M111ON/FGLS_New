"""Isolate decompress hang on segment 1 data."""
import sys
sys.path.insert(0, '.')
import geofield_ctypes as gf
import struct

# Directly call C decompress with segment 1 data
seg_comp_hex = "474643530200010000002b00000006000000900a000000000000d4e566937253"
# Actually, let me just create test data and compress it
data = b'test decompress hang! ' * 200  # 4400 bytes
print('Data: %d bytes' % len(data))
comp, stats = gf.full_compress(data)
if comp:
    print('Compressed: %d bytes' % len(comp))
    print('Stats: total_out=%d comp_size=%d' % (stats.total_out, stats.comp_size))
    
    # Now decompress
    print('Decompressing...')
    sys.stdout.flush()
    dec = gf.full_decompress(comp)
    if dec is None:
        print('FAILED')
    else:
        print('OK: %d bytes' % len(dec[0]))
        print('Match: %s' % (dec[0] == data))
else:
    print('Compress failed')
