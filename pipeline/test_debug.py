"""Debug GPXL decode."""
import sys
sys.path.insert(0, '.')
import geofield_ctypes as gf
import geofield_cli as cli

data = b'GPXL compressed format test data! ' * 200
print('Input: %d bytes' % len(data))

encoded, stats = cli.encode(data, compress=True)
print('Encoded: %d bytes' % len(encoded))
print('Check flags byte %d' % encoded[7])

# Manual decode step by step
import struct
buf = memoryview(encoded)
n_segments = struct.unpack_from('<I', buf, 10)[0]
coord_lc_sz = struct.unpack_from('<I', buf, 30)[0]
flags = buf[7]
print('n_segments=%d flags=%d' % (n_segments, flags))

data_offset = cli.GPXL_HEADER_SZ + coord_lc_sz
pos = data_offset
for si in range(n_segments):
    seg_len = struct.unpack_from('<I', buf, pos)[0]
    pos += 4
    comp_flag = buf[pos]
    pos += 1
    print('Segment %d: seg_len=%d comp_flag=%d' % (si, seg_len, comp_flag))
    if comp_flag:
        seg_comp = bytes(buf[pos:pos + seg_len])
        pos += seg_len
        print('  compressed: %d bytes' % len(seg_comp))
        print('  first 16 bytes: %s' % seg_comp[:16].hex())
        # Try to parse GFCS header
        magic = struct.unpack_from('<I', seg_comp, 0)[0]
        print('  GFCS magic: 0x%08X (expected 0x53434647)' % magic)
        if magic == 0x53434647:
            ver = struct.unpack_from('<H', seg_comp, 4)[0]
            orig = struct.unpack_from('<Q', seg_comp, 18)[0]
            print('  GFCS version=%d orig_size=%d' % (ver, orig))
            dec = gf.full_decompress(seg_comp)
            if dec is None:
                print('  Decompress FAILED!')
            else:
                dec_data, dec_xxh = dec
                print('  Decompress OK: %d bytes, xxh=0x%x' % (len(dec_data), dec_xxh))
        break  # Just test first segment
    else:
        pos += seg_len

print('DONE')
