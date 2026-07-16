"""
test_pipeline_e2e.py — GeoField → GeoPixel end-to-end pipeline test

Pipeline:
  raw data → Hilbert scatter (geo_field) → GeoPixel encode → GeoPixel decode → Hilbert gather → compare

This tests whether Hilbert rearrangement creates spatial coherence
that geopixel can compress (FLAT/SMOOTH/GRADIENT instead of all EDGE).
"""
import sys, os, ctypes, time
sys.path.insert(0, os.path.dirname(__file__))
import geofield_ctypes as gf

dll = gf._lib


def hilbert_scatter(src_64):
    """Scatter 64B block via Hilbert curve → 8×8 grid."""
    src = (ctypes.c_uint8 * 64)(*src_64)
    dst = (ctypes.c_uint8 * 64)()
    dll.geofield_hilbert_scatter_8x8(dst, src)
    return bytes(dst)


def hilbert_gather(grid_64):
    """Gather 8×8 grid via Hilbert curve → 64B block."""
    src = (ctypes.c_uint8 * 64)(*grid_64)
    dst = (ctypes.c_uint8 * 64)()
    dll.geofield_hilbert_gather_8x8(dst, src)
    return bytes(dst)


def gp_encode(src_64):
    """Geopixel encode 64B block → tagged stream."""
    src = (ctypes.c_uint8 * 64)(*src_64)
    enc = (ctypes.c_uint8 * 128)()
    wrote = dll.pogls_geopixel_encode_block(enc, 128, src, 64)
    return bytes(enc[:wrote]), wrote


def gp_decode(tagged):
    """Geopixel decode tagged block → 64B."""
    src = (ctypes.c_uint8 * len(tagged))(*tagged)
    dec = (ctypes.c_uint8 * 64)()
    wrote = dll.pogls_geopixel_decode_block(dec, 64, src, len(tagged))
    return bytes(dec[:wrote])


def classify_tag(tag):
    return {0: "FLAT", 1: "SMOOTH", 2: "GRADIENT", 3: "EDGE"}.get(tag, "?")


def test_roundtrip():
    """Test: Hilbert scatter → geopixel encode → decode → Hilbert gather = original."""
    print("=" * 60)
    print("  Test 1: Roundtrip (scatter → encode → decode → gather)")
    print("=" * 60)

    test_data = [
        bytes(range(64)),                          # gradient 0-63
        bytes([0x42] * 64),                        # flat
        bytes(range(0, 256, 4)) * 2,              # sawtooth
    ]
    labels = ["gradient 0-63", "flat 0x42", "sawtooth"]

    for data, label in zip(test_data, labels):
        scattered = hilbert_scatter(data)
        enc, sz = gp_encode(scattered)
        dec = gp_decode(enc)
        gathered = hilbert_gather(dec)

        tag = enc[0]
        match = gathered == data
        print(f"  {label:20s}  tag={classify_tag(tag):8s}  enc={sz:3d}B  roundtrip={'PASS' if match else 'FAIL'}")


def test_geopixel_benefits():
    """Test: compare geopixel on raw vs Hilbert-scattered data."""
    print("\n" + "=" * 60)
    print("  Test 2: GeoPixel on raw vs Hilbert-scattered")
    print("=" * 60)

    # Create test data with patterns
    tests = {
        "gradient": bytes(range(256)),
        "repeated pattern": bytes([0x10, 0x20, 0x30, 0x40] * 64),
        "random": bytes([((i * 7 + 13) & 0xFF) for i in range(256)]),
    }

    for name, data in tests.items():
        n_blocks = len(data) // 64

        # Raw: geopixel on original bytes
        raw_tags = {}
        raw_total = 0
        for bi in range(n_blocks):
            block = data[bi*64:(bi+1)*64]
            enc, sz = gp_encode(block)
            tag = classify_tag(enc[0])
            raw_tags[tag] = raw_tags.get(tag, 0) + 1
            raw_total += sz

        # Scattered: Hilbert scatter first, then geopixel
        scat_tags = {}
        scat_total = 0
        for bi in range(n_blocks):
            block = data[bi*64:(bi+1)*64]
            scattered = hilbert_scatter(block)
            enc, sz = gp_encode(scattered)
            tag = classify_tag(enc[0])
            scat_tags[tag] = scat_tags.get(tag, 0) + 1
            scat_total += sz

        print(f"\n  {name}:")
        print(f"    Raw:       {raw_total:5d}B  {raw_tags}")
        print(f"    Scattered: {scat_total:5d}B  {scat_tags}")
        print(f"    Delta:     {scat_total - raw_total:+5d}B  ({'scattered better' if scat_total < raw_total else 'raw better'})")


