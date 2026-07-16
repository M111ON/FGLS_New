"""
test_ds_vs_geopixel.py — Compare Diamond Shell classify vs Geopixel encode
on the same 64-byte blocks. Shows which codec handles which data patterns better.
"""
import sys, os, ctypes
sys.path.insert(0, os.path.dirname(__file__))
import geofield_ctypes as gf

# Load geopixel DLL (same pipeline DLL has pogls_geopixel functions)
dll = gf._lib
DS_SUB_N = 8
DS_SUB_SZ = 8


def ds_classify_block(block_bytes):
    """Classify 64B block with Diamond Shell, return (type, encoded_size_estimate)."""
    src = (ctypes.c_uint8 * 64)(*block_bytes)
    dc = dll.geofield_diamond_classify(src)
    # DS flag: 0=FLAT, 1=SPARSE, 2=DENSE
    type_names = {0: "FLAT", 1: "SPARSE", 2: "DENSE"}
    # Estimate encoded size: FLAT=2B, SPARSE=9B, DENSE=17B
    est_sizes = {0: 2, 1: 9, 2: 17}
    return type_names.get(dc.flag, "?"), est_sizes.get(dc.flag, 17)


def gp_classify_block(block_bytes):
    """Classify 64B block with Geopixel, return (type, encoded_size)."""
    src = (ctypes.c_uint8 * 64)(*block_bytes)
    enc = (ctypes.c_uint8 * 128)()
    wrote = dll.pogls_geopixel_encode_block(enc, 128, src, 64)
    tag = enc[0]
    tag_names = {0: "FLAT", 1: "SMOOTH", 2: "GRADIENT", 3: "EDGE"}
    return tag_names.get(tag, f"UNKNOWN({tag})"), wrote


def compare_file(path, label):
    """Compare Diamond Shell vs Geopixel on a file."""
    with open(path, 'rb') as f:
        data = f.read()

    n_blocks = (len(data) + 63) // 64

    # Classify all blocks with both codecs
    ds_types = {"FLAT": 0, "SPARSE": 0, "DENSE": 0}
    gp_types = {"FLAT": 0, "SMOOTH": 0, "GRADIENT": 0, "EDGE": 0}
    ds_total = 0
    gp_total = 0

    for bi in range(n_blocks):
        off = bi * 64
        bsz = min(64, len(data) - off)
        block = data[off:off + bsz]
        if bsz < 64:
            block = block + bytes(64 - bsz)

        ds_type, ds_sz = ds_classify_block(block)
        gp_type, gp_sz = gp_classify_block(block)

        ds_types[ds_type] += 1
        gp_types[gp_type] += 1
        ds_total += ds_sz
        gp_total += gp_sz

    print(f"\n  {label} ({len(data):,}B, {n_blocks} blocks)")
    print(f"    Diamond Shell: {ds_total:>8,}B  {ds_types}")
    print(f"    Geopixel:      {gp_total:>8,}B  {gp_types}")
    print(f"    Delta:         {ds_total - gp_total:>+8,}B  ({'Geopixel smaller' if gp_total < ds_total else 'Diamond Shell smaller'})")

    return ds_types, gp_types


def main():
    print("=" * 70)
    print("  Diamond Shell vs Geopixel — Block Classification Comparison")
    print("=" * 70)

    test_files = [
        ("pipeline/lettercube.h", "LetterCube header"),
        ("pipeline/geofield_pipeline.c", "Pipeline DLL source"),
        ("collection/geopixel/hbv_bundle/geo_flow_chunker.h", "Flow chunker"),
    ]

    for path, label in test_files:
        full = os.path.join(os.path.dirname(__file__), '..', path)
        if not os.path.exists(full):
            print(f"  SKIP: {path} not found")
            continue
        compare_file(full, label)

    # Edge cases
    print("\n  --- Edge cases ---")
    import random

    # All zeros
    zeros = bytes(256)
    n_blk = 256 // 64
    ds_t = {"FLAT": 0, "SPARSE": 0, "DENSE": 0}
    gp_t = {"FLAT": 0, "SMOOTH": 0, "GRADIENT": 0, "EDGE": 0}
    ds_s = gp_s = 0
    for bi in range(n_blk):
        block = zeros[bi*64:(bi+1)*64]
        dst, dsz = ds_classify_block(block)
        gst, gsz = gp_classify_block(block)
        ds_t[dst] += 1; gp_t[gst] += 1
        ds_s += dsz; gp_s += gsz
    print(f"\n  256B zeros:  DS={ds_s}B {ds_t}  GP={gp_s}B {gp_t}")

    # All 0xFF
    ones = bytes([0xFF]) * 256
    ds_t = {"FLAT": 0, "SPARSE": 0, "DENSE": 0}
    gp_t = {"FLAT": 0, "SMOOTH": 0, "GRADIENT": 0, "EDGE": 0}
    ds_s = gp_s = 0
    for bi in range(n_blk):
        block = ones[bi*64:(bi+1)*64]
        dst, dsz = ds_classify_block(block)
        gst, gsz = gp_classify_block(block)
        ds_t[dst] += 1; gp_t[gst] += 1
        ds_s += dsz; gp_s += gsz
    print(f"  256B 0xFF:   DS={ds_s}B {ds_t}  GP={gp_s}B {gp_t}")

    # Random
    random.seed(42)
    rand_data = bytes(random.getrandbits(8) for _ in range(1024))
    ds_t = {"FLAT": 0, "SPARSE": 0, "DENSE": 0}
    gp_t = {"FLAT": 0, "SMOOTH": 0, "GRADIENT": 0, "EDGE": 0}
    ds_s = gp_s = 0
    n_blk = 1024 // 64
    for bi in range(n_blk):
        block = rand_data[bi*64:(bi+1)*64]
        dst, dsz = ds_classify_block(block)
        gst, gsz = gp_classify_block(block)
        ds_t[dst] += 1; gp_t[gst] += 1
        ds_s += dsz; gp_s += gsz
    print(f"  1KB random:  DS={ds_s}B {ds_t}  GP={gp_s}B {gp_t}")

    # Gradient data
    gradient = bytes(range(256)) * 4
    ds_t = {"FLAT": 0, "SPARSE": 0, "DENSE": 0}
    gp_t = {"FLAT": 0, "SMOOTH": 0, "GRADIENT": 0, "EDGE": 0}
    ds_s = gp_s = 0
    n_blk = len(gradient) // 64
    for bi in range(n_blk):
        block = gradient[bi*64:(bi+1)*64]
        dst, dsz = ds_classify_block(block)
        gst, gsz = gp_classify_block(block)
        ds_t[dst] += 1; gp_t[gst] += 1
        ds_s += dsz; gp_s += gsz
    print(f"  256B grad:   DS={ds_s}B {ds_t}  GP={gp_s}B {gp_t}")

    print("\n" + "=" * 70)
    print("  Summary:")
    print("    FLAT:    Both → 2B (DS=flag+value, GP=tag+value)")
    print("    SMOOTH:  GP=10B (mean+residuals) vs DS SPARSE=9B or DENSE=17B")
    print("    GRADIENT: GP=10B (slope+intercept+residuals) — DS has no equivalent")
    print("    EDGE:    GP=65B (raw) vs DS non-FLAT=9-17B (rotation+sub-blocks)")
    print("    DS wins on: structured data with geometric rotation patterns")
    print("    GP wins on: smooth/gradient data with statistical patterns")
    print("=" * 70)


if __name__ == "__main__":
    main()
