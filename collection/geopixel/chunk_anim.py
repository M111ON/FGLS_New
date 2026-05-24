"""
chunk_anim.py — Binary ↔ GPX4 Container (single file, lossless)
══════════════════════════════════════════════════════════════════════
Default: single GPX4 layer, binary → ZSTD → .gpx4
Optional: animation frames via --animate flag

File structure (single mode):
  FILE_HEADER: GPX4 magic + 1 tile
  LAYER_TABLE: 1 layer (type=FALLBACK, name="DATA")
  TILE_TABLE : rows=0, blob_sz
  LAYER DATA : [magic4][orig_size4][xxh64_8][zstd_compressed...]

Usage:
  python chunk_anim.py encode <input.bin> -o out.gpx4
  python chunk_anim.py decode <input.gpx4> -o out.bin
  python chunk_anim.py encode --animate <input.bin> -o out.gpx4
"""

import argparse
import os
import struct
import sys
import zstandard as zstd

# ── constants ────────────────────────────────────────────────────────
CHUNK_SIZE = 64

# GPX4 spec
GPX4_MAGIC       = b'GPX4'
GPX4_VERSION     = 0x02
GPX4_FILE_HDR_SZ = 16
GPX4_LAYER_ENTRY_SZ = 14
GPX4_TILE_ENTRY_SZ  = 8
GPX4_LAYER_FALLBACK = 0x02
GPX4_TILE_PRESENT   = 0x0001

# xxh64
_H1  = 0x9e3779b97f4a7c15
_H2  = 0x6c62272e07bb0142
_M64 = (1 << 64) - 1

def _rotl64(x, r):
    return ((x << r) | (x >> (64 - r))) & _M64

def _hash_update(acc, word):
    acc = (acc ^ ((word * _H1) & _M64)) & _M64
    acc = _rotl64(acc, 27)
    acc = (acc * _H2 + 0x94d049bb133111eb) & _M64
    return acc

def xxh64(data: bytes) -> int:
    acc = (_H1 ^ len(data)) & _M64
    i = 0
    while i + 8 <= len(data):
        w, = struct.unpack_from("<Q", data, i)
        acc = _hash_update(acc, w)
        i += 8
    if i < len(data):
        tail = data[i:] + b"\x00" * (8 - len(data[i:]))
        w, = struct.unpack("<Q", tail)
        acc = _hash_update(acc, w)
    acc = (acc ^ (acc >> 33)) & _M64
    acc = (acc * _H1) & _M64
    acc = (acc ^ (acc >> 29)) & _M64
    acc = (acc * _H2) & _M64
    acc = (acc ^ (acc >> 32)) & _M64
    return acc


# ═══════════════════════════════════════════════════════════════════
# GPX4 Container I/O
# ═══════════════════════════════════════════════════════════════════

def _u16(b, off=0): return (b[off] << 8) | b[off+1]
def _u32(b, off=0): return (b[off] << 24) | (b[off+1] << 16) | (b[off+2] << 8) | b[off+3]
def _w16(v): return bytes([(v>>8)&0xFF, v&0xFF])
def _w32(v): return bytes([(v>>24)&0xFF, (v>>16)&0xFF, (v>>8)&0xFF, v&0xFF])


