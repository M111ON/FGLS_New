#!/usr/bin/env python3
import sys, os, time, hashlib, struct, argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geofield_ctypes as gf

CHUNK_SZ = 64
GPXL_MAGIC = b'GPXL'
GPXL_VERSION = 4
GPXL_HEADER_SZ = 48
GEO_JUMP_HILBERT = 0
GEO_FULL = 20736
GEO_PENTAGONS = 12
GEO_FACE_SLOTS = 120


def auto_gp_level(data_size):
    n_chunks = (data_size + 63) // 64
    for level in range(1, 16):
        if 10 * level * level + 2 >= n_chunks:
            return level
    return 15


def geo_jump_route(start_node, chunk_idx):
    dest = gf.geo_jump(start_node, GEO_JUMP_HILBERT, chunk_idx + 1)
    enc = dest % 1440
    face = enc // GEO_FACE_SLOTS
    slot = enc % GEO_FACE_SLOTS
    return enc, face, slot


def classify_64b(chunk):
    result = gf.ds_classify_block(chunk)
    if isinstance(result, tuple):
        data, sz = result
    else:
        data, sz = result, len(result)
    if data is None or sz >= 64:
        return b'\xfe' + bytes(chunk[:64])
    return bytes(data[:sz])


def decode_64b_to_64(data_bytes):
    if data_bytes[0] == 0xfe:
        return data_bytes[1:65], 65
    dec, n_read = gf.ds_decode_block(data_bytes)
    return dec[:64], n_read


def flow_chunk_simple(data, min_size=256, max_size=4096):
    sz = max(min_size, min(max_size, len(data)))
    chunks = []
    pos = 0
    while pos < len(data):
        n = min(sz, len(data) - pos)
        chunks.append((pos, n))
        pos += n
    return chunks


