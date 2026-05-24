"""
svg2chunk.py — SVG vector image → binary file (lossless reconstruct)
══════════════════════════════════════════════════════════════════════
Reads SVG produced by chunk2svg.py.
Parses data-* attributes from <svg> root for metadata.
Reads fill colors from <rect> elements to recover bits/bytes.
Reassembles original binary, trims to orig_size, verifies xxh64.

Usage:
  python svg2chunk.py <input.svg> [--out out.bin] [--no-verify]
"""

import argparse
import sys
import os
import re
import struct

CHUNK_SIZE = 64
GRID_W     = 8
GRID_H     = 8
MODE_BW    = 0
MODE_GRAY  = 1

CELL_BW    = 3
CELL_GRAY  = 8

C_ON       = "#f0f0f0"
C_OFF      = "#0a0a0a"

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


def _attr(tag: str, name: str) -> str:
    m = re.search(rf'{name}="([^"]*)"', tag)
    return m.group(1) if m else ""

def decode_svg(svg_text: str) -> tuple[bytes, dict]:
    svg_tag_m = re.search(r"<svg[^>]+>", svg_text, re.DOTALL)
    if not svg_tag_m:
        raise ValueError("no <svg> root tag found")
    svg_tag = svg_tag_m.group(0)

    mode        = int(_attr(svg_tag, "data-mode") or "0")
    orig_size   = int(_attr(svg_tag, "data-orig-size") or "0")
    chunk_count = int(_attr(svg_tag, "data-chunk-count") or "0")
    xxh64_str   = _attr(svg_tag, "data-xxh64")
    expected_h  = int(xxh64_str, 16) if xxh64_str else None

    meta = {
        "mode":        mode,
        "orig_size":   orig_size,
        "chunk_count": chunk_count,
        "xxh64":       xxh64_str,
    }

    rects = re.findall(r"<rect\b[^/]*/?>", svg_text)

    data_rects = []
    if mode == MODE_BW:
        for r in rects:
            fill = _attr(r, "fill")
            if fill in (C_ON, C_OFF):
                data_rects.append(r)
    else:
        for r in rects:
            dv = _attr(r, "data-v")
            if dv != "":
                data_rects.append(r)

    tile_w   = int(_attr(svg_tag, "data-tile-w")   or "0")
    tile_h   = int(_attr(svg_tag, "data-tile-h")   or "0")
    tile_gap = int(_attr(svg_tag, "data-tile-gap") or "2")
    pad      = int(_attr(svg_tag, "data-pad")      or "8")
    header_h = int(_attr(svg_tag, "data-header-h") or "24")
    n_cols   = int(_attr(svg_tag, "data-cols")     or "8")

    stride_x = tile_w + tile_gap
    stride_y = tile_h + tile_gap
    origin_y = pad + header_h + tile_gap

    if tile_w == 0 or tile_h == 0:
        raise ValueError("missing data-tile-w/h — SVG not from chunk2svg.py")

    tile_cells: dict[tuple, list] = {}
    for r in data_rects:
        x = float(_attr(r, "x") or "0")
        y = float(_attr(r, "y") or "0")
        tc = int((x - pad) // stride_x)
        tr = int((y - origin_y) // stride_y)
        key = (tr, tc)
        lx = x - (pad + tc * stride_x)
        ly = y - (origin_y + tr * stride_y)
        tile_cells.setdefault(key, []).append((ly, lx, r))

    sorted_keys = sorted(tile_cells.keys())

    raw_bytes = bytearray()
    for key in sorted_keys:
        cells = sorted(tile_cells[key])
        if mode == MODE_BW:
            bits = []
            for (_, _, r) in cells:
                bits.append(1 if _attr(r, "fill") == C_ON else 0)
            for i in range(0, len(bits), 8):
                byte_bits = bits[i:i+8]
                if len(byte_bits) < 8:
                    break
                val = 0
                for b in byte_bits:
                    val = (val << 1) | b
                raw_bytes.append(val)
        else:
            for (_, _, r) in cells:
                raw_bytes.append(int(_attr(r, "data-v") or "0") & 0xFF)

    result = bytes(raw_bytes[:orig_size])

    return result, meta


def main():
    ap = argparse.ArgumentParser(description="SVG vector image → binary (reconstruct)")
    ap.add_argument("input",           help="input SVG file")
    ap.add_argument("--out",   default="",    help="output binary (default: <input>.bin)")
    ap.add_argument("--no-verify", action="store_true", help="skip xxh64 checksum")
    args = ap.parse_args()

    if not os.path.isfile(args.input):
        print(f"error: file not found: {args.input}", file=sys.stderr)
        sys.exit(1)

    svg_text = open(args.input, "r").read()

    try:
        result, meta = decode_svg(svg_text)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)

    if not args.no_verify:
        got = xxh64(result)
        expected = int(meta["xxh64"], 16) if meta["xxh64"] else None
        if expected is not None and got != expected:
            print(f"FAIL  xxh64 mismatch: got={got:016X} expected={expected:016X}", file=sys.stderr)
            sys.exit(2)
        else:
            print(f"ok    xxh64 verified: {got:016X}")

    out = args.out or (args.input.replace(".svg", "") + ".bin")
    if out == args.input:
        out += ".bin"
    open(out, "wb").write(result)

    print(f"ok    {args.input} → {out}")
    print(f"      orig_size={meta['orig_size']}B  chunks={meta['chunk_count']}  mode={meta['mode']}")


if __name__ == "__main__":
    main()
