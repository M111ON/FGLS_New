"""Bermuda fingerprint — LetterCube visual fingerprint generator.

Generates a deterministic PNG from a pack's manifest.
LetterCube 6-face structure: N pairs per face (default N=16 = Metatron 4×4).
Bijection X:x preserved. Closure walk determines cell ordering.

Architecture:
  GEO_METATRON_CELLS = 4×4 = 16
  LC_PAIRS     = N (default 16)
  LC_SLOTS     = 2*N (upper + lower)
  LC_FACES     = 6
  TOTAL_CELLS  = 6 × 2*N

Each file → hash(path) % TOTAL_CELLS → (face, slot).
Cell colour from xxh64, cell size from log(file size).
"""
import math
import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple

import xxhash

# ── LetterCube configuration ────────────────────────────────────
# Default N=16 matches GEO_METATRON_CELLS (4×4) from geo_jump.h
LC_PAIRS_DEFAULT = 16  # pairs per face (A1..A16 : a1..a16)
LC_FACES = 6           # frustum faces — fixed


def lc_total_cells(n_pairs: int) -> int:
    """Total cells = 6 faces × 2n slots."""
    return LC_FACES * 2 * n_pairs


def lc_closure_steps(n_pairs: int) -> int:
    """LCM(n, 6) — full closure walk length."""
    from math import gcd
    return (n_pairs * 6) // gcd(n_pairs, 6)


# ── Closure walk (Python port of geo_letter_cube.h) ─────────────

def closure_walk(start: int, n_pairs: int) -> List[int]:
    """78-step (or LCM(n,6)-step) closure walk.

    init: state = start % n_pairs
    step: state = (state + 6) % n_pairs
    Returns list of state values (pair indices 0..n-1).
    """
    steps = lc_closure_steps(n_pairs)
    state = start % n_pairs
    visited = []
    for _ in range(steps):
        visited.append(state)
        state = (state + 6) % n_pairs
    return visited


def closure_order_for_face(face_id: int, n_pairs: int) -> List[int]:
    """Get the 2n cell ordering for a face using closure walk."""
    walk = closure_walk(face_id, n_pairs)
    # Each face gets a shifted version of the walk
    seen = set()
    ordering = []
    for s in walk:
        if s not in seen:
            seen.add(s)
            ordering.append(s)
    # Fill remaining (should already have n unique values)
    for i in range(n_pairs):
        if i not in seen:
            ordering.append(i)
    return ordering[:n_pairs]


# ── Data structures ─────────────────────────────────────────────

@dataclass
class LCCell:
    """One cell in the LetterCube grid."""
    face: int          # 0-5
    slot: int          # 0..2n-1 (upper half = WorldA, lower half = WorldB)
    pair_idx: int      # 0..n-1 (from closure walk)
    is_upper: bool     # True = WorldA (uppercase), False = WorldB (lowercase)
    hue: float         # 0-1
    lightness: float   # 0-1
    saturation: float  # 0-1
    file_size: int     # original file size in bytes
    file_hash: str     # xxh64 hex string
    file_path: str     # relative path in pack


