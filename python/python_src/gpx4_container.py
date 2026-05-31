"""
gpx4_container.py — Multi-layer GeoPixel container (.gpx4)

Python port of gpx4_container.h (v2).

File layout (all big-endian):
  [FILE_HEADER]   16B
    magic    4B  'GPX4'
    version  1B  0x02
    flags    1B  reserved=0
    n_layers 2B
    tw       2B  tile columns
    th       2B  tile rows
    n_tiles  4B  tw*th

  [LAYER_TABLE]   n_layers * 14B each
    type     1B  GPX4_LAYER_*
    flags    1B  GPX4_LFLAG_*
    name     4B  4-char tag
    offset   4B  byte offset of layer data from file start
    size     4B  byte size of layer data

  [TILE_TABLE]    tiled layers only * n_tiles * 8B per layer
    rows     2B  O4 grid rows (0 = tile absent/noise)
    tflags   2B  GPX4_TILE_*
    blob_sz  4B  compressed blob size in bytes

  [LAYER DATA]    raw bytes at offsets declared in LAYER_TABLE
"""
from __future__ import annotations
import struct, io, logging
from pathlib import Path
from typing import Optional, BinaryIO
from dataclasses import dataclass, field
from enum import IntEnum

logger = logging.getLogger("engine.gpx4")

# ── Constants ───────────────────────────────────────────
GPX4_MAGIC          = 0x47505834  # 'GPX4'
GPX4_VERSION        = 0x02
GPX4_MAX_LAYERS     = 64

GPX4_LAYER_O4       = 0x01
GPX4_LAYER_FALLBACK = 0x02
GPX4_LAYER_DELTA    = 0x03
GPX4_LAYER_META     = 0x04
GPX4_LAYER_ANIM_HDR = 0x05
GPX4_LAYER_GEO      = 0x06

GPX4_LFLAG_KEYFRAME = 0x01
GPX4_LFLAG_ZSTD     = 0x02

GPX4_TILE_PRESENT   = 0x0001
GPX4_TILE_REF       = 0x0002
GPX4_TILE_SKIP      = 0x0004

GPX4_FILE_HDR_SZ    = 16
GPX4_LAYER_ENTRY_SZ = 14
GPX4_TILE_ENTRY_SZ  = 8
GPX4_ANIM_HDR_SZ    = 16
GPX4_GEO_ADDR_SZ    = 4

LAYER_TYPE_NAMES = {
    GPX4_LAYER_O4: "O4",
    GPX4_LAYER_FALLBACK: "FALLBACK",
    GPX4_LAYER_DELTA: "DELTA",
    GPX4_LAYER_META: "META",
    GPX4_LAYER_ANIM_HDR: "ANIM_HDR",
    GPX4_LAYER_GEO: "GEO",
}


# ── Data classes ────────────────────────────────────────

@dataclass
class Gpx4AnimHdr:
    n_frames: int = 0
    fps_num: int = 24
    fps_den: int = 1
    keyframe_interval: int = 0
    width_px: int = 0
    height_px: int = 0
    flags: int = 0
    reserved: int = 0

    @staticmethod
    def from_bytes(data: bytes) -> Gpx4AnimHdr:
        (nf, fn, fd, ki, wp, hp, fl, rs) = struct.unpack_from(">HHHHHHHH", data, 0)
        return Gpx4AnimHdr(nf, fn, fd, ki, wp, hp, fl, rs)

    def to_bytes(self) -> bytes:
        return struct.pack(">HHHHHHHH",
            self.n_frames, self.fps_num, self.fps_den,
            self.keyframe_interval, self.width_px, self.height_px,
            self.flags, self.reserved)


@dataclass
class Gpx4TileEntry:
    rows: int = 0
    tflags: int = 0
    blob_sz: int = 0


@dataclass
class Gpx4LayerDef:
    """Layer definition for writing."""
    type: int
    lflags: int = 0
    name: str = "    "
    data: Optional[bytes] = None
    tiles: Optional[list[Gpx4TileEntry]] = None

    @property
    def size(self) -> int:
        return len(self.data) if self.data else 0


@dataclass
class Gpx4LayerInfo:
    """Layer information from reading."""
    type: int
    lflags: int
    name: str
    offset: int
    size: int


