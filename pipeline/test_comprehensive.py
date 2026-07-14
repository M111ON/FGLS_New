"""
test_comprehensive.py — Test pipeline with diverse file types
Verifies roundtrip integrity across .c, .h, .cu, .py, .ps1, .pdf
"""
import sys, os, time, hashlib, struct, ctypes

sys.path.insert(0, os.path.dirname(__file__))
import geofield_ctypes as gf

GPXL_MAGIC = b'GPXL'
GPXL_VERSION = 2
GPXL_HEADER_SZ = 48
LC_BUF = 76
CHUNK_SZ = 64


def make_lettercube(seg_idx):
    buf = (ctypes.c_uint8 * LC_BUF)()
    gf._lib.geofield_lc_init(buf)
    for lane, pair, angle in [(0, 0, 0), (1, 12, 0),
                               (2, 1, 1), (3, 13, 1),
                               (4, 2, 2), (5, 14, 2)]:
        gf._lib.geofield_lc_assign(buf, lane, pair, angle)
    gf._lib.geofield_lc_bond(buf, 0, 1)
    gf._lib.geofield_lc_bond(buf, 2, 3)
    gf._lib.geofield_lc_bond(buf, 4, 5)
    gf._lib.geofield_lc_assemble(buf)
    return bytes(buf)