def test_full_pipeline():
    """Test: full pipeline with real file."""
    print("\n" + "=" * 60)
    print("  Test 3: Full pipeline on real file")
    print("=" * 60)

    path = os.path.join(os.path.dirname(__file__), '..', 'pipeline', 'geofield_pipeline.c')
    if not os.path.exists(path):
        path = os.path.join(os.path.dirname(__file__), '..', 'pipeline', 'lettercube.h')
    if not os.path.exists(path):
        print("  SKIP: no test file found")
        return

    with open(path, 'rb') as f:
        data = f.read()

    n_blocks = (len(data) + 63) // 64
    print(f"  File: {os.path.basename(path)} ({len(data):,}B, {n_blocks} blocks)")

    # Method 1: Direct geopixel (no rearrangement)
    t0 = time.perf_counter()
    direct_tags = {}
    direct_total = 0
    for bi in range(n_blocks):
        off = bi * 64
        bsz = min(64, len(data) - off)
        block = data[off:off+bsz]
        if bsz < 64:
            block = block + bytes(64 - bsz)
        enc, sz = gp_encode(block)
        tag = classify_tag(enc[0])
        direct_tags[tag] = direct_tags.get(tag, 0) + 1
        direct_total += sz
    t1 = time.perf_counter()

    # Method 2: Hilbert scatter → geopixel
    t2 = time.perf_counter()
    scat_tags = {}
    scat_total = 0
    for bi in range(n_blocks):
        off = bi * 64
        bsz = min(64, len(data) - off)
        block = data[off:off+bsz]
        if bsz < 64:
            block = block + bytes(64 - bsz)
        scattered = hilbert_scatter(block)
        enc, sz = gp_encode(scattered)
        tag = classify_tag(enc[0])
        scat_tags[tag] = scat_tags.get(tag, 0) + 1
        scat_total += sz
    t3 = time.perf_counter()

    # Method 3: Diamond Shell classify (for comparison)
    t4 = time.perf_counter()
    ds_types = {}
    ds_total = 0
    for bi in range(n_blocks):
        off = bi * 64
        bsz = min(64, len(data) - off)
        block = data[off:off+bsz]
        if bsz < 64:
            block = block + bytes(64 - bsz)
        src = (ctypes.c_uint8 * 64)(*block)
        dc = dll.geofield_diamond_classify(src)
        type_names = {0: "FLAT", 1: "SPARSE", 2: "DENSE"}
        est = {0: 2, 1: 9, 2: 17}
        t = type_names.get(dc.flag, "?")
        ds_types[t] = ds_types.get(t, 0) + 1
        ds_total += est.get(dc.flag, 17)
    t5 = time.perf_counter()

    print(f"\n  Direct geopixel:   {direct_total:8,}B  {direct_tags}  ({(t1-t0)*1000:.1f}ms)")
    print(f"  Hilbert + geopixel:{scat_total:8,}B  {scat_tags}  ({(t3-t2)*1000:.1f}ms)")
    print(f"  Diamond Shell:     {ds_total:8,}B  {ds_types}  ({(t5-t4)*1000:.1f}ms)")
    print(f"\n  Hilbert rearrangement {'HELPS' if scat_tags.get('EDGE', 0) < direct_tags.get('EDGE', 0) else 'DOES NOT HELP'} geopixel")


if __name__ == "__main__":
    test_roundtrip()
    test_geopixel_benefits()
    test_full_pipeline()