@dataclass
class Gpx4GeoAddr:
    packed: int

    @property
    def pent(self) -> int: return (self.packed >> 28) & 0xF
    @property
    def hilbert(self) -> int: return (self.packed >> 14) & 0x3FFF
    @property
    def sub(self) -> int: return self.packed & 0x3FFF


# ── Layer name helpers ─────────────────────────────────

def gpx4_frame_name(is_keyframe: bool, idx: int) -> str:
    prefix = 'F' if is_keyframe else 'D'
    return f"{prefix}{idx % 1000:03d}"


def gpx4_frame_index(name: str) -> int:
    if not name or name[0] not in ('F', 'D'):
        return -1
    try:
        return int(name[1:4])
    except ValueError:
        return -1


def gpx4_is_keyframe_name(name: str) -> bool:
    return bool(name) and name[0] == 'F'


# ════════════════════════════════════════════════════════
# GPX4 File class
# ════════════════════════════════════════════════════════

class Gpx4File:
    """Read/write GPX4 container files."""

    def __init__(self):
        self.tw: int = 0
        self.th: int = 0
        self.n_layers: int = 0
        self.n_tiles: int = 0
        self.layers: list[Gpx4LayerInfo] = []
        self.tile_tables: dict[int, list[Gpx4TileEntry]] = {}  # layer_index → tiles
        self._raw: Optional[bytes] = None

    # ── Write ───────────────────────────────────────────

    @staticmethod
    def write(
        path: str | Path,
        tw: int,
        th: int,
        layers: list[Gpx4LayerDef],
    ) -> dict:
        """Write a GPX4 file. Returns metadata dict."""
        if len(layers) > GPX4_MAX_LAYERS:
            raise ValueError(f"Too many layers: {len(layers)} > {GPX4_MAX_LAYERS}")
        nt = tw * th
        n_tiled = sum(1 for l in layers if l.tiles is not None)

        buf = io.BytesIO()

        # FILE_HEADER placeholder
        hdr_start = buf.tell()
        buf.write(b'\x00' * GPX4_FILE_HDR_SZ)

        # LAYER_TABLE
        lt_start = buf.tell()
        layer_offsets = []
        for l in layers:
            layer_offsets.append(0)  # placeholder
            buf.write(struct.pack(">BB4s", l.type, l.lflags, l.name.encode('ascii')))
            buf.write(b'\x00' * 8)  # offset(4) + size(4) placeholder

        # TILE_TABLE(s)
        tt_starts = []
        for i, l in enumerate(layers):
            tt_starts.append(buf.tell())
            if l.tiles is not None:
                for t in l.tiles:
                    buf.write(struct.pack(">HHI", t.rows, t.tflags, t.blob_sz))

        # LAYER DATA
        data_starts = []
        for i, l in enumerate(layers):
            data_starts.append(buf.tell())
            if l.data:
                buf.write(l.data)

        # Write FILE_HEADER
        file_end = buf.tell()
        buf.seek(hdr_start)
        fh = struct.pack(">4sBBHHHI",
            b'GPX4', GPX4_VERSION, 0,
            len(layers), tw, th, nt)
        buf.write(fh)

        # Write LAYER_TABLE with real offsets/sizes
        buf.seek(lt_start)
        for i, l in enumerate(layers):
            buf.write(struct.pack(">BB4s", l.type, l.lflags, l.name.encode('ascii')))
            buf.write(struct.pack(">II", data_starts[i], l.size if l.data else 0))

        buf.seek(file_end)
        result = buf.getvalue()
        Path(path).write_bytes(result)

        total_data = sum(l.size for l in layers if l.data)
        file_size = len(result)
        return {
            "path": str(path),
            "file_size": file_size,
            "tw": tw,
            "th": th,
            "n_tiles": nt,
            "n_layers": len(layers),
            "n_tiled_layers": n_tiled,
            "header_overhead": file_size - total_data,
            "layers": [
                {
                    "type": LAYER_TYPE_NAMES.get(l.type, f"0x{l.type:02x}"),
                    "name": l.name.strip(),
                    "size": l.size if l.data else 0,
                    "tiled": l.tiles is not None,
                }
                for l in layers
            ],
        }

    # ── Read ────────────────────────────────────────────

    @staticmethod
    def open(path: str | Path) -> Gpx4File:
        """Open and parse a GPX4 file."""
        raw = Path(path).read_bytes()
        gf = Gpx4File()
        gf._raw = raw
        gf._parse(raw)
        return gf

    def _parse(self, raw: bytes):
        p = 0
        if raw[p:p+4] != b'GPX4':
            raise ValueError(f"Bad GPX4 magic: {raw[p:p+4]!r}")
        p += 4
        ver = raw[p]; p += 1
        entry_sz = GPX4_LAYER_ENTRY_SZ if ver >= 0x02 else 12
        p += 1  # flags
        self.n_layers = struct.unpack_from(">H", raw, p)[0]; p += 2
        self.tw = struct.unpack_from(">H", raw, p)[0]; p += 2
        self.th = struct.unpack_from(">H", raw, p)[0]; p += 2
        self.n_tiles = struct.unpack_from(">I", raw, p)[0]; p += 4

        # LAYER_TABLE
        self.layers = []
        for _ in range(self.n_layers):
            typ = raw[p]; p += 1
            lf = raw[p]; p += 1
            name = raw[p:p+4].decode('ascii', errors='replace'); p += 4
            offset = struct.unpack_from(">I", raw, p)[0]; p += 4
            sz = struct.unpack_from(">I" if ver >= 0x02 else ">H", raw, p)[0]
            p += 4 if ver >= 0x02 else 2
            self.layers.append(Gpx4LayerInfo(typ, lf, name, offset, sz))

        # TILE_TABLE(s)
        nt = self.n_tiles
        for i, li in enumerate(self.layers):
            if li.type in (GPX4_LAYER_META, GPX4_LAYER_ANIM_HDR, GPX4_LAYER_GEO):
                continue
            tiles = []
            for _ in range(nt):
                rows = struct.unpack_from(">H", raw, p)[0]; p += 2
                tflags = struct.unpack_from(">H", raw, p)[0]; p += 2
                blob_sz = struct.unpack_from(">I", raw, p)[0]; p += 4
                tiles.append(Gpx4TileEntry(rows, tflags, blob_sz))
            self.tile_tables[i] = tiles

    # ── Accessors ───────────────────────────────────────

    def layer_data(self, type_code: int) -> Optional[bytes]:
        """Get raw data for first layer matching type."""
        for li in self.layers:
            if li.type == type_code:
                return self._raw[li.offset:li.offset + li.size] if self._raw else None
        return None

    def layer_data_by_name(self, name: str) -> Optional[bytes]:
        """Get raw data for first layer matching name."""
        for li in self.layers:
            if li.name.strip() == name.strip():
                return self._raw[li.offset:li.offset + li.size] if self._raw else None
        return None

    def tile_table(self, type_code: int) -> Optional[list[Gpx4TileEntry]]:
        for i, li in enumerate(self.layers):
            if li.type == type_code:
                return self.tile_tables.get(i)
        return None

    def tile_table_by_name(self, name: str) -> Optional[list[Gpx4TileEntry]]:
        for i, li in enumerate(self.layers):
            if li.name.strip() == name.strip():
                return self.tile_tables.get(i)
        return None

    def layer_index(self, name: str) -> int:
        for i, li in enumerate(self.layers):
            if li.name.strip() == name.strip():
                return i
        return -1

    def geo_addrs(self) -> Optional[list[Gpx4GeoAddr]]:
        """Parse and return GEO layer addresses, if present."""
        data = self.layer_data(GPX4_LAYER_GEO)
        if not data:
            return None
        n = len(data) // GPX4_GEO_ADDR_SZ
        addrs = []
        for i in range(n):
            packed = struct.unpack_from(">I", data, i * GPX4_GEO_ADDR_SZ)[0]
            addrs.append(Gpx4GeoAddr(packed))
        return addrs

    def anim_hdr(self) -> Optional[Gpx4AnimHdr]:
        """Parse and return animation header, if present."""
        data = self.layer_data(GPX4_LAYER_ANIM_HDR)
        if not data:
            return None
        return Gpx4AnimHdr.from_bytes(data)

    def summary(self) -> dict:
        """Return a human-readable summary of the GPX4 file."""
        return {
            "n_layers": self.n_layers,
            "tile_grid": f"{self.tw}x{self.th}",
            "n_tiles": self.n_tiles,
            "layers": [
                {
                    "type": LAYER_TYPE_NAMES.get(li.type, f"0x{li.type:02x}"),
                    "name": li.name.strip(),
                    "size": li.size,
                    "flags": li.lflags,
                    "has_tile_table": i in self.tile_tables,
                    "tile_count": len(self.tile_tables[i]) if i in self.tile_tables else 0,
                }
                for i, li in enumerate(self.layers)
            ],
        }


