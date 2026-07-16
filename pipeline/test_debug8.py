"""Low-level test of geofield_full_decompress."""
import sys, struct, ctypes
sys.path.insert(0, '.')
import geofield_ctypes as gf

data = b'test decompress hang! ' * 200
print('Data: %d bytes' % len(data))

# Compress via low-level C
import geofield_ctypes as _gfc
stats = _gfc.GFCSStats()
in_buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
rc = _gfc._lib.geofield_full_compress(in_buf, len(data), 32, 4096, None, 0, ctypes.byref(stats))
print('Dry run rc=%d total_out=%d' % (rc, stats.total_out))

buf = (ctypes.c_uint8 * stats.total_out)()
rc = _gfc._lib.geofield_full_compress(in_buf, len(data), 32, 4096, buf, stats.total_out, ctypes.byref(stats))
print('Compress rc=%d total_out=%d' % (rc, stats.total_out))

# Now decompress via low-level C
out_buf = (ctypes.c_uint8 * len(data))()
got_xxh = ctypes.c_uint64()
print('Calling geofield_full_decompress...')
sys.stdout.flush()
rc = _gfc._lib.geofield_full_decompress(buf, stats.total_out, out_buf, len(data), ctypes.byref(got_xxh))
print('Decompress rc=%d xxh=0x%x' % (rc, got_xxh.value))
result = bytes(out_buf[:len(data)])
print('Match: %s' % (result == data))
