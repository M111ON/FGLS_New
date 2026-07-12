"""Bermuda fingerprint — visual fingerprint generator.

Generates a deterministic 512×512 PNG from a pack's manifest.
Each file = one block, colour from xxh64 hash, size from log file size.
Mirror-symmetric layout for aesthetic recognition.
"""
import json
import struct
import zlib
from pathlib import Path
from typing import List


def _hsl_to_rgb(h: float, s: float, l: float) -> tuple:
    """Convert HSL (0-1) to RGB (0-255)."""
    if s == 0:
        v = int(l * 255)
        return (v, v, v)
    q = l * (1 + s) if l < 0.5 else l + s - l * s
    p = 2 * l - q
    def _t(tc):
        if tc < 0: tc += 1
        if tc > 1: tc -= 1
        if tc < 1/6: return p + (q - p) * 6 * tc
        if tc < 1/2: return q
        if tc < 2/3: return p + (q - p) * (2/3 - tc) * 6
        return p
    return (int(_t(h + 1/3) * 255), int(_t(h) * 255), int(_t(h - 1/3) * 255))


def _file_hue(file_hash: str) -> float:
    """Derive hue 0-1 from xxh64 hex string."""
    return (int(file_hash[:8], 16) & 0xFFFFFF) / 0x1000000


def _file_luminosity(file_hash: str) -> float:
    """Derive lightness offset from hash tail."""
    return 0.35 + ((int(file_hash[-4:], 16) & 0xFF) / 0xFF) * 0.3


def _png_chunk(chunk_type: bytes, data: bytes) -> bytes:
    """Build a PNG chunk: length + type + data + CRC."""
    raw = chunk_type + data
    crc = struct.pack(">I", zlib.crc32(raw) & 0xFFFFFFFF)
    return struct.pack(">I", len(data)) + raw + crc


def _write_png(path: Path, pixels: List[bytes], w: int, h: int):
    """Write indexed or RGB PNG from row-filtered pixel data."""
    sig = b'\x89PNG\r\n\x1a\n'
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    raw = b''
    for row in pixels:
        raw += b'\x00' + row  # filter=none per row
    compressed = zlib.compress(raw)
    with open(path, "wb") as f:
        f.write(sig)
        f.write(_png_chunk(b'IHDR', ihdr))
        f.write(_png_chunk(b'IDAT', compressed))
        f.write(_png_chunk(b'IEND', b''))


def generate_fingerprint(entries: List[dict], out_path: Path, img_size: int = 512):
    """Generate unique visual fingerprint PNG from pack manifest entries.

    Layout: mirror-symmetric radial blocks.
    Each entry → coloured block, position from hash, size from log(file size).
    """
    if not entries:
        # Empty pack → solid grey with X pattern
        pixels = []
        for y in range(img_size):
            row = bytearray()
            for x in range(img_size):
                dx = abs(x - img_size//2)
                dy = abs(y - img_size//2)
                d = (dx + dy) / img_size
                c = int(20 + 30 * d)
                row.extend([c, c, c])
            pixels.append(bytes(row))
        _write_png(out_path, pixels, img_size, img_size)
        return

    half = img_size // 2
    canvas = bytearray(half * half * 3)

    # Sort entries by size (largest first) for stable layout
    sorted_entries = sorted(entries, key=lambda e: e['size'], reverse=True)

    # Assign cells: we have half×half grid, place files sequentially
    # but with position seeded from path hash for visual variation
    n = len(sorted_entries)
    # Calculate grid dimensions for the quadrant
    cells_per_side = max(2, int(half / max(8, min(64, half // max(1, n//4 + 1)))))
    cell_size = max(4, half // cells_per_side)
    grid_cells = min(cells_per_side, half // cell_size)

    for idx, entry in enumerate(sorted_entries):
        h = entry['xxh64']
        hue = _file_hue(h)
        lum = _file_luminosity(h)
        # Deterministic position from path hash
        path_seed = sum(ord(c) for c in entry['path']) & 0xFFFF
        gx = (path_seed * (idx + 1)) % grid_cells
        gy = (idx * 7 + (path_seed >> 4)) % grid_cells

        # Size: log scale, min 4px, max cell_size-2
        sz = max(4, min(cell_size - 2,
                        int(cell_size * (entry['size'] ** 0.3) / (1024 ** 0.3))))

        r, g, b = _hsl_to_rgb(hue, 0.75, lum)

        ox = gx * cell_size + (cell_size - sz) // 2
        oy = gy * cell_size + (cell_size - sz) // 2

        for dy in range(sz):
            row_start = (oy + dy) * half * 3 + ox * 3
            for dx in range(sz):
                p = row_start + dx * 3
                # Only draw if not already filled (layering)
                if canvas[p] == 0 and canvas[p+1] == 0 and canvas[p+2] == 0:
                    canvas[p] = r
                    canvas[p+1] = g
                    canvas[p+2] = b

    # If no entries drew (all zero), fill with seed pattern
    if all(v == 0 for v in canvas):
        seed = sum(int(e['xxh64'][:4], 16) for e in sorted_entries[:5])
        rng_state = seed
        for i in range(0, len(canvas), 3):
            rng_state = (rng_state * 1103515245 + 12345) & 0x7FFFFFFF
            c = rng_state & 0xFF
            canvas[i] = c
            canvas[i+1] = (c * 3) & 0xFF
            canvas[i+2] = (c * 7) & 0xFF

    # Mirror to full canvas
    full = bytearray(img_size * img_size * 3)
    for y in range(half):
        row_src = canvas[y * half * 3:(y + 1) * half * 3]
        for x in range(half):
            src_p = x * 3
            r, g, b = row_src[src_p:src_p + 3]
            # Four quadrants
            # Q1 (top-left)
            p1 = y * img_size * 3 + x * 3
            full[p1] = r; full[p1+1] = g; full[p1+2] = b
            # Q2 (top-right)
            p2 = y * img_size * 3 + (img_size - 1 - x) * 3
            full[p2] = r; full[p2+1] = g; full[p2+2] = b
            # Q3 (bottom-left)
            p3 = (img_size - 1 - y) * img_size * 3 + x * 3
            full[p3] = r; full[p3+1] = g; full[p3+2] = b
            # Q4 (bottom-right)
            p4 = (img_size - 1 - y) * img_size * 3 + (img_size - 1 - x) * 3
            full[p4] = r; full[p4+1] = g; full[p4+2] = b

    pixels = [bytes(full[y * img_size * 3:(y + 1) * img_size * 3]) for y in range(img_size)]
    _write_png(out_path, pixels, img_size, img_size)