def write_gpx4_single(path: str, blob: bytes):
    """Write a single-layer GPX4 file.
    blob = raw payload (must include metadata header internally).
    """
    nt = 1
    blob_sz = len(blob)

    with open(path, 'wb') as f:
        fh = bytearray(GPX4_FILE_HDR_SZ)
        fh[0:4] = GPX4_MAGIC
        fh[4] = GPX4_VERSION
        fh[5] = 0
        fh[6:8] = _w16(1)           # n_layers
        fh[8:10] = _w16(1)          # tw
        fh[10:12] = _w16(1)         # th
        fh[12:16] = _w32(1)         # n_tiles
        f.write(fh)

        data_off = GPX4_FILE_HDR_SZ + GPX4_LAYER_ENTRY_SZ + GPX4_TILE_ENTRY_SZ

        lb = bytearray(GPX4_LAYER_ENTRY_SZ)
        lb[0] = GPX4_LAYER_FALLBACK  # type
        lb[1] = 0                     # flags
        lb[2:6] = b'DATA'             # name
        lb[6:10] = _w32(data_off)
        lb[10:14] = _w32(blob_sz)
        f.write(lb)

        tb = bytearray(GPX4_TILE_ENTRY_SZ)
        tb[0:2] = _w16(0)            # rows
        tb[2:4] = _w16(GPX4_TILE_PRESENT)
        tb[4:8] = _w32(blob_sz)
        f.write(tb)

        f.write(blob)


def read_gpx4_single(path: str) -> bytes:
    """Read blob from single-layer GPX4 file. Returns raw payload bytes."""
    with open(path, 'rb') as f:
        raw = f.read()

    if raw[:4] != GPX4_MAGIC:
        raise ValueError(f"bad magic: {raw[:4]}")

    pos = GPX4_FILE_HDR_SZ
    nl = _u16(raw, 6)
    if nl < 1:
        raise ValueError("no layers")

    entry_sz = GPX4_LAYER_ENTRY_SZ if raw[4] >= 0x02 else 12
    last_type = 0
    for i in range(nl):
        lt = raw[pos]
        off = _u32(raw, pos + 6)
        sz = _u32(raw, pos + 10) if raw[4] >= 0x02 else _u16(raw, pos + 10)
        if i == nl - 1:
            # Return last layer's data
            return raw[off:off+sz]
        pos += entry_sz

    raise ValueError("empty GPX4 file")


# ═══════════════════════════════════════════════════════════════════
# Encode / Decode
# ═══════════════════════════════════════════════════════════════════

MAGIC_SINGLE = b'GPXD'  # 4B magic for single-blob payload

def encode_to_gpx4(data: bytes, out_path: str, zstd_level: int = 9) -> dict:
    """
    Encode binary → single-layer GPX4.
    Payload: magic+orig_size+xxh64+zstd_compressed.
    """
    orig_size = len(data)
    digest = xxh64(data)
    cctx = zstd.ZstdCompressor(level=zstd_level)
    compressed = cctx.compress(data)

    payload = bytearray()
    payload.extend(MAGIC_SINGLE)               # 4B
    payload.extend(struct.pack('<I', orig_size))  # 4B
    payload.extend(struct.pack('<Q', digest))     # 8B
    payload.extend(compressed)                    # ZSTD blob

    write_gpx4_single(out_path, bytes(payload))

    meta = {
        'orig_size': orig_size,
        'compressed_size': len(compressed),
        'payload_size': len(payload),
        'xxh64': f"{digest:016X}",
    }
    return meta


def decode_from_gpx4(path: str) -> bytes:
    """
    Decode GPX4 → original binary.
    Auto-detects: single-layer (magic GPXD) or animation frames.
    """
    raw = read_gpx4_single(path)
    return _decode_payload(raw)


def _decode_payload(raw: bytes) -> bytes:
    if raw[:4] == MAGIC_SINGLE:
        orig_size = struct.unpack('<I', raw[4:8])[0]
        stored_digest = struct.unpack('<Q', raw[8:16])[0]
        compressed = raw[16:]

        dctx = zstd.ZstdDecompressor()
        result = dctx.decompress(compressed, max_output_size=orig_size + 1)
        if len(result) != orig_size:
            result = result[:orig_size]

        got = xxh64(result)
        if got != stored_digest:
            raise ValueError(
                f"xxh64 mismatch: got={got:016X} stored={stored_digest:016X}")
        return result

    raise ValueError(f"unknown payload magic: {raw[:4]}")


# ═══════════════════════════════════════════════════════════════════
# Animation frame mode (kept for compatibility, --animate flag)
# ═══════════════════════════════════════════════════════════════════

