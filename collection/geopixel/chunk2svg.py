"""
chunk2svg.py — binary file → SVG vector image (lossless)
═══════════════════════════════════════════════════════════
Encoding:
  - Split file into 64B chunks (DiamondBlock, frozen)
  - Each chunk = 8×8 grid of cells
  - B/W mode:  1 bit  = 1 cell  → 512 bits = 64B per chunk
  - Gray mode: 1 byte = 1 cell  → 64 cells per chunk (8×8)

Layout (single SVG):
  - Header row: metadata bar (file_size, chunk_count, mode, xxh64)
  - Chunk tiles: left→right, top→bottom
  - Each tile separated by 1px gap
  - Tile label: chunk index (tiny text, top-left corner)

Reconstruct contract:
  - SVG viewBox and tile positions are deterministic from header
  - Parser reads fill color per cell → bit/byte → reassemble
  - No lossy ops — SVG is exact, no antialiasing on fills

Usage:
  python chunk2svg.py <input_file> [--mode bw|gray] [--cols N] [--out out.svg]

Reconstruct:
  python svg2chunk.py <input.svg> [--out out.bin]
"""

import argparse
import struct
import sys
import os

# ── frozen constants ──────────────────────────────────────────────────
CHUNK_SIZE   = 64        # DiamondBlock — never change
GRID_W       = 8         # cells per row (bits or bytes)
GRID_H       = 8         # cells per col
MODE_BW      = 0         # 1 bit  per cell, 512 cells per chunk
MODE_GRAY    = 1         # 1 byte per cell,  64 cells per chunk

# SVG layout
CELL_BW      = 3         # px per cell in B/W mode (512 cells → 24px wide)
CELL_GRAY    = 8         # px per cell in gray mode ( 64 cells →  64px wide)
TILE_GAP     = 2         # px gap between tiles
HEADER_H     = 24        # px for metadata bar
PAD          = 8         # outer padding

# colors (B/W)
C_OFF        = "#0a0a0a"
C_ON         = "#f0f0f0"
C_BG         = "#111111"
C_HDR        = "#1a1a2e"
C_HDR_TEXT   = "#8888cc"
C_BORDER     = "#2a2a3a"

# ── hash (mirrors wallet_hash_buf — xxh64-style) ──────────────────────
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

# ── chunk helpers ──────────────────────────────────────────────────────
def split_chunks(data: bytes) -> list[bytes]:
    chunks = []
    for i in range(0, len(data), CHUNK_SIZE):
        raw = data[i : i + CHUNK_SIZE]
        if len(raw) < CHUNK_SIZE:
            raw = raw.ljust(CHUNK_SIZE, b"\x00")
        chunks.append(raw)
    return chunks

def chunk_to_bits(chunk: bytes) -> list[int]:
    bits = []
    for b in chunk:
        for i in range(7, -1, -1):
            bits.append((b >> i) & 1)
    return bits

def chunk_to_bytes(chunk: bytes) -> list[int]:
    return list(chunk)

# ── SVG tile builders ─────────────────────────────────────────────────
def _tile_bw(chunk_idx: int, chunk: bytes, ox: int, oy: int) -> list[str]:
    bits   = chunk_to_bits(chunk)
    cell   = CELL_BW
    cols   = GRID_W * GRID_H
    rows   = 8
    lines  = []
    for r in range(rows):
        for c in range(cols):
            bit = bits[r * cols + c]
            x   = ox + c * cell
            y   = oy + r * cell
            col = C_ON if bit else C_OFF
            lines.append(
                f'<rect x="{x}" y="{y}" width="{cell}" height="{cell}" '
                f'fill="{col}"/>'
            )
    lines.append(
        f'<text x="{ox+1}" y="{oy+rows*cell-1}" '
        f'font-size="3" fill="{C_HDR_TEXT}" opacity="0.7">{chunk_idx}</text>'
    )
    return lines

def _tile_gray(chunk_idx: int, chunk: bytes, ox: int, oy: int) -> list[str]:
    vals = chunk_to_bytes(chunk)
    cell = CELL_GRAY
    lines = []
    for r in range(GRID_H):
        for c in range(GRID_W):
            v   = vals[r * GRID_W + c]
            col = f"#{v:02x}{v:02x}{v:02x}"
            x   = ox + c * cell
            y   = oy + r * cell
            lines.append(
                f'<rect x="{x}" y="{y}" width="{cell}" height="{cell}" '
                f'fill="{col}" data-v="{v}"/>'
            )
    lines.append(
        f'<text x="{ox+1}" y="{oy+1+6}" '
        f'font-size="5" fill="{C_HDR_TEXT}" opacity="0.6">{chunk_idx}</text>'
    )
    return lines

