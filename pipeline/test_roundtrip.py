"""
test_roundtrip.py — Phase A4: Adaptive chunk roundtrip test

Flow: file → flow_chunk → encode per-segment → serialize → deserialize → reassemble → verify
"""
import sys, os, time, ctypes, hashlib, struct

sys.path.insert(0, os.path.dirname(__file__))
import geofield_ctypes as gf

CHUNK_SZ = 64  # Diamond Shell / skeleton operate on 64B blocks
GPXL_MAGIC = b'GPXL'
GPXL_VERSION = 2
GPXL_HEADER_SZ = 48


def roundtrip_encode(data):
    """Encode data into geofield format. Returns (encoded_bytes, stats)."""
    original_size = len(data)
    xxh64 = gf.xxh64(data)

    # Step 1: Adaptive chunking
    max_segs = (original_size // 32) + 256
    offsets = (ctypes.c_uint64 * max_segs)()
    lengths = (ctypes.c_uint64 * max_segs)()
    n_segments = gf._lib.geofield_flow_chunk(
        data, original_size, 32, 4096, offsets, lengths, max_segs
    )
    if n_segments <= 0:
        raise ValueError(f"flow_chunk returned {n_segments}")

    stats = {
        'n_segments': n_segments,
        'original_size': original_size,
        'xxh64': xxh64,
        'skel_hits': [0] * 6,
        'shell_hits': [0] * 3,  # FLAT, SPARSE, DENSE
    }

    # Step 2: Process each segment
    # For each segment, split into 64B blocks for Diamond Shell / skeleton
    segments = []  # list of (raw_bytes, [per-block skel_strategy])
    total_blocks = 0

    for si in range(n_segments):
        seg_off = offsets[si]
        seg_len = lengths[si]
        seg_data = bytes(data[seg_off:seg_off + seg_len])

        # Split segment into 64B blocks
        block_strategies = []
        for bi in range(0, seg_len, CHUNK_SZ):
            block = seg_data[bi:bi + CHUNK_SZ]
            if len(block) < CHUNK_SZ:
                block = block + b'\x00' * (CHUNK_SZ - len(block))

            # Diamond Shell classify
            shell_result = gf.diamond_classify(block)
            shell_flag = shell_result.flag

            # Skeleton classify
            prev_block = bytes(segments[-1][-1][-1]) if segments and segments[-1][-1] else None
            skel_result = gf.skel_decide(block, prev_block, prev_block is not None)
            skel_strategy = skel_result.strategy

            stats['skel_hits'][skel_strategy] += 1
            stats['shell_hits'][shell_flag] += 1

            block_strategies.append((skel_strategy, shell_flag))
            total_blocks += 1

        segments.append((seg_data, block_strategies))

    stats['total_blocks'] = total_blocks

    # Step 3: Serialize
    # Format: [header 48B] [coord_section 20B × n_segments] [segment_data...]
    buf = bytearray()

    # Compute coord section size
    coord_section_sz = 20 * n_segments

    # Header
    buf += GPXL_MAGIC                                    # 4B
    buf += struct.pack('<H', GPXL_VERSION)               # 2B
    buf += struct.pack('<B', 0)                          # gp_level 1B
    buf += struct.pack('<B', 0)                          # base 1B
    buf += struct.pack('<H', 0)                          # side 2B
    buf += struct.pack('<I', n_segments)                 # n_chunks = n_segments 4B
    buf += struct.pack('<Q', original_size)              # 8B
    buf += struct.pack('<Q', xxh64)                      # xxh64 8B
    buf += struct.pack('<I', coord_section_sz)           # coord_sec_size 4B
    buf += b'\x00' * (GPXL_HEADER_SZ - len(buf))        # pad to 48B

    assert len(buf) == GPXL_HEADER_SZ, f"Header size mismatch: {len(buf)} != {GPXL_HEADER_SZ}"

    # Coord records (one per segment)
    for si in range(n_segments):
        seg_data, block_strats = segments[si]
        first_strategy = block_strats[0][0] if block_strats else 5  # RAW
        first_block = bytes(seg_data[:CHUNK_SZ]) if len(seg_data) >= CHUNK_SZ else seg_data.ljust(CHUNK_SZ, b'\x00')
        seed = gf.wallet_seed(first_block)
        checksum = gf.xxh64(seg_data) & 0xFFFF
        fast_sig = seg_data[0] if seg_data else 0

        coord = struct.pack('<I', si)                    # tile_id (segment index)
        coord += struct.pack('<B', 0)                    # dim
        coord += struct.pack('<B', si % 12)              # face
        coord += struct.pack('<B', si % 6)               # edge
        coord += struct.pack('<B', 0)                    # z
        coord += struct.pack('<B', first_strategy)       # skel_strategy
        coord += struct.pack('<Q', seed)                 # seed 8B
        coord += struct.pack('<H', checksum)             # checksum 2B
        coord += struct.pack('<B', fast_sig)             # fast_sig 1B
        buf += coord

    # Segment data (raw bytes, length-prefixed)
    for seg_data, _ in segments:
        buf += struct.pack('<I', len(seg_data))
        buf += seg_data

    return bytes(buf), stats


def roundtrip_decode(encoded):
    """Decode geofield format back to original data. Returns (data, ok)."""
    buf = memoryview(encoded)

    # Header
    magic = bytes(buf[0:4])
    assert magic == GPXL_MAGIC, f"Bad magic: {magic}"
    version = struct.unpack_from('<H', buf, 4)[0]
    n_segments = struct.unpack_from('<I', buf, 10)[0]
    original_size = struct.unpack_from('<Q', buf, 14)[0]
    stored_xxh64 = struct.unpack_from('<Q', buf, 22)[0]
    coord_sec_sz = struct.unpack_from('<I', buf, 30)[0]

    # Skip coord records
    data_offset = GPXL_HEADER_SZ + coord_sec_sz

    # Read segments
    result = bytearray()
    pos = data_offset
    for si in range(n_segments):
        seg_len = struct.unpack_from('<I', buf, pos)[0]
        pos += 4
        seg_data = bytes(buf[pos:pos + seg_len])
        pos += seg_len
        result += seg_data

    # Truncate to original size (remove zero padding)
    result = bytes(result[:original_size])

    # Verify xxh64
    actual_xxh64 = gf.xxh64(result)
    ok = (actual_xxh64 == stored_xxh64)

    return result, ok


def test_roundtrip(path):
    """Test encode→decode→verify on a file."""
    with open(path, 'rb') as f:
        original = f.read()
    original_hash = hashlib.sha256(original).hexdigest()

    name = os.path.basename(path)

    # Encode
    t0 = time.perf_counter()
    encoded, stats = roundtrip_encode(original)
    t1 = time.perf_counter()

    # Decode
    decoded, ok = roundtrip_decode(encoded)
    t2 = time.perf_counter()

    decoded_hash = hashlib.sha256(decoded).hexdigest()
    match = (original_hash == decoded_hash)

    ratio = len(encoded) / len(original) if len(original) > 0 else 999

    print(f"\n{name} ({len(original):,} bytes)")
    print(f"  Segments: {stats['n_segments']}  Blocks: {stats['total_blocks']}")
    print(f"  Skeleton: ID={stats['skel_hits'][0]} FLAT={stats['skel_hits'][1]} "
          f"DIFF={stats['skel_hits'][2]} BREF={stats['skel_hits'][3]} "
          f"GEOM={stats['skel_hits'][4]} RAW={stats['skel_hits'][5]}")
    print(f"  Diamond:  FLAT={stats['shell_hits'][0]} SPARSE={stats['shell_hits'][1]} DENSE={stats['shell_hits'][2]}")
    print(f"  Encoded: {len(encoded):,} bytes  ratio={ratio:.2f}x")
    print(f"  Encode: {(t1-t0)*1000:.1f}ms  Decode: {(t2-t1)*1000:.1f}ms")
    print(f"  xxh64 match: {ok}  SHA256 match: {match}")

    if not match:
        print(f"  ORIGINAL hash: {original_hash}")
        print(f"  DECODED  hash: {decoded_hash}")
        # Show first difference
        for i in range(min(len(original), len(decoded))):
            if original[i] != decoded[i]:
                print(f"  First diff at byte {i}: orig=0x{original[i]:02x} decoded=0x{decoded[i]:02x}")
                break

    return match


if __name__ == '__main__':
    test_files = [
        '../tools/geopixel_pipeline.py',
        '../runner/llama_pogls_runner_sid_v2.c',
        '../core/geo_field_core.h',
        '../collection/geopixel/hbv_bundle/geo_flow_chunker_v8.h',
    ]

    print("=" * 60)
    print("Phase A4: Adaptive Chunk Roundtrip Test")
    print("=" * 60)

    all_pass = True
    for path in test_files:
        full = os.path.join(os.path.dirname(__file__), path)
        if os.path.exists(full):
            ok = test_roundtrip(full)
            if not ok:
                all_pass = False

    print("\n" + "=" * 60)
    print(f"RESULT: {'ALL PASS' if all_pass else 'SOME FAILED'}")
    print("=" * 60)
