"""Quick test for GPXL decode."""
import sys
sys.path.insert(0, '.')
import geofield_ctypes as gf
import geofield_cli as cli

data = b'GPXL compressed format test data! ' * 200
encoded, stats = cli.encode(data, compress=True)
print('Encoded: %d bytes' % len(encoded))

decoded, xxh_ok = cli.decode(encoded)
print('Match: %s' % (decoded == data))
print('xxh64: %s' % ('PASS' if xxh_ok else 'FAIL'))
print('ALL OK')