TILE_PX         = 32
PIXELS          = TILE_PX * TILE_PX
FRAME_BYTES     = PIXELS * 3
FH_BYTES        = 9
HDR_META_BYTES  = 21
HDR_DATA_BYTES  = FRAME_BYTES - FH_BYTES - HDR_META_BYTES
FRM_DATA_BYTES  = FRAME_BYTES - FH_BYTES

GPX4_LAYER_O4       = 0x01
GPX4_LAYER_DELTA    = 0x03
GPX4_LAYER_ANIM_HDR = 0x05
GPX4_LFLAG_KEYFRAME = 0x01
GPX4_LFLAG_ZSTD     = 0x02
GPX4_TILE_SKIP      = 0x0004
GPX4_ANIM_HDR_SZ    = 16


class Gpx4File:
    """Minimal GPX4 reader (for animation mode)."""
    def __init__(self):
        self.layers = []
        self._raw = None

    def read(self, path):
        with open(path, 'rb') as f:
            self._raw = f.read()
        raw = self._raw
        if raw[:4] != GPX4_MAGIC:
            raise ValueError(f"bad magic: {raw[:4]}")
        entry_sz = GPX4_LAYER_ENTRY_SZ if raw[4] >= 0x02 else 12
        nl = _u16(raw, 6)
        pos = GPX4_FILE_HDR_SZ
        for _ in range(nl):
            lt = raw[pos]
            lf = raw[pos+1]
            nm = raw[pos+2:pos+6].decode('ascii', errors='replace')
            of = _u32(raw, pos+6)
            sz = _u32(raw, pos+10) if raw[4] >= 0x02 else _u16(raw, pos+10)
            self.layers.append({
                'type': lt, 'flags': lf, 'name': nm,
                'offset': of, 'size': sz,
                'data': raw[of:of+sz],
            })
            pos += entry_sz

    def close(self):
        self._raw = None
        self.layers = []


def _write_gpx4_anim(path, frames, W, H, fps_num, fps_den, kfi, zstd_level):
    """Write animation GPX4. frames = list of (rgb_bytes, is_key, seq)."""
    gpx = Gpx4File.__new__(Gpx4File)
    gpx.layers = []
    cctx = zstd.ZstdCompressor(level=zstd_level)

    n_frames = len(frames)
    nt = 1

    ahdr = bytearray(GPX4_ANIM_HDR_SZ)
    struct.pack_into('>HHHHHHHH', ahdr, 0, n_frames, fps_num, fps_den, kfi, W, H, 0, 0)
    gpx.layers.append({'type': GPX4_LAYER_ANIM_HDR, 'flags': 0, 'name': 'AHDR',
                        'data': bytes(ahdr), 'offset': 0, 'size': GPX4_ANIM_HDR_SZ})

    for fi, (rgb, is_key, seq) in enumerate(frames):
        blob = cctx.compress(rgb)
        lt = GPX4_LAYER_O4 if is_key else GPX4_LAYER_DELTA
        lf = GPX4_LFLAG_KEYFRAME if is_key else GPX4_LFLAG_ZSTD
        nm = f"F{fi:03d}" if is_key else f"D{fi:03d}"
        gpx.layers.append({'type': lt, 'flags': lf, 'name': nm,
                            'data': blob, 'offset': 0, 'size': len(blob)})

    nl = len(gpx.layers)
    n_tiled = nl - 1  # AHDR is not tiled
    data_start = (GPX4_FILE_HDR_SZ + nl * GPX4_LAYER_ENTRY_SZ
                  + n_tiled * nt * GPX4_TILE_ENTRY_SZ)

    with open(path, 'wb') as f:
        fh = bytearray(GPX4_FILE_HDR_SZ)
        fh[0:4] = GPX4_MAGIC
        fh[4] = GPX4_VERSION
        fh[5] = 0
        fh[6:8] = _w16(nl)
        fh[8:10] = _w16(1)
        fh[10:12] = _w16(1)
        fh[12:16] = _w32(nt)
        f.write(fh)

        off = data_start
        for l in gpx.layers:
            lb = bytearray(GPX4_LAYER_ENTRY_SZ)
            lb[0] = l['type']
            lb[1] = l['flags']
            lb[2:6] = l['name'].encode()[:4]
            lb[6:10] = _w32(off)
            lb[10:14] = _w32(l['size'])
            f.write(lb)
            off += l['size']

        for l in gpx.layers:
            if l['name'] == 'AHDR':
                continue
            tb = bytearray(GPX4_TILE_ENTRY_SZ)
            tb[0:2] = _w16(1)
            tb[2:4] = _w16(GPX4_TILE_PRESENT)
            tb[4:8] = _w32(l['size'])
            f.write(tb)

        for l in gpx.layers:
            f.write(l['data'])

    gpx.close()


