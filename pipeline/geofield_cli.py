#!/usr/bin/env python3
"""
geofield CLI — GeoField Pipeline command-line tool

Usage:
  python geofield_cli.py encode <input> [-o <output>] [--level N]
  python geofield_cli.py decode <input> [-o <output>]
  python geofield_cli.py info <input>
  python geofield_cli.py benchmark [file] [--size N]
  python geofield_cli.py verify <input>
  python geofield_cli.py batch <dir> [-o <outdir>]

Modes:
  encode   — File → GPXL (adaptive chunks + skeleton + diamond + LetterCube)
  decode   — GPXL → File (roundtrip restore)
  info     — Show GPXL metadata (segments, skeleton stats, ratio)
  benchmark — Run pipeline benchmark
  verify   — Encode → Decode → Compare hash
  batch    — Encode all files in a directory
"""
import sys, os, time, hashlib, struct, argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geofield_ctypes as gf

CHUNK_SZ = 64
GPXL_MAGIC = b'GPXL'
GPXL_VERSION = 2
GPXL_HEADER_SZ = 48
LC_BUF_SZ = 76

SKEL_NAMES = ["ID", "FLAT", "DIFF", "BREF", "GEOM", "RAW"]
DIAMOND_NAMES = ["FLAT", "SPARSE", "DENSE"]


# ════════════════════════════════════════════════════════════════
# ENCODE
# ════════════════════════════════════════════════════════════════