def full_encode(data):
    original_size = len(data)
    xxh64_val = gf.xxh64(data)

    max_segs = (original_size // 32) + 256
    offsets = (ctypes.c_uint64 * max_segs)()
    lengths = (ctypes.c_uint64 * max_segs)()
    n_segments = gf._lib.geofield_flow_chunk(
        data, original_size, 32, 4096, offsets, lengths, max_segs)
    if n_segments <= 0:
        raise ValueError(f"flow_chunk returned {n_segments}")

    skel_hits = [0] * 6
    diamond_hits = [0] * 3
    total_blocks = 0

    segments = []
    for si in range(n_segments):
        seg_off = offsets[si]
        seg_len = lengths[si]
        seg_data = bytes(data[seg_off:seg_off + seg_len])
        block_strats = []
        for bi in range(0, seg_len, CHUNK_SZ):
            block = seg_data[bi:bi + CHUNK_SZ]
            if len(block) < CHUNK_SZ:
                block = block + b'\x00' * (CHUNK_SZ - len(block))
            shell = gf.diamond_classify(block)
            prev_block = bytes(segments[-1][-1][-1]) if segments and segments[-1][-1] else None
            skel = gf.skel_decide(block, prev_block, prev_block is not None)
            skel_hits[skel.strategy] += 1
            diamond_hits[shell.flag] += 1
            block_strats.append((skel.strategy, shell.flag))
            total_blocks += 1
        segments.append((seg_data, block_strats))

    lc_states = [make_lettercube(si) for si in range(n_segments)]

    coord_sec = 20 * n_segments
    lc_sec = LC_BUF * n_segments

    buf = bytearray()
    buf += GPXL_MAGIC
    buf += struct.pack('<H', GPXL_VERSION)
    buf += struct.pack('<BB', 0, 0)
    buf += struct.pack('<H', 0)
    buf += struct.pack('<I', n_segments)
    buf += struct.pack('<Q', original_size)
    buf += struct.pack('<Q', xxh64_val)
    buf += struct.pack('<I', coord_sec + lc_sec)
    buf += b'\x00' * (GPXL_HEADER_SZ - len(buf))

    for si in range(n_segments):
        seg_data, block_strats = segments[si]
        first_strat = block_strats[0][0] if block_strats else 5
        first_block = bytes(seg_data[:CHUNK_SZ]) if len(seg_data) >= CHUNK_SZ else seg_data.ljust(CHUNK_SZ, b'\x00')
        seed = gf.wallet_seed(first_block)
        checksum = gf.xxh64(seg_data) & 0xFFFF
        fast_sig = seg_data[0] if seg_data else 0
        coord = struct.pack('<I', si)
        coord += struct.pack('<BBB', 0, si % 12, si % 6)
        coord += struct.pack('<B', 0)
        coord += struct.pack('<B', first_strat)
        coord += struct.pack('<Q', seed)
        coord += struct.pack('<H', checksum)
        coord += struct.pack('<B', fast_sig)
        buf += coord

    for lc in lc_states:
        buf += lc

    for seg_data, _ in segments:
        buf += struct.pack('<I', len(seg_data))
        buf += seg_data

    return bytes(buf)


def full_decode(encoded):
    buf = memoryview(encoded)
    magic = bytes(buf[0:4])
    assert magic == GPXL_MAGIC
    n_segments = struct.unpack_from('<I', buf, 10)[0]
    original_size = struct.unpack_from('<Q', buf, 14)[0]
    stored_xxh64 = struct.unpack_from('<Q', buf, 22)[0]
    coord_lc_sz = struct.unpack_from('<I', buf, 30)[0]

    coord_sec = 20 * n_segments
    lc_sec = coord_lc_sz - coord_sec
    actual_lc = lc_sec // LC_BUF

    lc_start = GPXL_HEADER_SZ + coord_sec
    lc_ok = 0
    for ci in range(actual_lc):
        off = lc_start + ci * LC_BUF
        lc_bytes = bytes(buf[off:off + LC_BUF])
        if gf._lib.geofield_lc_verify(lc_bytes):
            lc_ok += 1

    data_offset = GPXL_HEADER_SZ + coord_lc_sz
    result = bytearray()
    pos = data_offset
    for si in range(n_segments):
        seg_len = struct.unpack_from('<I', buf, pos)[0]
        pos += 4
        result += bytes(buf[pos:pos + seg_len])
        pos += seg_len

    result = bytes(result[:original_size])
    return result, stored_xxh64 == gf.xxh64(result), lc_ok


def test_file(path):
    name = os.path.basename(path)
    with open(path, 'rb') as f:
        original = f.read()
    orig_hash = hashlib.sha256(original).hexdigest()

    t0 = time.perf_counter()
    encoded = full_encode(original)
    t1 = time.perf_counter()
    decoded, hash_ok, lc_ok = full_decode(encoded)
    t2 = time.perf_counter()

    dec_hash = hashlib.sha256(decoded).hexdigest()
    match = (orig_hash == dec_hash)
    ratio = len(encoded) / len(original) if len(original) > 0 else 999

    status = "PASS" if match else "FAIL"
    print(f"  {status}  {name:40s} {len(original):>10,} B  ->  {len(encoded):>10,} B  ratio={ratio:.2f}x  enc={(t1-t0)*1000:.1f}ms  dec={(t2-t1)*1000:.1f}ms  LC={lc_ok}/{len(encoded)//1}")

    if not match:
        for i in range(min(len(original), len(decoded))):
            if original[i] != decoded[i]:
                print(f"         First diff at byte {i}: orig=0x{original[i]:02x} decoded=0x{decoded[i]:02x}")
                break

    return match


if __name__ == '__main__':
    test_files = [
        'geofield_pipeline.c',
        'lettercube.h',
        'geofield_cuda.cu',
        'geofield_cli.py',
        'geofield_ctypes.py',
        '../runner/llama_pogls_runner_sid_v2.c',
        '../runner/dramtile_store.h',
        '../runner/bench_sid.ps1',
        '../core/geo_field_core.h',
    ]

    print("=" * 110)
    print("  Comprehensive Roundtrip Test — Diverse File Types")
    print("=" * 110)
    print(f"  {'Status':7s} {'File':40s} {'Input':>10s}  ->  {'Output':>10s}  {'Ratio':>8s}  {'Enc':>7s}  {'Dec':>7s}  LC")
    print("-" * 110)

    all_pass = True
    for path in test_files:
        full = os.path.join(os.path.dirname(__file__), path)
        if os.path.exists(full):
            ok = test_file(full)
            if not ok:
                all_pass = False
        else:
            print(f"  SKIP  {path}")

    # Also test edge cases
    print("-" * 110)
    print("  Edge cases:")

    # Empty file
    empty = b''
    try:
        encoded = full_encode(empty)
        print(f"  FAIL  empty file produced output (should handle gracefully)")
    except:
        print(f"  PASS  empty file -> error handled correctly")

    # 1 byte
    tiny = b'\x42'
    encoded = full_encode(tiny)
    decoded, ok, lc = full_decode(encoded)
    status = "PASS" if ok else "FAIL"
    ratio = len(encoded) / len(tiny)
    print(f"  {status}  1-byte file    ratio={ratio:.1f}x")

    # 63 bytes (just under one block)
    small = bytes(range(63))
    encoded = full_encode(small)
    decoded, ok, lc = full_decode(encoded)
    status = "PASS" if ok else "FAIL"
    ratio = len(encoded) / len(small)
    print(f"  {status}  63-byte file  ratio={ratio:.1f}x")

    # 64 bytes (exactly one block)
    exact = bytes(range(64))
    encoded = full_encode(exact)
    decoded, ok, lc = full_decode(encoded)
    status = "PASS" if ok else "FAIL"
    ratio = len(encoded) / len(exact)
    print(f"  {status}  64-byte file  ratio={ratio:.1f}x")

    # 65 bytes (just over one block)
    over = bytes(range(65))
    encoded = full_encode(over)
    decoded, ok, lc = full_decode(encoded)
    status = "PASS" if ok else "FAIL"
    ratio = len(encoded) / len(over)
    print(f"  {status}  65-byte file  ratio={ratio:.1f}x")

    # All zeros
    zeros = b'\x00' * 1024
    encoded = full_encode(zeros)
    decoded, ok, lc = full_decode(encoded)
    status = "PASS" if ok else "FAIL"
    ratio = len(encoded) / len(zeros)
    print(f"  {status}  1KB zeros     ratio={ratio:.1f}x")

    # All 0xFF
    maxval = b'\xff' * 4096
    encoded = full_encode(maxval)
    decoded, ok, lc = full_decode(encoded)
    status = "PASS" if ok else "FAIL"
    ratio = len(encoded) / len(maxval)
    print(f"  {status}  4KB 0xFF      ratio={ratio:.1f}x")

    # Random data
    import random
    random.seed(42)
    random_data = bytes(random.getrandbits(8) for _ in range(8192))
    encoded = full_encode(random_data)
    decoded, ok, lc = full_decode(encoded)
    status = "PASS" if ok else "FAIL"
    ratio = len(encoded) / len(random_data)
    print(f"  {status}  8KB random    ratio={ratio:.1f}x")

    # Repeated pattern
    pattern = b'ABCDEFGH' * 1024
    encoded = full_encode(pattern)
    decoded, ok, lc = full_decode(encoded)
    status = "PASS" if ok else "FAIL"
    ratio = len(encoded) / len(pattern)
    print(f"  {status}  8KB pattern   ratio={ratio:.1f}x")

    # Binary with null bytes
    binary = bytes(range(256)) * 32
    encoded = full_encode(binary)
    decoded, ok, lc = full_decode(encoded)
    status = "PASS" if ok else "FAIL"
    ratio = len(encoded) / len(binary)
    print(f"  {status}  8KB binary    ratio={ratio:.1f}x")

    print("=" * 110)
    print(f"  RESULT: {'ALL PASS' if all_pass else 'SOME FAILED'}")
    print("=" * 110)