def _encode_animate(data, out_path, kfi, zstd_level, fps_num, fps_den):
    """Animation mode: split data across animation frames."""
    orig_size = len(data)
    digest = xxh64(data)
    total_chunks = (orig_size + CHUNK_SIZE - 1) // CHUNK_SIZE
    chunk_bytes = b''.join(
        data[i:i+CHUNK_SIZE].ljust(CHUNK_SIZE, b'\x00')
        for i in range(0, orig_size, CHUNK_SIZE))

    pos, frames = 0, []
    for fi in range(999):
        cap = HDR_DATA_BYTES if fi == 0 else FRM_DATA_BYTES
        if pos >= len(chunk_bytes):
            break
        fd = chunk_bytes[pos:pos+cap]
        pos += cap
        is_key = (fi == 0) or (kfi > 0 and fi % kfi == 0)
        rgb = bytearray(FRAME_BYTES)

        if fi == 0:
            rgb[0] = ord('H')
            o = FH_BYTES
            for bv in struct.pack('>I', orig_size):
                rgb[o] = bv; o += 1
            for bv in struct.pack('>I', total_chunks):
                rgb[o] = bv; o += 1
            for bi in range(8):
                rgb[o + bi] = (digest >> (bi * 8)) & 0xFF
            o += 8
            nf = (len(chunk_bytes) + HDR_DATA_BYTES - 1) // HDR_DATA_BYTES
            if cap == FRM_DATA_BYTES:
                nf = 1 + (len(chunk_bytes) - HDR_DATA_BYTES + FRM_DATA_BYTES - 1) // FRM_DATA_BYTES
            rgb[o] = (nf >> 8) & 0xFF
            rgb[o+1] = nf & 0xFF
            rgb[FH_BYTES + HDR_META_BYTES:FH_BYTES + HDR_META_BYTES + len(fd)] = fd
        else:
            rgb[0] = ord('D')
            rgb[1] = (fi >> 8) & 0xFF
            rgb[2] = fi & 0xFF
            rgb[9:9+len(fd)] = fd

        frames.append((bytes(rgb), is_key, fi))

    _write_gpx4_anim(out_path, frames, TILE_PX, TILE_PX, fps_num, fps_den, kfi, zstd_level)

    return {
        'orig_size': orig_size,
        'n_frames': len(frames),
        'keyframe_interval': kfi,
        'xxh64': f"{digest:016X}",
    }