# ════════════════════════════════════════════════════════
# O4 Grid decode + preview render
# ════════════════════════════════════════════════════════

O4_GRID_W = 27
O4_CHUNK_BYTES = 3


def _geo_pixel_encode(idx: int, W: int = 27) -> tuple:
    idx_mod = idx % W
    r = ((idx_mod % 27) << 3) | (idx_mod % 6)
    g = ((idx_mod % 9) << 4) | (idx_mod % 26 & 0xF)
    b = idx_mod % 144
    return (r & 0xFF, g & 0xFF, b & 0xFF)


def o4_decode_grid(pixels: list[list[tuple]], n_chunks: int, out_sz: int) -> bytes:
    """Decode O4 pixel grid back to original blob bytes."""
    buf = bytearray(out_sz)
    for i in range(n_chunks):
        slot_idx = i % 6912
        row = slot_idx // O4_GRID_W
        col = slot_idx % O4_GRID_W
        if row >= len(pixels) or col >= len(pixels[row]):
            continue
        pr, pg, pb = pixels[row][col]
        geo = _geo_pixel_encode(slot_idx, O4_GRID_W)
        b0 = pr ^ geo[0]
        b1 = pg ^ geo[1]
        b2 = pb ^ geo[2]
        base = i * 3
        if base + 0 < out_sz:
            buf[base + 0] = b0
        if base + 1 < out_sz:
            buf[base + 1] = b1
        if base + 2 < out_sz:
            buf[base + 2] = b2
    return bytes(buf)


