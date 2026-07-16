"""Debug GPXL decode — test all segments."""
import sys
sys.path.insert(0, '.')
import geofield_ctypes as gf
import geofield_cli as cli
import struct

data = b'GPXL compressed format test data! ' * 200
print('Input: %d bytes' % len(data))

encoded, stats = cli.encode(data, compress=True)
print('Encoded: %d bytes, segments=%d' % (len(encoded), stats['n_segments']))

buf = memoryview(encoded)
is_compressed = bool(buf[7] & cli.GPXL_FLAG_COMPRESSED)
n_segments = struct.unpack_from('<I', buf, 10)[0]
coord_lc_sz = struct.unpack_from('<I', buf, 30)[0]
data_offset = cli.GPXL_HEADER_SZ + coord_lc_sz

pos = data_offset
result = bytearray()
for si in range(n_segments):
    seg_len = struct.unpack_from('<I', buf, pos)[0]
    pos += 4
    comp_flag = buf[pos]
    pos += 1
    sys.stdout.write('Seg %d: len=%d flag=%d ' % (si, seg_len, comp_flag))
    sys.stdout.flush()

    if comp_flag and is_compressed:
        seg_comp = bytes(buf[pos:pos + seg_len])
        pos += seg_len
        dec = gf.full_decompress(seg_comp)
        if dec is None:
            print('DECOMPRESS FAIL')
            sys.exit(1)
        dec_data, dec_xxh = dec
        result += dec_data
        print('decompressed %d bytes (xxh=0x%x)' % (len(dec_data), dec_xxh))
    else:
        seg_raw = bytes(buf[pos:pos + seg_len])
        pos += seg_len
        result += seg_raw
        print('raw %d bytes' % len(seg_raw))
    sys.stdout.flush()

result = bytes(result)
match = (result == data)
print('Match: %s' % match)
print('DONE')