# ── SVG generator ─────────────────────────────────────────────────────
def encode_svg(data: bytes, mode: int = MODE_BW, cols: int = 8) -> str:
    chunks      = split_chunks(data)
    n           = len(chunks)
    digest      = xxh64(data)
    orig_size   = len(data)

    cell        = CELL_BW   if mode == MODE_BW   else CELL_GRAY
    tile_w      = (GRID_W * GRID_H * cell) if mode == MODE_BW else (GRID_W * cell)
    tile_h      = (GRID_H           * cell) if mode == MODE_BW else (GRID_H * cell)

    rows_needed = (n + cols - 1) // cols

    total_w = PAD * 2 + cols * tile_w + (cols - 1) * TILE_GAP
    total_h = PAD * 2 + HEADER_H + TILE_GAP + rows_needed * tile_h + (rows_needed - 1) * TILE_GAP

    svg = []
    svg.append(
        f'<svg xmlns="http://www.w3.org/2000/svg" '
        f'width="{total_w}" height="{total_h}" '
        f'viewBox="0 0 {total_w} {total_h}" '
        f'data-mode="{mode}" data-orig-size="{orig_size}" '
        f'data-chunk-count="{n}" data-xxh64="{digest:016X}" '
        f'data-chunk-size="{CHUNK_SIZE}" '
        f'data-tile-w="{tile_w}" data-tile-h="{tile_h}" '
        f'data-tile-gap="{TILE_GAP}" data-pad="{PAD}" '
        f'data-header-h="{HEADER_H}" data-cols="{cols}">'
    )

    svg.append(f'<rect width="{total_w}" height="{total_h}" fill="{C_BG}"/>')

    svg.append(
        f'<rect x="{PAD}" y="{PAD}" width="{total_w - PAD*2}" height="{HEADER_H}" '
        f'fill="{C_HDR}" rx="3"/>'
    )
    hdr_text = (
        f"POGLS  mode={'BW' if mode==MODE_BW else 'GRAY'}  "
        f"orig={orig_size}B  chunks={n}  xxh64={digest:016X}"
    )
    svg.append(
        f'<text x="{PAD+6}" y="{PAD+16}" font-family="monospace" '
        f'font-size="9" fill="{C_HDR_TEXT}">{hdr_text}</text>'
    )

    for idx, chunk in enumerate(chunks):
        row = idx // cols
        col = idx  % cols
        ox  = PAD + col * (tile_w + TILE_GAP)
        oy  = PAD + HEADER_H + TILE_GAP + row * (tile_h + TILE_GAP)

        if mode == MODE_BW:
            svg.extend(_tile_bw(idx, chunk, ox, oy))
        else:
            svg.extend(_tile_gray(idx, chunk, ox, oy))

    svg.append("</svg>")
    return "\n".join(svg)

# ── CLI ───────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description="binary → SVG vector image (lossless)")
    ap.add_argument("input",          help="input binary file")
    ap.add_argument("--mode",  default="bw",  choices=["bw", "gray"])
    ap.add_argument("--cols",  type=int, default=8, help="tiles per row")
    ap.add_argument("--out",   default="",    help="output SVG (default: <input>.svg)")
    args = ap.parse_args()

    if not os.path.isfile(args.input):
        print(f"error: file not found: {args.input}", file=sys.stderr)
        sys.exit(1)

    data = open(args.input, "rb").read()
    if not data:
        print("error: empty file", file=sys.stderr)
        sys.exit(1)

    mode = MODE_BW if args.mode == "bw" else MODE_GRAY
    svg  = encode_svg(data, mode=mode, cols=args.cols)

    out  = args.out or (args.input + ".svg")
    open(out, "w").write(svg)

    chunks = (len(data) + CHUNK_SIZE - 1) // CHUNK_SIZE
    print(f"ok  {args.input} → {out}")
    print(f"    orig={len(data)}B  chunks={chunks}  mode={args.mode}  cols={args.cols}")
    print(f"    svg={os.path.getsize(out)}B  ratio={os.path.getsize(out)/len(data):.1f}x")

if __name__ == "__main__":
    main()