def o4_grid_to_svg(pixels: list[list[tuple]], cell_sz: int = 4) -> str:
    """Render O4 pixel grid as inline SVG."""
    h = len(pixels)
    w = len(pixels[0]) if h > 0 else O4_GRID_W
    sw = w * cell_sz
    sh = h * cell_sz
    rects = []
    for y in range(h):
        for x in range(w):
            r, g, b = pixels[y][x]
            hex_c = "#%02x%02x%02x" % (r, g, b)
            rects.append(
                '<rect x="%d" y="%d" width="%d" height="%d" '
                'fill="%s" stroke="#333" stroke-width="0.3"/>'
                % (x * cell_sz, y * cell_sz, cell_sz, cell_sz, hex_c))
    svg = (
        '<svg xmlns="http://www.w3.org/2000/svg" '
        'width="%d" height="%d" shape-rendering="crispEdges">'
        '%s</svg>'
    ) % (sw, sh, "".join(rects))
    return svg


def gpx4_decode_o4_layer(layer_data: bytes, n_tiles: int) -> dict:
    """Decode a GPX4 O4 layer's raw data (PNG blob) to tile info + SVG preview.

    For this Python-level preview, we don't require Pillow/PIL.
    Instead, we return:
      - raw_size: layer_data size
      - n_tiles: tile count
      - note: full preview requires PIL for PNG decode
      - tile_blob_sizes: per-tile blob sizes (from tile table)
      - svg: minimal SVG grid if data is raw RGB (not PNG)
    """
    # Try PIL for PNG decode
    try:
        from PIL import Image
        import io
        img = Image.open(io.BytesIO(layer_data))
        if img.mode != 'RGB':
            img = img.convert('RGB')
        w, h_img = img.size
        pixels_rgb = list(img.getdata())
        grid = []
        for y in range(h_img):
            row = []
            for x in range(w_img):
                idx = y * w_img + x
                row.append(pixels_rgb[idx][:3])
            grid.append(row)
        svg = o4_grid_to_svg(grid, cell_sz=4)
        return {
            "decoded": True,
            "png": False,
            "width": w_img,
            "height": h_img,
            "svg": svg,
            "n_pixels": w_img * h_img,
            "note": "Decoded via PIL",
        }
    except ImportError:
        return {
            "decoded": False,
            "png": False,
            "raw_size": len(layer_data),
            "n_tiles": n_tiles,
            "note": "PIL not available. Install Pillow for PNG decode.",
        }
    except Exception as e:
        # Might be raw RGB data (not PNG)
        # Try to interpret as raw 27×N RGB
        data_len = len(layer_data)
        if data_len >= O4_GRID_W * 3 and data_len % 3 == 0:
            h_raw = data_len // (O4_GRID_W * 3)
            grid = []
            for y in range(h_raw):
                row = []
                for x in range(O4_GRID_W):
                    off = (y * O4_GRID_W + x) * 3
                    if off + 2 < data_len:
                        row.append((layer_data[off], layer_data[off + 1], layer_data[off + 2]))
                grid.append(row)
            svg = o4_grid_to_svg(grid, cell_sz=3)
            return {
                "decoded": True,
                "png": False,
                "width": O4_GRID_W,
                "height": h_raw,
                "svg": svg,
                "n_pixels": len(grid) * O4_GRID_W,
                "note": "Raw RGB data (not PNG)",
            }
        return {
            "decoded": False,
            "png": False,
            "raw_size": len(layer_data),
            "error": str(e),
            "n_tiles": n_tiles,
            "note": "Not a valid image format",
        }


