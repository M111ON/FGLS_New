"""Integration test for GeoField Pipeline compression step."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geofield_ctypes as gf
import geofield_cli as cli

def test_gfcs_roundtrip():
    """Test GFCS compress/decompress roundtrip."""
    data = b'Hello GeoField Pipeline Compression! ' * 100
    print(f'Test 1: GFCS compress/decompress roundtrip')
    print(f'  Input: {len(data)} bytes')
    comp, stats = gf.full_compress(data)
    assert comp is not None, 'compress returned None'
    print(f'  Compressed: {len(comp)} bytes (ratio={len(comp)/len(data):.2f}x)')
    print(f'  Stats: total_out={stats.total_out}, comp_size={stats.comp_size}, patterns={stats.n_patterns}')
    dec = gf.full_decompress(comp, len(data))
    assert dec is not None, 'decompress returned None'
    dec_data, dec_xxh = dec
    assert dec_data == data, f'Roundtrip mismatch'
    print(f'  Roundtrip: PASS')
    print()

def test_dt_compressed_roundtrip():
    """Test DRamTile compressed store/restore."""
    print('Test 2: DRamTile compressed store/restore')
    dt = gf._lib.geofield_dt_init(64)
    assert dt, 'dt_init returned NULL'
    data2 = b'DRamTile compressed store test data! ' * 50
    print(f'  Input: {len(data2)} bytes')
    rc = gf.dt_store_compressed(dt, data2)
    assert rc == 0, f'Store failed: rc={rc}'
    print(f'  Store: rc={rc}')
    restored, rc2 = gf.dt_restore_compressed(dt, len(data2))
    assert rc2 == 0 and restored is not None, f'Restore failed: rc={rc2}'
    assert restored == data2, 'Roundtrip mismatch'
    print(f'  Restore: rc={rc2}')
    print(f'  Roundtrip: PASS')
    gf._lib.geofield_dt_destroy(dt)
    print()

def test_gpxl_v3_roundtrip():
    """Test GPXL v3 image sequence encode/decode."""
    print('Test 3: GPXL v3 encode/decode roundtrip')
    data = b'GPXL v3 image sequence format test data! ' * 200
    print(f'  Input: {len(data)} bytes')
    encoded, stats = cli.encode(data)
    print(f'  Encoded: {len(encoded)} bytes (ratio={len(encoded)/len(data):.2f}x)')
    print(f'  Segments: {stats["n_segments"]}  Blocks: {stats["total_blocks"]}')
    print(f'  GP level: {stats["gp_level"]}  Shell level: {stats["shell_level"]}')
    decoded, xxh_ok = cli.decode(encoded)
    assert xxh_ok, 'xxh64 mismatch'
    assert decoded == data, 'Roundtrip mismatch'
    print(f'  xxh64: PASS  Roundtrip: PASS')
    print()

def test_gpxl_v3_auto_scale():
    """Test GPXL v3 with different shell levels."""
    print('Test 4: GPXL v3 auto-scaling / shell levels')
    for size in [1000, 10000, 100000, 1000000]:
        data = bytes([i % 256 for i in range(size)])
        encoded, stats = cli.encode(data)
        decoded, xxh_ok = cli.decode(encoded)
        assert decoded == data, f'Roundtrip mismatch at {size} bytes'
        sl = stats['shell_level']
        gp = stats['gp_level']
        print(f'  {size:>7,}b -> {len(encoded):>7,}b ({len(encoded)/size:.2f}x) sl={sl} gp={gp} nseg={stats["n_segments"]}')
    print()

if __name__ == '__main__':
    test_gfcs_roundtrip()
    test_dt_compressed_roundtrip()
    test_gpxl_v3_roundtrip()
    test_gpxl_v3_auto_scale()
    print('ALL TESTS PASSED')