def _decode_animate(path):
    """Decode animation GPX4 → binary."""
    gpx = Gpx4File()
    gpx.read(path)

    ahdr_layer = next((l for l in gpx.layers if l['type'] == GPX4_LAYER_ANIM_HDR), None)
    if not ahdr_layer:
        raise ValueError("no AHDR layer")

    ah = ahdr_layer['data']
    n_frames, _, _, kfi, W, H, _, _ = struct.unpack_from('>HHHHHHHH', ah, 0)
    dctx = zstd.ZstdDecompressor()
    all_bytes = bytearray()

    for fi in range(n_frames):
        is_key = (fi == 0) or (kfi > 0 and fi % kfi == 0)
        name = f"F{fi:03d}" if is_key else f"D{fi:03d}"
        layer = next((l for l in gpx.layers if l['name'] == name), None)
        if not layer:
            continue

        blob = layer['data']
        try:
            rgb = dctx.decompress(blob, max_output_size=FRAME_BYTES)
        except:
            rgb = blob
        rgb = rgb[:FRAME_BYTES].ljust(FRAME_BYTES, b'\x00')

        if fi == 0:
            o = FH_BYTES
            orig_size = (rgb[o] << 24) | (rgb[o+1] << 16) | (rgb[o+2] << 8) | rgb[o+3]
            sd = 0
            for bi in range(8):
                sd |= rgb[o+8+4+bi] << (bi * 8)
            all_bytes.extend(rgb[FH_BYTES+HDR_META_BYTES:FH_BYTES+HDR_META_BYTES+HDR_DATA_BYTES])
        else:
            all_bytes.extend(rgb[9:9+FRM_DATA_BYTES])

    result = bytes(all_bytes[:orig_size])
    got = xxh64(result)
    if got != sd:
        raise ValueError(f"xxh64 mismatch: got={got:016X} stored={sd:016X}")
    gpx.close()
    return result


# ═══════════════════════════════════════════════════════════════════
# CLI
# ═══════════════════════════════════════════════════════════════════

def main():
    ap = argparse.ArgumentParser(description="Binary ↔ GPX4 Container")
    ap.add_argument("cmd", choices=['encode', 'decode'])
    ap.add_argument("input")
    ap.add_argument("-o", "--out", default="", help="output path")
    ap.add_argument("--zstd", type=int, default=9, help="ZSTD level 1-19")
    ap.add_argument("--animate", action="store_true",
                    help="use animation frames (default: single layer)")
    ap.add_argument("--kfi", type=int, default=4, help="keyframe interval")
    args = ap.parse_args()

    if args.cmd == 'encode':
        if not os.path.isfile(args.input):
            print(f"error: not found: {args.input}", file=sys.stderr)
            sys.exit(1)
        data = open(args.input, 'rb').read()
        if not data:
            print("error: empty file", file=sys.stderr)
            sys.exit(1)

        out = args.out or (args.input + '.gpx4')

        if args.animate:
            meta = _encode_animate(data, out, args.kfi, args.zstd, 24, 1)
            print(f"ok  {args.input} → {out}")
            print(f"    orig={meta['orig_size']}B  frames={meta['n_frames']}  "
                  f"kfi={meta['keyframe_interval']}  xxh64={meta['xxh64']}")
        else:
            meta = encode_to_gpx4(data, out, zstd_level=args.zstd)
            sz = os.path.getsize(out)
            print(f"ok  {args.input} → {out}")
            print(f"    orig={meta['orig_size']}B  gpx4={sz}B  "
                  f"ratio={sz/meta['orig_size']:.2f}x  "
                  f"xxh64={meta['xxh64']}")

    elif args.cmd == 'decode':
        if not os.path.isfile(args.input):
            print(f"error: not found: {args.input}", file=sys.stderr)
            sys.exit(1)

        try:
            result = decode_from_gpx4(args.input)
        except Exception as e:
            # try animation decode
            try:
                result = _decode_animate(args.input)
            except Exception as e2:
                print(f"error (single): {e}", file=sys.stderr)
                print(f"error (anim): {e2}", file=sys.stderr)
                sys.exit(2)

        got = xxh64(result)
        print(f"ok    xxh64 verified: {got:016X}")

        out = args.out or (args.input.replace('.gpx4', '') + '.bin')
        if out == args.input:
            out += '.bin'
        open(out, 'wb').write(result)
        print(f"ok  {args.input} → {out}  ({len(result)}B)")


if __name__ == "__main__":
    main()