def gpx4_timeline(path: str | Path) -> dict:
    """Extract timeline info from a GPX4 animation file.

    Returns ordered frames (keyframes + deltas) with metadata + SVG preview.
    """
    gf = Gpx4File.open(path)
    raw = gf._raw
    raw_sz = len(raw) if raw else 0

    # Collect frame layers in order
    frame_layers = []
    for i, li in enumerate(gf.layers):
        fi = gpx4_frame_index(li.name)
        if fi >= 0:
            is_kf = gpx4_is_keyframe_name(li.name)
            frame_layers.append((fi, is_kf, li, i))

    frame_layers.sort(key=lambda x: x[0])

    anim = gf.anim_hdr()
    frames = []
    for fi, is_kf, li, li_idx in frame_layers:
        data = gf.layer_data_by_name(li.name)
        tiles = gf.tile_table_by_name(li.name)
        tile_info = []
        if tiles:
            present = sum(1 for t in tiles if t.rows > 0)
            total_sz = sum(t.blob_sz for t in tiles)
            tile_info = {
                "n_tiles": len(tiles),
                "present": present,
                "total_blob_sz": total_sz,
            }

        preview = None
        if data and is_kf:
            try:
                preview = gpx4_decode_o4_layer(data, len(tiles) if tiles else 0)
            except Exception:
                preview = {"decoded": False, "error": "decode failed"}

        fr = {
            "index": fi,
            "type": "keyframe" if is_kf else "delta",
            "name": li.name,
            "layer_type": LAYER_TYPE_NAMES.get(li.type, f"0x{li.type:02x}"),
            "size": li.size,
            "flags": li.lflags,
            "tiles": tile_info,
            "preview": preview,
        }
        frames.append(fr)

    return {
        "path": str(path),
        "file_size": raw_sz,
        "n_layers": gf.n_layers,
        "tw": gf.tw,
        "th": gf.th,
        "n_tiles": gf.n_tiles,
        "animation_header": {
            "n_frames": anim.n_frames if anim else 0,
            "fps": f"{anim.fps_num}/{anim.fps_den}" if anim else "N/A",
            "keyframe_interval": anim.keyframe_interval if anim else 0,
            "width_px": anim.width_px if anim else 0,
            "height_px": anim.height_px if anim else 0,
        } if anim else None,
        "frames": frames,
    }


# ════════════════════════════════════════════════════════
# Convenience functions
# ════════════════════════════════════════════════════════

def gpx4_info(path: str | Path) -> dict:
    """Read GPX4 header metadata."""
    gf = Gpx4File.open(path)
    raw = gf._raw
    result = gf.summary()
    result["file_size"] = len(raw) if raw else 0
    return result


def gpx4_extract_layer(
    path: str | Path,
    layer_name: str,
    output_path: str | Path,
) -> dict:
    """Extract a single layer's data to a file."""
    gf = Gpx4File.open(path)
    data = gf.layer_data_by_name(layer_name)
    if data is None:
        raise ValueError(f"Layer '{layer_name}' not found")
    Path(output_path).write_bytes(data)
    return {
        "source": str(path),
        "layer_name": layer_name,
        "output_path": str(output_path),
        "size": len(data),
    }
