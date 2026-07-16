"""
test_ds_roundtrip.py — Diamond Shell classification + compression roundtrip test
Verifies: structure → classify → decode → xxh64 match
          compress (codebook dedup) → decompress → xxh64 match
"""
import sys, os, ctypes, hashlib
sys.path.insert(0, os.path.dirname(__file__))
import geofield_ctypes as gf

dll = gf._lib
SKEL = ['ID','FLAT','DIFF','BREF','GEOM','RAW']
DSMOND = ['FLAT','SPARSE','DENSE']


def test_structure(label, data):
    """Classify data, decode, verify xxh64 match."""
    data_buf = (ctypes.c_uint8 * len(data))(*data)
    stats = gf.GFStructureStats()

    rc = dll.geofield_full_structure_stats(data_buf, len(data), 32, 4096, ctypes.byref(stats))
    if rc != 0:
        print(f"  {label}: dry run failed rc={rc}")
        return False

    header_sz = 32
    idx_sz = stats.n_segments * 12
    total_out = header_sz + idx_sz + stats.struct_size
    out_buf = (ctypes.c_uint8 * total_out)()

    rc = dll.geofield_full_structure(data_buf, len(data), 32, 4096,
                                   out_buf, total_out, ctypes.byref(stats))
    if rc != 0:
        print(f"  {label}: structure failed rc={rc}")
        return False

    n_blocks_aligned = ((len(data) + 63) // 64) * 64
    dec_buf = (ctypes.c_uint8 * n_blocks_aligned)()
    got_xxh = ctypes.c_uint64()
    rc = dll.geofield_full_decode(out_buf, total_out,
                                   dec_buf, len(data), ctypes.byref(got_xxh))
    if rc == -2:
        print(f"  {label}: xxh64 MISMATCH stored=0x{stats.xxh64:016x} got=0x{got_xxh.value:016x}")
        return False
    elif rc != 0:
        print(f"  {label}: decode failed rc={rc}")
        return False

    match = (bytes(data) == bytes(dec_buf[:len(data)]))
    struct_total = header_sz + idx_sz + stats.struct_size
    flat_ct = stats.diamond_hits[0]
    sparse_ct = stats.diamond_hits[1]
    dense_ct = stats.diamond_hits[2]
    verdict = "SAVED" if struct_total < len(data) else "EXPANDED"
    delta = len(data) - struct_total

    print(f"  {label:20s} {len(data):>8,}B -> {struct_total:>8,}B  {verdict} {abs(delta):,}B  blocks={stats.n_blocks}")
    print(f"    Diamond: FLAT={flat_ct} SPARSE={sparse_ct} DENSE={dense_ct}")
    print(f"    xxh64: 0x{stats.xxh64:016x}  wall: {stats.wall_ms:.3f}ms")
    print(f"    Structure: {'PASS' if match else 'FAIL'}")
    return match


def test_compress(label, data):
    """Compress (codebook dedup), decompress, verify xxh64 match."""
    data_buf = (ctypes.c_uint8 * len(data))(*data)
    stats = gf.GFCSStats()

    # Dry run to get sizes
    rc = dll.geofield_full_compress(data_buf, len(data), 32, 4096,
                                    None, 0, ctypes.byref(stats))
    if rc != 0:
        print(f"  {label}: compress dry run failed rc={rc}")
        return False

    total_out = stats.total_out
    out_buf = (ctypes.c_uint8 * total_out)()

    rc = dll.geofield_full_compress(data_buf, len(data), 32, 4096,
                                    out_buf, total_out, ctypes.byref(stats))
    if rc != 0:
        print(f"  {label}: compress failed rc={rc}")
        return False

    n_blocks_aligned = ((len(data) + 63) // 64) * 64
    dec_buf = (ctypes.c_uint8 * n_blocks_aligned)()
    got_xxh = ctypes.c_uint64()
    rc = dll.geofield_full_decompress(out_buf, total_out,
                                       dec_buf, len(data), ctypes.byref(got_xxh))
    if rc != 0:
        print(f"  {label}: decompress rc={rc}  stored_xxh=0x{stats.xxh64:016x} got_xxh=0x{got_xxh.value:016x} total_out={stats.total_out} comp_size={stats.comp_size} n_patterns={stats.n_patterns}")
        return False

    match = (bytes(data) == bytes(dec_buf[:len(data)]))
    flat_ct = stats.diamond_hits[0]
    sparse_ct = stats.diamond_hits[1]
    dense_ct = stats.diamond_hits[2]
    verdict = "SAVED" if stats.total_out < len(data) else "EXPANDED"
    delta = len(data) - stats.total_out

    print(f"  {label:20s} {len(data):>8,}B -> {stats.total_out:>8,}B  {verdict} {abs(delta):,}B  blocks={stats.n_blocks} patterns={stats.n_patterns}")
    print(f"    Diamond: FLAT={flat_ct} SPARSE={sparse_ct} DENSE={dense_ct}")
    print(f"    Codebook: {stats.n_patterns} unique patterns ({stats.n_patterns * 2}B)")
    print(f"    xxh64: 0x{stats.xxh64:016x}  wall: {stats.wall_ms:.3f}ms")
    print(f"    Compress: {'PASS' if match else 'FAIL'}")
    return match


def main():
    print("=" * 70)
    print("  Diamond Shell — Classification + Compression Roundtrip Test")
    print("=" * 70)

    test_files = [
        ("pipeline/lettercube.h",    "LetterCube header"),
        ("pipeline/geofield_pipeline.c", "Pipeline DLL source"),
        ("collection/geopixel/hbv_bundle/geo_flow_chunker.h", "Flow chunker"),
    ]

    all_pass = True
    for path, label in test_files:
        full = os.path.join(os.path.dirname(__file__), '..', path)
        if not os.path.exists(full):
            print(f"  SKIP: {path} not found")
            continue
        with open(full, 'rb') as f:
            data = f.read()

        print(f"\n  --- {label} ---")
        ok1 = test_structure(label, data)
        all_pass = all_pass and ok1
        print()
        ok2 = test_compress(label, data)
        all_pass = all_pass and ok2
        print()

    # Edge cases
    print("--- Edge cases ---")
    for label, data in [
        ("256B zeros", bytes(256)),
        ("128B 0xFF", bytes([0xFF]) * 128),
        ("1KB random", bytes(__import__('random').seed(42) or __import__('random').getrandbits(8) for _ in range(1024))),
        ("64B single block", bytes(range(64))),
    ]:
        print(f"\n  --- {label} ---")
        ok1 = test_structure(label, data)
        all_pass = all_pass and ok1
        print()
        ok2 = test_compress(label, data)
        all_pass = all_pass and ok2
        print()

    print("=" * 70)
    print(f"  RESULT: {'ALL PASS' if all_pass else 'SOME FAILED'}")
    print("=" * 70)
    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