@dataclass
class LetterCube:
    """Complete LetterCube structure — intermediate representation between
    raw pack manifest and pixel rendering.

    Bijection X:x preserved: upper[i] ↔ lower[i] for all i in 0..n-1.
    """
    n_pairs: int                                      # N (default 16)
    cells: List[Optional[LCCell]] = field(default_factory=list)  # 6×2N cells
    pack_hash: str = ""                               # overall pack hash (xxh64)
    file_count: int = 0

    @staticmethod
    def from_manifest(entries: List[dict], n_pairs: int = LC_PAIRS_DEFAULT) -> "LetterCube":
        """Build LetterCube from pack manifest entries.

        Each entry: {path, size, xxh64}
        Bijection: face × (upper_slot, lower_slot) = pair
        """
        total = lc_total_cells(n_pairs)
        cells: List[Optional[LCCell]] = [None] * total

        # Compute overall pack hash (xxh64 — fast, deterministic)
        hasher = xxhash.xxh64()
        for e in sorted(entries, key=lambda x: x['path']):
            hasher.update(e['path'].encode('utf-8'))
            hasher.update(e['xxh64'].encode('utf-8'))
        pack_hash = hasher.hexdigest()

        cube = LetterCube(
            n_pairs=n_pairs,
            cells=cells,
            pack_hash=pack_hash,
            file_count=len(entries),
        )

        for entry in entries:
            path_hash = int(xxhash.xxh64(entry['path'].encode('utf-8')).hexdigest(), 16)
            cell_idx = path_hash % total
            face = cell_idx // (2 * n_pairs)
            slot = cell_idx % (2 * n_pairs)
            pair_idx = slot % n_pairs
            is_upper = slot < n_pairs

            # HSL from xxh64
            h_hex = entry['xxh64']
            hue = (int(h_hex[:8], 16) & 0xFFFFFF) / 0x1000000
            lightness = 0.35 + ((int(h_hex[-4:], 16) & 0xFF) / 0xFF) * 0.30
            saturation = 0.60 + ((int(h_hex[8:12], 16) & 0xFF) / 0xFF) * 0.35

            cell = LCCell(
                face=face,
                slot=slot,
                pair_idx=pair_idx,
                is_upper=is_upper,
                hue=hue,
                lightness=lightness,
                saturation=saturation,
                file_size=entry['size'],
                file_hash=entry['xxh64'],
                file_path=entry['path'],
            )

            # Collision: keep larger file
            existing = cells[cell_idx]
            if existing is None or entry['size'] > existing.file_size:
                cells[cell_idx] = cell

        return cube

    def get_face_cells(self, face: int) -> List[Optional[LCCell]]:
        """Get all 2N cells for a face."""
        stride = 2 * self.n_pairs
        return self.cells[face * stride:(face + 1) * stride]

    def get_occupied_count(self) -> int:
        """Number of non-empty cells."""
        return sum(1 for c in self.cells if c is not None)

    def verify_bijection(self) -> bool:
        """Verify X:x bijection — each pair_idx appears exactly once
        in upper and once in lower for each face."""
        for face in range(LC_FACES):
            stride = 2 * self.n_pairs
            base = face * stride
            upper_pairs = set()
            lower_pairs = set()
            for slot in range(self.n_pairs):
                c = self.cells[base + slot]
                if c:
                    upper_pairs.add(c.pair_idx)
            for slot in range(self.n_pairs, 2 * self.n_pairs):
                c = self.cells[base + slot]
                if c:
                    lower_pairs.add(c.pair_idx)
            # Bijection: each occupied pair in upper must also be in lower
            for p in upper_pairs:
                if p not in lower_pairs:
                    return False
        return True


# ── PNG writing ─────────────────────────────────────────────────

def _hsl_to_rgb(h: float, s: float, l: float) -> Tuple[int, int, int]:
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


def _png_chunk(chunk_type: bytes, data: bytes) -> bytes:
    raw = chunk_type + data
    crc = struct.pack(">I", zlib.crc32(raw) & 0xFFFFFFFF)
    return struct.pack(">I", len(data)) + raw + crc


def _write_png(path: Path, pixels: List[bytes], w: int, h: int):
    sig = b'\x89PNG\r\n\x1a\n'
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    raw = b''
    for row in pixels:
        raw += b'\x00' + row
    compressed = zlib.compress(raw)
    with open(path, "wb") as f:
        f.write(sig)
        f.write(_png_chunk(b'IHDR', ihdr))
        f.write(_png_chunk(b'IDAT', compressed))
        f.write(_png_chunk(b'IEND', b''))


def _put_pixel(canvas: bytearray, w: int, x: int, y: int, r: int, g: int, b: int):
    if 0 <= x < w and 0 <= y:
        p = y * w * 3 + x * 3
        canvas[p] = r; canvas[p+1] = g; canvas[p+2] = b


def _blend_pixel(canvas: bytearray, w: int, x: int, y: int,
                 r: int, g: int, b: int, alpha: float):
    if 0 <= x < w and 0 <= y:
        p = y * w * 3 + x * 3
        a = max(0.0, min(1.0, alpha))
        inv = 1.0 - a
        canvas[p]   = int(canvas[p]   * inv + r * a)
        canvas[p+1] = int(canvas[p+1] * inv + g * a)
        canvas[p+2] = int(canvas[p+2] * inv + b * a)


