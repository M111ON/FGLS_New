"""Debug GPXL decode — check segment 1 header."""
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
n_segments = struct.unpack_from('<I', buf, 10)[0]
coord_lc_sz = struct.unpack_from('<I', buf, 30)[0]
data_offset = cli.GPXL_HEADER_SZ + coord_lc_sz

# Parse segment 0 to get to segment 1
pos = data_offset
for si in range(2):
    seg_len = struct.unpack_from('<I', buf, pos)[0]
    pos += 4
    comp_flag = buf[pos]
    pos += 1
    print('Seg %d: seg_len=%d comp_flag=%d' % (si, seg_len, comp_flag))
    if si == 1:
        seg_comp = bytes(buf[pos:pos + seg_len])
        print('  compressed: %d bytes' % len(seg_comp))
        print('  first 32 hex: %s' % seg_comp[:32].hex())
        magic = struct.unpack_from('<I', seg_comp, 0)[0]
        print('  magic: 0x%08X' % magic)
        if magic == 0x53434647:
            ver = struct.unpack_from('<H', seg_comp, 4)[0]
            n_segs = struct.unpack_from('<I', seg_comp, 6)[0]
            n_blocks = struct.unpack_from('<I', seg_comp, 10)[0]
            n_pats = struct.unpack_from('<I', seg_comp, 14)[0]
            orig = struct.unpack_from('<Q', seg_comp, 18)[0]
            stored_xxh = struct.unpack_from('<Q', seg_comp, 26)[0]
            print('  GFCS: ver=%d segs=%d blocks=%d pats=%d orig=%d xxh=0x%x' % (
                ver, n_segs, n_blocks, n_pats, orig, stored_xxh))
            # Try decompress with short timeout... just test header parsing
            print('  Calling full_decompress...')
            sys.stdout.flush()
            dec = gf.full_decompress(seg_comp)
            if dec is None:
                print('  Decompress returned None!')
            else:
                dec_data, dec_xxh = dec
                print('  OK: %d bytes, xxh=0x%x' % (len(dec_data), dec_xxh))
    else:
        pos += seg_len  # skip segment 1 data

print('DONE')