def encode(data, gp_level=None, geometric=False):
    original_size = len(data)
    xxh64_val = gf.xxh64(data)
    t0 = time.perf_counter()

    if gp_level is None:
        gp_level = auto_gp_level(original_size)

    n_chunks = (original_size + 63) // 64
    flat_count = 0
    nonflat_count = 0
    raw_count = 0
    skel_id = 0
    skel_flat = 0
    skel_diff = 0
    skel_bref = 0
    skel_geom = 0
    skel_raw = 0
    n_segments = 0

    buf = bytearray()
    buf += GPXL_MAGIC
    buf += struct.pack('<H', GPXL_VERSION)
    buf += struct.pack('<B', gp_level)
    buf += struct.pack('<B', 0)
    buf += struct.pack('<?', geometric)
    buf += b'\x00' * 3
    buf += struct.pack('<I', 0)
    buf += struct.pack('<Q', original_size)
    buf += struct.pack('<Q', xxh64_val)
    buf += b'\x00' * 16

    if geometric:
        buf[7:8] = b'\x01'
        face_count = gf.gp_face_count(gp_level)
        blocks_per_layer = (face_count + 54 - 1) // 54
        n_blocks = ((n_chunks + face_count - 1) // face_count) * blocks_per_layer if n_chunks > 0 else 0
        n_segments = n_blocks
        blocks = [[None] * 54 for _ in range(n_blocks)]
        for ci in range(n_chunks):
            off = ci * 64
            chunk = data[off:off + 64]
            if len(chunk) < 64:
                chunk = chunk + b'\x00' * (64 - len(chunk))
            addr = gf.chunk_to_addr(gp_level, ci)
            tile_id = addr.tile_id
            dim = addr.dim
            tile_group = tile_id // 54
            diamond_slot = tile_id % 54
            block_idx = dim * blocks_per_layer + tile_group
            if block_idx < n_blocks:
                blocks[block_idx][diamond_slot] = chunk
        while n_segments > 0 and all(s is None for s in blocks[n_segments - 1]):
            n_segments -= 1
        blocks = blocks[:n_segments] if n_segments > 0 else []
        struct.pack_into('<I', buf, 12, n_segments)
        for bi in range(n_segments):
            slot_table = bytearray(216)
            compressed_all = bytearray()
            data_offset = 0
            for slot_idx in range(54):
                raw_64b = blocks[bi][slot_idx]
                if raw_64b is None:
                    continue
                classified = classify_64b(raw_64b)
                sz = len(classified)
                dc = gf.diamond_classify(raw_64b)
                flag = dc.flag
                if sz < 65:
                    if flag == 0:
                        flat_count += 1; skel_id += 1
                    elif flag == 1:
                        nonflat_count += 1; skel_diff += 1
                    else:
                        nonflat_count += 1; skel_geom += 1
                else:
                    raw_count += 1; skel_raw += 1
                struct.pack_into('<HH', slot_table, slot_idx * 4, data_offset, sz)
                compressed_all += classified
                data_offset += sz
            buf += slot_table
            buf += compressed_all
    else:
        segments = flow_chunk_simple(data)
        n_segments = len(segments)
        struct.pack_into('<I', buf, 12, n_segments)
        for si, (seg_off, seg_len) in enumerate(segments):
            face_enc = gf.frame_enc(si)
            start_node = (face_enc // GEO_FACE_SLOTS) * GEO_FACE_SLOTS + (face_enc % GEO_FACE_SLOTS)
            fibo_tick = face_enc % 144
            buf += struct.pack('<B', fibo_tick)
            buf += struct.pack('<H', face_enc)
            buf += struct.pack('<I', seg_len)
            n_blk = (seg_len + 63) // 64
            buf += struct.pack('<B', n_blk)
            for bi in range(n_blk):
                off = seg_off + bi * 64
                chunk = data[off:off + 64]
                if len(chunk) < 64:
                    chunk = chunk + b'\x00' * (64 - len(chunk))
                enc, face, slot = geo_jump_route(start_node, bi)
                classified = classify_64b(chunk)
                sz = len(classified)
                dc = gf.diamond_classify(chunk)
                flag = dc.flag
                if sz < 65:
                    if flag == 0:
                        flat_count += 1; skel_id += 1
                    elif flag == 1:
                        nonflat_count += 1; skel_diff += 1
                    else:
                        nonflat_count += 1; skel_geom += 1
                else:
                    raw_count += 1; skel_raw += 1
                buf += classified

    t_encode = time.perf_counter() - t0
    stats = {
        'n_chunks': n_chunks, 'n_segments': n_segments,
        'flat_count': flat_count, 'nonflat_count': nonflat_count, 'raw_count': raw_count,
        'skel_id': skel_id, 'skel_flat': skel_flat, 'skel_diff': skel_diff,
        'skel_bref': skel_bref, 'skel_geom': skel_geom, 'skel_raw': skel_raw,
        'encode_time': t_encode, 'encoded_size': len(buf),
        'ratio': len(buf) / original_size if original_size > 0 else 0,
        'gp_level': gp_level, 'original_size': original_size, 'xxh64': xxh64_val, 'geometric': geometric,
    }
    return bytes(buf), stats


def decode(encoded):
    buf = memoryview(encoded)
    magic = bytes(buf[0:4])
    if magic != GPXL_MAGIC:
        raise ValueError(f"Bad magic: {magic}")
    version = struct.unpack_from('<H', buf, 4)[0]
    gp_level = buf[6]
    geometric = bool(buf[8])
    n_items = struct.unpack_from('<I', buf, 12)[0]
    original_size = struct.unpack_from('<Q', buf, 16)[0]
    stored_xxh64 = struct.unpack_from('<Q', buf, 24)[0]

    result = bytearray(original_size)
    pos = GPXL_HEADER_SZ

    if geometric:
        face_count = gf.gp_face_count(gp_level)
        blocks_per_layer = (face_count + 54 - 1) // 54
        for block_idx in range(n_items):
            slot_table_start = pos
            compressed_start = slot_table_start + 216
            max_comp_end = 0
            slot_entries = []
            for slot_idx in range(54):
                entry_off = slot_table_start + slot_idx * 4
                off = struct.unpack_from('<H', buf, entry_off)[0]
                sz = struct.unpack_from('<H', buf, entry_off + 2)[0]
                slot_entries.append((off, sz))
                if sz > 0 and off + sz > max_comp_end:
                    max_comp_end = off + sz
            dim = block_idx // blocks_per_layer
            tile_group = block_idx % blocks_per_layer
            for slot_idx in range(54):
                off, sz = slot_entries[slot_idx]
                if sz == 0:
                    continue
                raw_64b, _ = decode_64b_to_64(bytes(buf[compressed_start + off:compressed_start + off + sz]))
                tile_id = tile_group * 54 + slot_idx
                if tile_id >= face_count:
                    continue
                chunk_idx = dim * face_count + tile_id
                byte_off = chunk_idx * 64
                if byte_off < original_size:
                    end = min(byte_off + 64, original_size)
                    result[byte_off:end] = raw_64b[:end - byte_off]
            pos = compressed_start + max_comp_end
    else:
        out_off = 0
        for si in range(n_items):
            fibo_tick = buf[pos]
            enc_val = struct.unpack_from('<H', buf, pos + 1)[0]
            seg_len = struct.unpack_from('<I', buf, pos + 3)[0]
            n_blk = buf[pos + 7]
            pos += 8
            for bi in range(n_blk):
                raw_64b, n_read = decode_64b_to_64(bytes(buf[pos:pos + 67]))
                if bi == n_blk - 1 and seg_len % 64 != 0:
                    remaining = seg_len - bi * 64
                    result[out_off + bi * 64:out_off + bi * 64 + remaining] = raw_64b[:remaining]
                else:
                    result[out_off + bi * 64:out_off + bi * 64 + 64] = raw_64b[:64]
                pos += n_read
            out_off += seg_len

    return bytes(result), gf.xxh64(bytes(result)) == stored_xxh64


def cmd_encode(args):
    with open(args.input, 'rb') as f:
        data = f.read()
    name = os.path.basename(args.input)
    print(f"Encoding: {name} ({len(data):,} bytes)")
    print(f"  GP level: {args.gp_level if args.gp_level is not None else 'auto'}")
    if args.geometric:
        print(f"  Mode: GEOMETRIC (FrustumBlock scatter)")
    encoded, stats = encode(data, gp_level=args.gp_level, geometric=args.geometric)
    out_path = args.output or (args.input + '.gpxl')
    with open(out_path, 'wb') as f:
        f.write(encoded)
    print(f"  Chunks: {stats['n_chunks']}  Segments: {stats['n_segments']}")
    print(f"  Diamond: FLAT={stats['flat_count']} SPARSE/DENSE={stats['nonflat_count']} RAW={stats['raw_count']}")
    print(f"  Output: {out_path} ({stats['encoded_size']:,} bytes, ratio={stats['ratio']:.2f}x)")
    print(f"  Time: {stats['encode_time']*1000:.1f}ms")


def cmd_decode(args):
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
    with open(args.input, 'rb') as f:
        encoded = f.read()
    buf = memoryview(encoded)
    magic = bytes(buf[0:4])
    if magic != GPXL_MAGIC:
        print(f"Not a GPXL file (magic: {magic})")
        return
    version = struct.unpack_from('<H', buf, 4)[0]
    gp_level = buf[6]
    geometric = bool(buf[8])
    n_items = struct.unpack_from('<I', buf, 12)[0]
    original_size = struct.unpack_from('<Q', buf, 16)[0]
    stored_xxh64 = struct.unpack_from('<Q', buf, 24)[0]
    ratio = len(encoded) / original_size if original_size > 0 else 0.0
    print(f"GPXL v{version} — {os.path.basename(args.input)}")
    print(f"  Mode: {'Geometric' if geometric else 'Sequential'}")
    print(f"  GP Level: {gp_level}")
    print(f"  {'FrustumBlocks' if geometric else 'Segments'}: {n_items}")
    print(f"  Original: {original_size:,} bytes")
    print(f"  Encoded:  {len(encoded):,} bytes")
    print(f"  Ratio:    {ratio:.2f}x")
    print(f"  xxh64:    0x{stored_xxh64:016x}")


def cmd_verify(args):
    with open(args.input, 'rb') as f:
        original = f.read()
    original_hash = hashlib.sha256(original).hexdigest()
    name = os.path.basename(args.input)
    t0 = time.perf_counter()
    encoded, stats = encode(original, gp_level=args.gp_level, geometric=args.geometric)
    t1 = time.perf_counter()
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


def cmd_benchmark(args):
    from benchmark_pipeline import run_benchmark
    run_benchmark()


def cmd_batch(args):
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
        encoded, stats = encode(data, gp_level=args.gp_level)
        out_path = os.path.join(outdir, fname + '.gpxl')
        with open(out_path, 'wb') as f:
            f.write(encoded)
        total_in += len(data)
        total_out += len(encoded)
        print(f"  {fname}: {len(data):,}B -> {len(encoded):,}B ({stats['ratio']:.2f}x) {stats['encode_time']*1000:.1f}ms")
    print(f"\n  Total: {total_in:,}B -> {total_out:,}B ({total_out/total_in:.2f}x)")


def main():
    parser = argparse.ArgumentParser(description='GeoField Pipeline CLI')
    sub = parser.add_subparsers(dest='command')
    p_enc = sub.add_parser('encode', help='Encode file to GPXL')
    p_enc.add_argument('input')
    p_enc.add_argument('-o', '--output')
    p_enc.add_argument('--gp-level', type=int, default=None)
    p_enc.add_argument('--geometric', action='store_true')
    p_dec = sub.add_parser('decode', help='Decode GPXL to file')
    p_dec.add_argument('input')
    p_dec.add_argument('-o', '--output')
    p_info = sub.add_parser('info', help='Show GPXL metadata')
    p_info.add_argument('input')
    p_verify = sub.add_parser('verify', help='Verify roundtrip')
    p_verify.add_argument('input')
    p_verify.add_argument('--gp-level', type=int, default=None)
    p_verify.add_argument('--geometric', action='store_true')
    sub.add_parser('benchmark', help='Run benchmark')
    p_batch = sub.add_parser('batch', help='Batch encode directory')
    p_batch.add_argument('input')
    p_batch.add_argument('-o', '--output')
    p_batch.add_argument('--gp-level', type=int, default=None)
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


if __name__ == '__main__':
    main()