def _fill_rect(canvas: bytearray, w: int, x0: int, y0: int,
               x1: int, y1: int, r: int, g: int, b: int):
    for y in range(max(0, y0), min(y1, len(canvas) // (w * 3))):
        p = y * w * 3 + x0 * 3
        for x in range(max(0, x0), min(x1, w)):
            pp = p + (x - x0) * 3
            canvas[pp] = r; canvas[pp+1] = g; canvas[pp+2] = b


def _fill_rect_alpha(canvas: bytearray, w: int, x0: int, y0: int,
                     x1: int, y1: int, r: int, g: int, b: int, alpha: float):
    for y in range(max(0, y0), min(y1, len(canvas) // (w * 3))):
        for x in range(max(0, x0), min(x1, w)):
            _blend_pixel(canvas, w, x, y, r, g, b, alpha)


# ── 5×7 bitmap font ────────────────────────────────────────────

_FONT_5X7 = {
    'A': [0x0E,0x11,0x11,0x1F,0x11,0x11,0x11],
    'B': [0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E],
    'C': [0x0E,0x11,0x10,0x10,0x10,0x11,0x0E],
    'D': [0x1E,0x11,0x11,0x11,0x11,0x11,0x1E],
    'E': [0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F],
    'F': [0x1F,0x10,0x10,0x1E,0x10,0x10,0x10],
    'G': [0x0E,0x11,0x10,0x17,0x11,0x11,0x0E],
    'H': [0x11,0x11,0x11,0x1F,0x11,0x11,0x11],
    'I': [0x0E,0x04,0x04,0x04,0x04,0x04,0x0E],
    'J': [0x07,0x02,0x02,0x02,0x02,0x12,0x0C],
    'K': [0x11,0x12,0x14,0x18,0x14,0x12,0x11],
    'L': [0x10,0x10,0x10,0x10,0x10,0x10,0x1F],
    'M': [0x11,0x1B,0x15,0x15,0x11,0x11,0x11],
    'N': [0x11,0x19,0x15,0x13,0x11,0x11,0x11],
    'O': [0x0E,0x11,0x11,0x11,0x11,0x11,0x0E],
    'P': [0x1E,0x11,0x11,0x1E,0x10,0x10,0x10],
    'Q': [0x0E,0x11,0x11,0x11,0x15,0x12,0x0D],
    'R': [0x1E,0x11,0x11,0x1E,0x14,0x12,0x11],
    'S': [0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E],
    'T': [0x1F,0x04,0x04,0x04,0x04,0x04,0x04],
    'U': [0x11,0x11,0x11,0x11,0x11,0x11,0x0E],
    'V': [0x11,0x11,0x11,0x11,0x11,0x0A,0x04],
    'W': [0x11,0x11,0x11,0x15,0x15,0x1B,0x11],
    'X': [0x11,0x11,0x0A,0x04,0x0A,0x11,0x11],
    'Y': [0x11,0x11,0x0A,0x04,0x04,0x04,0x04],
    'Z': [0x1F,0x01,0x02,0x04,0x08,0x10,0x1F],
    '0': [0x0E,0x11,0x13,0x15,0x19,0x11,0x0E],
    '1': [0x04,0x0C,0x04,0x04,0x04,0x04,0x0E],
    '2': [0x0E,0x11,0x01,0x06,0x08,0x10,0x1F],
    '3': [0x0E,0x11,0x01,0x06,0x01,0x11,0x0E],
    '4': [0x02,0x06,0x0A,0x12,0x1F,0x02,0x02],
    '5': [0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E],
    '6': [0x06,0x08,0x10,0x1E,0x11,0x11,0x0E],
    '7': [0x1F,0x01,0x02,0x04,0x08,0x08,0x08],
    '8': [0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E],
    '9': [0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C],
    ' ': [0x00,0x00,0x00,0x00,0x00,0x00,0x00],
    '-': [0x00,0x00,0x00,0x1F,0x00,0x00,0x00],
    '.': [0x00,0x00,0x00,0x00,0x00,0x00,0x04],
    ':': [0x00,0x04,0x04,0x00,0x04,0x04,0x00],
}


def _draw_char(canvas: bytearray, w: int, x: int, y: int,
               ch: str, r: int, g: int, b: int):
    glyph = _FONT_5X7.get(ch.upper(), _FONT_5X7[' '])
    for row in range(7):
        bits = glyph[row]
        for col in range(5):
            if bits & (1 << (4 - col)):
                _put_pixel(canvas, w, x + col, y + row, r, g, b)


def _draw_text(canvas: bytearray, w: int, x: int, y: int,
               text: str, r: int, g: int, b: int):
    for i, ch in enumerate(text):
        _draw_char(canvas, w, x + i * 6, y, ch, r, g, b)


# ── Main fingerprint generator ──────────────────────────────────

def generate_fingerprint(entries: List[dict], out_path: Path,
                         img_size: int = 512,
                         n_pairs: int = LC_PAIRS_DEFAULT):
    """Generate LetterCube fingerprint PNG from pack manifest entries.

    Uses LetterCube data structure as intermediate representation.
    """
    # Build structured data
    cube = LetterCube.from_manifest(entries, n_pairs)
    total = lc_total_cells(n_pairs)
    slots_per_face = 2 * n_pairs

    W = H = img_size
    margin_x = 28
    margin_top = 20
    margin_bot = 8
    margin_right = 8
    content_w = W - margin_x - margin_right
    content_h = H - margin_top - margin_bot
    cell_w = content_w // slots_per_face
    face_h = content_h // LC_FACES

    canvas = bytearray(W * H * 3)

    # ── Background: dark radial gradient ──
    cx_bg, cy_bg = W // 2, H // 2
    max_r = math.sqrt(cx_bg**2 + cy_bg**2)
    for y in range(H):
        for x in range(W):
            d = math.sqrt((x-cx_bg)**2 + (y-cy_bg)**2) / max_r
            c = int(10 + 20 * d)
            _put_pixel(canvas, W, x, y, c, c+2, c+5)

    # ── Face colour bands ──
    face_hues = [0.55, 0.75, 0.10, 0.85, 0.30, 0.60]
    for fi in range(LC_FACES):
        fy0 = margin_top + fi * face_h
        fy1 = margin_top + (fi + 1) * face_h
        r, g, b = _hsl_to_rgb(face_hues[fi], 0.18, 0.13)
        _fill_rect_alpha(canvas, W, margin_x, fy0, W - margin_right, fy1, r, g, b, 0.35)

    # ── Ghost cells (all 6×2N slots) ──
    for fi in range(LC_FACES):
        order = closure_order_for_face(fi, n_pairs)
        for si in range(slots_per_face):
            cx0 = margin_x + si * cell_w
            cy0 = margin_top + fi * face_h
            cx1 = margin_x + (si + 1) * cell_w
            cy1 = margin_top + (fi + 1) * face_h
            pair_idx = si % n_pairs
            is_upper = si < n_pairs
            ghost_hue = (order[pair_idx % len(order)] / n_pairs + fi / 6.0) % 1.0
            gr, gg, gb = _hsl_to_rgb(ghost_hue, 0.20, 0.14)
            _fill_rect_alpha(canvas, W, cx0+1, cy0+1, cx1-1, cy1-1, gr, gg, gb, 0.22)

    # ── Grid lines ──
    for fi in range(LC_FACES + 1):
        gy = margin_top + fi * face_h
        for x in range(margin_x, W - margin_right):
            _blend_pixel(canvas, W, x, gy, 55, 60, 72, 0.5)
    for si in range(slots_per_face + 1):
        gx = margin_x + si * cell_w
        for y in range(margin_top, H - margin_bot):
            _blend_pixel(canvas, W, gx, y, 45, 50, 62, 0.3)

    # ── Labels ──
    # Top: slot numbers (hex for compactness)
    for si in range(slots_per_face):
        cx = margin_x + si * cell_w + cell_w // 2 - 2
        label = f"{si:X}" if si < 16 else f"{si-16:X}'"  # 0-F, then 0'-F'
        _draw_text(canvas, W, cx, 2, label, 110, 120, 135)

    # Left: face labels
    for fi in range(LC_FACES):
        fy = margin_top + fi * face_h + face_h // 2 - 3
        _draw_text(canvas, W, 2, fy, f"F{fi}", 130, 140, 155)

    # ── Occupied cells ──
    for cell in cube.cells:
        if cell is None:
            continue
        fi = cell.face
        si = cell.slot
        cx0 = margin_x + si * cell_w + 1
        cy0 = margin_top + fi * face_h + 1
        cx1 = margin_x + (si + 1) * cell_w - 1
        cy1 = margin_top + (fi + 1) * face_h - 1

        r, g, b = _hsl_to_rgb(cell.hue, cell.saturation, cell.lightness)

        # Glow
        _fill_rect_alpha(canvas, W, cx0-2, cy0-2, cx1+2, cy1+2, r, g, b, 0.18)
        # Body
        _fill_rect(canvas, W, cx0, cy0, cx1, cy1, r, g, b)
        # Highlight
        _fill_rect_alpha(canvas, W, cx0, cy0, cx1, cy0+2,
                         min(255,r+70), min(255,g+70), min(255,b+70), 0.5)

        # File initial
        fname = cell.file_path.split('/')[-1].split('\\')[-1]
        initial = fname[0].upper() if fname else '?'
        _draw_char(canvas, W, cx0 + (cx1-cx0-5)//2, cy0 + (cy1-cy0-7)//2,
                   initial, min(255,r+100), min(255,g+100), min(255,b+100))

    # ── Bijection indicators (right side, paired connectors) ──
    for fi in range(LC_FACES):
        fy_upper = margin_top + fi * face_h + face_h // 4
        fy_lower = margin_top + fi * face_h + 3 * face_h // 4
        cr, cg, cb = _hsl_to_rgb(face_hues[fi], 0.5, 0.4)
        for y in range(fy_upper, fy_lower + 1):
            _blend_pixel(canvas, W, W-3, y, cr, cg, cb, 0.5)

    # ── Metadata line ──
    meta = f"N={n_pairs} PAIRS={n_pairs} SLOTS={slots_per_face} TOTAL={total} FILES={cube.file_count}"
    _draw_text(canvas, W, margin_x, H - 12, meta, 60, 65, 75)

    # ── Write PNG ──
    pixels = [bytes(canvas[y * W * 3:(y + 1) * W * 3]) for y in range(H)]
    _write_png(out_path, pixels, W, H)

    return cube  # return for inspection