def encode(data, base_level=2):
    """Encode raw data → GPXL format."""
    original_size = len(data)
    xxh64_val = gf.xxh64(data)
    t0 = time.perf_counter()

    # Step 1: Adaptive chunking
    max_segs = (original_size // 32) + 256
    offsets = (ctypes.c_uint64 * max_segs)()
    lengths = (ctypes.c_uint64 * max_segs)()
    n_segments = gf._lib.geofield_flow_chunk(
        (ctypes.c_uint8 * original_size)(*data), original_size,
        32, 4096, offsets, lengths, max_segs
    )

    stats = {
        'n_segments': n_segments,
        'original_size': original_size,
        'xxh64': xxh64_val,
        'skel_hits': [0] * 6,
        'shell_hits': [0] * 3,
        'total_blocks': 0,
    }

    # Step 2-4: Per-segment processing
    segments = []
    lc_states = []

    for si in range(n_segments):
        seg_off = offsets[si]
        seg_len = lengths[si]
        seg_data = bytes(data[seg_off:seg_off + seg_len])

        # Diamond + Skeleton per block
        block_strategies = []
        for bi in range(0, seg_len, CHUNK_SZ):
            block = seg_data[bi:bi + CHUNK_SZ]
            if len(block) < CHUNK_SZ:
                block = block + b'\x00' * (CHUNK_SZ - len(block))

            shell_result = gf.diamond_classify(block)
            prev_block = bytes(segments[-1][-1][-1]) if segments and segments[-1][-1] else None
            skel_result = gf.skel_decide(block, prev_block, prev_block is not None)

            stats['skel_hits'][skel_result.strategy] += 1
            stats['shell_hits'][shell_result.flag] += 1
            block_strategies.append((skel_result.strategy, shell_result.flag))
            stats['total_blocks'] += 1

        segments.append((seg_data, block_strategies))

        # LetterCube per segment
        lc_buf = gf.lc_create_with_bonds([
            (0, 0), (12, 1), (1, 0), (13, 1), (2, 0), (14, 1)
        ])
        lc_states.append(lc_buf)

    t_encode = time.perf_counter() - t0

    # Step 5: Serialize
    coord_section_sz = 20 * n_segments
    lc_section_sz = LC_BUF_SZ * n_segments

    buf = bytearray()

    # Header
    buf += GPXL_MAGIC
    buf += struct.pack('<H', GPXL_VERSION)
    buf += struct.pack('<B', base_level)
    buf += struct.pack('<B', 0)
    buf += struct.pack('<H', 0)
    buf += struct.pack('<I', n_segments)
    buf += struct.pack('<Q', original_size)
    buf += struct.pack('<Q', xxh64_val)
    buf += struct.pack('<I', coord_section_sz + lc_section_sz)
    buf += b'\x00' * (GPXL_HEADER_SZ - len(buf))

    # Coord records
    for si in range(n_segments):
        seg_data, block_strats = segments[si]
        first_strategy = block_strats[0][0] if block_strats else 5
        first_block = bytes(seg_data[:CHUNK_SZ]) if len(seg_data) >= CHUNK_SZ else seg_data.ljust(CHUNK_SZ, b'\x00')
        seed = gf.wallet_seed(first_block)
        checksum = gf.xxh64(seg_data) & 0xFFFF
        fast_sig = seg_data[0] if seg_data else 0

        coord = struct.pack('<I', si)
        coord += struct.pack('<B', 0)
        coord += struct.pack('<B', si % 12)
        coord += struct.pack('<B', si % 6)
        coord += struct.pack('<B', 0)
        coord += struct.pack('<B', first_strategy)
        coord += struct.pack('<Q', seed)
        coord += struct.pack('<H', checksum)
        coord += struct.pack('<B', fast_sig)
        buf += coord

    # LetterCube section
    for lc_buf in lc_states:
        buf += bytes(lc_buf)

    # Segment data
    for seg_data, _ in segments:
        buf += struct.pack('<I', len(seg_data))
        buf += seg_data

    encoded = bytes(buf)
    stats['encode_time'] = t_encode
    stats['encoded_size'] = len(encoded)
    stats['ratio'] = len(encoded) / original_size if original_size > 0 else 0

    return encoded, stats


def decode(encoded):
    """Decode GPXL → raw data."""
    buf = memoryview(encoded)
    magic = bytes(buf[0:4])
    if magic != GPXL_MAGIC:
        raise ValueError(f"Bad magic: {magic}")

    n_segments = struct.unpack_from('<I', buf, 10)[0]
    original_size = struct.unpack_from('<Q', buf, 14)[0]
    stored_xxh64 = struct.unpack_from('<Q', buf, 22)[0]
    coord_lc_sz = struct.unpack_from('<I', buf, 30)[0]

    data_offset = GPXL_HEADER_SZ + coord_lc_sz

    result = bytearray()
    pos = data_offset
    for si in range(n_segments):
        seg_len = struct.unpack_from('<I', buf, pos)[0]
        pos += 4
        result += buf[pos:pos + seg_len]
        pos += seg_len

    result = bytes(result[:original_size])
    return result, gf.xxh64(result) == stored_xxh64


# ════════════════════════════════════════════════════════════════
# CLI COMMANDS
# ════════════════════════════════════════════════════════════════

def cmd_encode(args):
    """Encode file to GPXL."""
    with open(args.input, 'rb') as f:
        data = f.read()

    name = os.path.basename(args.input)
    print(f"Encoding: {name} ({len(data):,} bytes)")

    encoded, stats = encode(data)

    out_path = args.output or (args.input + '.gpxl')
    with open(out_path, 'wb') as f:
        f.write(encoded)

    print(f"  Segments: {stats['n_segments']}  Blocks: {stats['total_blocks']}")
    print(f"  Skeleton: ID={stats['skel_hits'][0]} FLAT={stats['skel_hits'][1]} "
          f"DIFF={stats['skel_hits'][2]} BREF={stats['skel_hits'][3]} "
          f"GEOM={stats['skel_hits'][4]} RAW={stats['skel_hits'][5]}")
    print(f"  Diamond: FLAT={stats['shell_hits'][0]} SPARSE={stats['shell_hits'][1]} DENSE={stats['shell_hits'][2]}")
    print(f"  Output: {out_path} ({stats['encoded_size']:,} bytes, ratio={stats['ratio']:.2f}x)")
    print(f"  Time: {stats['encode_time']*1000:.1f}ms")


def cmd_decode(args):
    """Decode GPXL to original file."""
    with open(args.input, 'rb') as f:
        encoded = f.read()

    print(f"Decoding: {os.path.basename(args.input)} ({len(encoded):,} bytes)")

    t0 = time.perf_counter()
    data, ok = decode(encoded)
    t1 = time.perf_counter()

    out_path = args.output
    if not out_path:
        if args.input.endswith('.gpxl'):
            out_path = args.input[:-5]
        else:
            out_path = args.input + '.decoded'

    with open(out_path, 'wb') as f:
        f.write(data)

    print(f"  Output: {out_path} ({len(data):,} bytes)")
    print(f"  Hash match: {'PASS' if ok else 'FAIL'}")
    print(f"  Time: {(t1-t0)*1000:.1f}ms")


def cmd_info(args):
    """Show GPXL metadata."""
    with open(args.input, 'rb') as f:
        encoded = f.read()

    buf = memoryview(encoded)
    magic = bytes(buf[0:4])
    if magic != GPXL_MAGIC:
        print(f"Not a GPXL file (magic: {magic})")
        return

    version = struct.unpack_from('<H', buf, 4)[0]
    gp_level = buf[6]
    n_segments = struct.unpack_from('<I', buf, 10)[0]
    original_size = struct.unpack_from('<Q', buf, 14)[0]
    stored_xxh64 = struct.unpack_from('<Q', buf, 22)[0]
    coord_lc_sz = struct.unpack_from('<I', buf, 30)[0]

    print(f"GPXL v{version} — {os.path.basename(args.input)}")
    print(f"  Segments: {n_segments}")
    print(f"  Original: {original_size:,} bytes")
    print(f"  Encoded:  {len(encoded):,} bytes")
    print(f"  Ratio:    {len(encoded)/original_size:.2f}x")
    print(f"  GP Level: {gp_level}")
    print(f"  xxh64:    0x{stored_xxh64:016x}")

    # Skeleton stats from coord records
    skel_counts = [0] * 6
    diamond_counts = [0] * 3
    pos = GPXL_HEADER_SZ
    for si in range(n_segments):
        strategy = buf[pos + 8]
        if strategy < 6:
            skel_counts[strategy] += 1
        pos += 20  # coord record size

    print(f"  Skeleton: ID={skel_counts[0]} FLAT={skel_counts[1]} "
          f"DIFF={skel_counts[2]} BREF={skel_counts[3]} "
          f"GEOM={skel_counts[4]} RAW={skel_counts[5]}")


def cmd_verify(args):
    """Verify roundtrip: encode → decode → compare hash."""
    with open(args.input, 'rb') as f:
        original = f.read()

    original_hash = hashlib.sha256(original).hexdigest()
    name = os.path.basename(args.input)

    # Encode
    t0 = time.perf_counter()
    encoded, stats = encode(original)
    t1 = time.perf_counter()

    # Decode
    decoded, xxh_ok = decode(encoded)
    t2 = time.perf_counter()

    decoded_hash = hashlib.sha256(decoded).hexdigest()
    match = (original_hash == decoded_hash)

    print(f"Verify: {name} ({len(original):,} bytes)")
    print(f"  Encode: {(t1-t0)*1000:.1f}ms  Decode: {(t2-t1)*1000:.1f}ms")
    print(f"  SHA256: {'PASS' if match else 'FAIL'}")
    print(f"  xxh64:  {'PASS' if xxh_ok else 'FAIL'}")

    if not match:
        for i in range(min(len(original), len(decoded))):
            if original[i] != decoded[i]:
                print(f"  First diff at byte {i}: orig=0x{original[i]:02x} decoded=0x{decoded[i]:02x}")
                break

    return match


def cmd_benchmark(args):
    """Run pipeline benchmark."""
    from benchmark_pipeline import run_benchmark
    run_benchmark()


def cmd_batch(args):
    """Batch encode all files in a directory."""
    if not os.path.isdir(args.input):
        print(f"Not a directory: {args.input}")
        return

    outdir = args.output or (args.input + '_gpxl')
    os.makedirs(outdir, exist_ok=True)

    files = [f for f in os.listdir(args.input) if os.path.isfile(os.path.join(args.input, f))]
    print(f"Batch encoding {len(files)} files → {outdir}")

    total_in = 0
    total_out = 0
    for fname in files:
        in_path = os.path.join(args.input, fname)
        with open(in_path, 'rb') as f:
            data = f.read()

        encoded, stats = encode(data)
        out_path = os.path.join(outdir, fname + '.gpxl')
        with open(out_path, 'wb') as f:
            f.write(encoded)

        total_in += len(data)
        total_out += len(encoded)
        print(f"  {fname}: {len(data):,}B → {len(encoded):,}B ({stats['ratio']:.2f}x) {stats['encode_time']*1000:.1f}ms")

    print(f"\n  Total: {total_in:,}B → {total_out:,}B ({total_out/total_in:.2f}x)")


# ════════════════════════════════════════════════════════════════
# MAIN
# ════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description='GeoField Pipeline CLI')
    sub = parser.add_subparsers(dest='command')

    # encode
    p_enc = sub.add_parser('encode', help='Encode file to GPXL')
    p_enc.add_argument('input', help='Input file path')
    p_enc.add_argument('-o', '--output', help='Output .gpxl path')
    p_enc.add_argument('--level', type=int, default=2, help='Goldberg level')

    # decode
    p_dec = sub.add_parser('decode', help='Decode GPXL to file')
    p_dec.add_argument('input', help='Input .gpxl path')
    p_dec.add_argument('-o', '--output', help='Output file path')

    # info
    p_info = sub.add_parser('info', help='Show GPXL metadata')
    p_info.add_argument('input', help='Input .gpxl path')

    # verify
    p_verify = sub.add_parser('verify', help='Verify roundtrip')
    p_verify.add_argument('input', help='Input file path')

    # benchmark
    sub.add_parser('benchmark', help='Run benchmark')

    # batch
    p_batch = sub.add_parser('batch', help='Batch encode directory')
    p_batch.add_argument('input', help='Input directory')
    p_batch.add_argument('-o', '--output', help='Output directory')

    args = parser.parse_args()

    if args.command == 'encode':
        cmd_encode(args)
    elif args.command == 'decode':
        cmd_decode(args)
    elif args.command == 'info':
        cmd_info(args)
    elif args.command == 'verify':
        cmd_verify(args)
    elif args.command == 'benchmark':
        cmd_benchmark(args)
    elif args.command == 'batch':
        cmd_batch(args)
    else:
        parser.print_help()


import ctypes

if __name__ == '__main__':
    main()
