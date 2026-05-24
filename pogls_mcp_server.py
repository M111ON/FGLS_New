"""
pogls_mcp_server.py — POGLS Pipeline MCP Server

Exposes the POGLS pipeline (compress, encode, wallet, address) as MCP tools.
Use from opencode by adding `use pogls` to your prompt.

Tools:
  - pogls_compress:          binary → GPX4 (ZSTD + container)
  - pogls_decompress:        GPX4 → binary  
  - pogls_svg_encode:        binary → SVG vector
  - pogls_svg_decode:        SVG → binary
  - pogls_addr_encode:       uint64 → base62 address
  - pogls_addr_decode:       base62 → uint64 address
  - pogls_addr_decompose:    full addr breakdown (face, spoke, slot, ...)
  - pogls_visual_card:       single POGLS card as SVG
  - pogls_visual_grid:       grid of POGLS cards as SVG
  - pogls_compress_c1:       C1 PHI-pattern delta compression
  - pogls_compress_c1_decode: C1 decompression
  - pogls_compress_c2:       C2 seekable compression
  - pogls_wallet_build:      build .pogwallet from a file
  - pogls_wallet_info:       inspect .pogwallet metadata
  - pogls_xxh64:             compute xxh64 hash
  - pogls_pipeline:          full pipeline status / version info
"""

import os, sys, json, struct, base64, io, re

# ── paths ────────────────────────────────────────────────────────────
BASE = os.path.dirname(os.path.abspath(__file__))
PATHS = [
    os.path.join(BASE, "collection", "geopixel"),
    os.path.join(BASE, "core", "pogls_engine"),
    os.path.join(BASE, "core", "pogls_engine", "TPOGLS_s11", "TPOGLS_s11"),
    os.path.join(BASE, "collection", "geopixel", "wallet"),
    os.path.join(BASE, "collection", "core", "pogls_engine"),
    os.path.join(BASE, "collection", "core", "pogls_engine", "twin_core"),
    os.path.join(BASE, "core", "pogls_engine", "core"),
]
for p in PATHS:
    if p not in sys.path:
        sys.path.insert(0, p)

from mcp.server.fastmcp import FastMCP

mcp = FastMCP("POGLS Pipeline")

# ── import wrappers ──────────────────────────────────────────────────
CHUNK_ANIM = CHUNK2SVG = SVG2CHUNK = ADDR_CODEC = VISUAL = C1 = C2 = WALLET = None

def _import_modules():
    global CHUNK_ANIM, CHUNK2SVG, SVG2CHUNK, ADDR_CODEC, VISUAL, C1, C2, WALLET
    try:
        import chunk_anim
        CHUNK_ANIM = chunk_anim
    except Exception as e:
        CHUNK_ANIM = None
        _log(f"chunk_anim import failed: {e}")
    try:
        import chunk2svg
        CHUNK2SVG = chunk2svg
    except Exception as e:
        CHUNK2SVG = None
        _log(f"chunk2svg import failed: {e}")
    try:
        import svg2chunk
        SVG2CHUNK = svg2chunk
    except Exception as e:
        SVG2CHUNK = None
        _log(f"svg2chunk import failed: {e}")
    try:
        import pogls_addr_codec
        ADDR_CODEC = pogls_addr_codec
    except Exception as e:
        ADDR_CODEC = None
        _log(f"pogls_addr_codec import failed: {e}")
    try:
        import pogls_visual
        VISUAL = pogls_visual
    except Exception as e:
        VISUAL = None
        _log(f"pogls_visual import failed: {e}")
    try:
        from pogls_compress_c1 import C1Encoder, C1Decoder, encode_stream, decode_stream
        C1 = {"C1Encoder": C1Encoder, "C1Decoder": C1Decoder, "encode_stream": encode_stream, "decode_stream": decode_stream}
    except Exception as e:
        C1 = None
        _log(f"pogls_compress_c1 import failed: {e}")
    try:
        from pogls_compress_c2 import C2Encoder, C2Decoder
        C2 = {"C2Encoder": C2Encoder, "C2Decoder": C2Decoder}
    except Exception as e:
        C2 = None
        _log(f"pogls_compress_c2 import failed: {e}")
    try:
        from pogls_wallet_py import WalletBuilder, WalletReader, chunk_seed, chunk_checksum, verify_chunk
        WALLET = {
            "WalletBuilder": WalletBuilder,
            "WalletReader": WalletReader,
            "chunk_seed": chunk_seed,
            "chunk_checksum": chunk_checksum,
            "verify_chunk": verify_chunk,
        }
    except Exception as e:
        WALLET = None
        _log(f"pogls_wallet_py import failed: {e}")

def _log(msg):
    import datetime
    ts = datetime.datetime.now().strftime("%H:%M:%S")
    print(f"[{ts}] {msg}", file=sys.stderr, flush=True)

_import_modules()
_avail = {k: v for k, v in [
    ("chunk_anim", CHUNK_ANIM), ("chunk2svg", CHUNK2SVG), ("svg2chunk", SVG2CHUNK),
    ("addr_codec", ADDR_CODEC), ("visual", VISUAL), ("c1", C1), ("c2", C2),
    ("wallet", WALLET),
] if v is not None}
_log(f"modules loaded: {', '.join(_avail.keys())}")


# ═════════════════════════════════════════════════════════════════════
# Helpers
# ═════════════════════════════════════════════════════════════════════

def _read_file(path: str) -> bytes:
    p = os.path.abspath(path)
    if not os.path.isfile(p):
        raise FileNotFoundError(f"not found: {p}")
    with open(p, "rb") as f:
        return f.read()

def _write_file(path: str, data: bytes):
    p = os.path.abspath(path)
    os.makedirs(os.path.dirname(p) or ".", exist_ok=True)
    with open(p, "wb") as f:
        f.write(data)

def _b64(data: bytes) -> str:
    return base64.b64encode(data).decode()

def _from_b64(s: str) -> bytes:
    return base64.b64decode(s)

def _nice_size(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024: return f"{n:.1f}{unit}"
        n /= 1024
    return f"{n:.1f}TB"


# ═════════════════════════════════════════════════════════════════════
# Tools
# ═════════════════════════════════════════════════════════════════════

# ── 1. GPX4 Compress ────────────────────────────────────────────────
@mcp.tool()
def pogls_compress(path: str | None = None, data_b64: str | None = None,
                   out: str = "", zstd_level: int = 9) -> str:
    """Compress binary → GPX4 container (ZSTD + xxh64). Provide path or data_b64."""
    if path:
        raw = _read_file(path)
        abs_path = os.path.abspath(path)
        base_name = os.path.splitext(os.path.basename(path))[0]
        default_dir = os.path.dirname(abs_path)
    elif data_b64:
        raw = _from_b64(data_b64)
        base_name = "data"
        default_dir = "."
    else:
        return "err  provide path or data_b64"

    if CHUNK_ANIM is None:
        return "err  chunk_anim module not available"

    out_path = out or os.path.join(default_dir, f"{base_name}.gpx4")
    meta = CHUNK_ANIM.encode_to_gpx4(raw, out_path, zstd_level=zstd_level)
    sz = os.path.getsize(out_path)
    ratio = sz / len(raw) if len(raw) else 1
    return (f"ok  {os.path.basename(out_path)}  "
            f"orig={_nice_size(len(raw))}  gpx4={_nice_size(sz)}  "
            f"ratio={ratio:.2f}x  xxh64={meta['xxh64']}")


@mcp.tool()
def pogls_decompress(path: str, out: str = "") -> str:
    """Decompress GPX4 → original binary."""
    if CHUNK_ANIM is None:
        return "err  chunk_anim module not available"
    try:
        result = CHUNK_ANIM.decode_from_gpx4(path)
    except Exception as e1:
        try:
            result = CHUNK_ANIM._decode_animate(path)
        except Exception as e2:
            return f"err  decode failed: single={e1} anim={e2}"
    digest = CHUNK_ANIM.xxh64(result)
    out_path = out or (os.path.splitext(path)[0] + ".bin")
    _write_file(out_path, result)
    return f"ok  {os.path.basename(out_path)}  ({_nice_size(len(result))})  xxh64={digest:016X}"


# ── 3. SVG Wallet ────────────────────────────────────────────────────
@mcp.tool()
def pogls_svg_encode(path: str, mode: str = "bw", cols: int = 8, out: str = "") -> str:
    """Encode binary → SVG vector image. mode: bw (1 bit/cell) or gray (1 byte/cell)."""
    if CHUNK2SVG is None:
        return "err  chunk2svg module not available"
    raw = _read_file(path)
    mode_int = 0 if mode == "bw" else 1
    svg = CHUNK2SVG.encode_svg(raw, mode=mode_int, cols=cols)
    out_path = out or (os.path.splitext(os.path.basename(path))[0] + ".svg")
    _write_file(out_path, svg.encode("utf-8"))
    return f"ok  {os.path.basename(out_path)}  mode={mode}  cols={cols}  size={len(svg)}B  xxh64={CHUNK2SVG.xxh64(raw):016X}"


@mcp.tool()
def pogls_svg_decode(path: str, out: str = "") -> str:
    """Decode SVG → original binary (lossless)."""
    if SVG2CHUNK is None:
        return "err  svg2chunk module not available"
    raw = _read_file(path)
    result, meta = SVG2CHUNK.decode_svg(raw.decode("utf-8", errors="replace"))
    out_path = out or (os.path.splitext(os.path.basename(path))[0] + ".bin")
    _write_file(out_path, result)
    raw_hash = meta.get('xxh64')
    if isinstance(raw_hash, str):
        hash_str = raw_hash
    else:
        hash_str = f"{int(raw_hash):016X}" if raw_hash else "?"
    return f"ok  {os.path.basename(out_path)}  orig={meta.get('orig_size', len(result))}B  xxh64={hash_str}"


# ── 4. Address Codec ────────────────────────────────────────────────
@mcp.tool()
def pogls_addr_encode(addr: int = 0) -> str:
    """Encode uint64 address → base62 11-char string."""
    if ADDR_CODEC is None:
        return "err  addr_codec module not available"
    if addr < 0 or addr > 2**64 - 1:
        return f"err  addr out of range (0 .. 2^64-1): {addr}"
    encoded = ADDR_CODEC.encode(addr)
    bridge = ADDR_CODEC.to_bridge(addr)
    return f"ok  {addr} → {encoded}  (bridge: {bridge})"


@mcp.tool()
def pogls_addr_decode(encoded: str = "") -> str:
    """Decode base62 11-char string → uint64 address."""
    if ADDR_CODEC is None:
        return "err  addr_codec module not available"
    try:
        decoded = ADDR_CODEC.decode(encoded)
        bridge = ADDR_CODEC.to_bridge(decoded)
        return f"ok  {encoded} → {decoded}  (bridge: {bridge})"
    except Exception as e:
        return f"err  decode failed: {e}"


@mcp.tool()
def pogls_addr_decompose(addr: int = 0) -> str:
    """Decompose address → {face, spoke, slot, world, layer, intensity, bits}."""
    if VISUAL is None:
        return "err  pogls_visual module not available"
    try:
        d = VISUAL.decompose(addr)
        lines = [f"ok  addr={d.get('addr', addr)}"]
        for k in ("spoke", "slot", "world", "layer", "intensity", "face", "bits"):
            if k in d:
                lines.append(f"  {k}: {d[k]}")
        return "\n".join(lines)
    except Exception as e:
        return f"err  decompose failed: {e}"


# ── 5. Visual Cards ─────────────────────────────────────────────────
@mcp.tool()
def pogls_visual_card(addr: int = 0, out: str = "") -> str:
    """Render a single POGLS address as SVG card."""
    if VISUAL is None:
        return "err  pogls_visual module not available"
    try:
        svg = VISUAL.render_svg(addr)
        out_path = out or f"pogls_card_{addr}.svg"
        _write_file(out_path, svg.encode("utf-8"))
        return f"ok  {os.path.basename(out_path)}  ({len(svg)}B)"
    except Exception as e:
        return f"err  render failed: {e}"


@mcp.tool()
def pogls_visual_grid(addrs: list[int] = [], cols: int = 4, out: str = "") -> str:
    """Render multiple POGLS addresses as SVG card grid."""
    if VISUAL is None:
        return "err  pogls_visual module not available"
    if not addrs:
        return "err  provide addrs list"
    try:
        svg = VISUAL.render_multi_svg(addrs, cols=cols)
        out_path = out or f"pogls_grid_{len(addrs)}cards.svg"
        _write_file(out_path, svg.encode("utf-8"))
        return f"ok  {os.path.basename(out_path)}  ({len(addrs)} cards, {len(svg)}B)"
    except Exception as e:
        return f"err  render failed: {e}"


# ── 6. C1 Compression ───────────────────────────────────────────────
@mcp.tool()
def pogls_compress_c1(path: str | None = None, data_b64: str | None = None,
                      window_size: int = 256, out: str = "") -> str:
    """C1: PHI-pattern delta compression. Input → ZERO/PHI/DELTA/RAW frame stream."""
    if C1 is None:
        return "err  C1 module not available"
    if path:
        raw = _read_file(path)
        base_name = os.path.splitext(os.path.basename(path))[0]
        default_dir = os.path.dirname(os.path.abspath(path))
    elif data_b64:
        raw = _from_b64(data_b64)
        base_name = "data"
        default_dir = "."
    else:
        return "err  provide path or data_b64"
    try:
        enc = C1["C1Encoder"](window_size=window_size)
        chunks = [raw[i:i+64].ljust(64, b'\x00') for i in range(0, len(raw), 64)]
        result = enc.encode_chunks(chunks)
    except Exception as e:
        return f"err  C1 encode failed: {e}"
    out_path = out or os.path.join(default_dir, f"{base_name}.c1")
    _write_file(out_path, result)
    ratio = len(result) / len(raw) if len(raw) else 1
    return f"ok  {os.path.basename(out_path)}  orig={_nice_size(len(raw))}  c1={_nice_size(len(result))}  ratio={ratio:.2f}x"


@mcp.tool()
def pogls_compress_c1_decode(path: str, out: str = "") -> str:
    """Decode C1 compressed data → original."""
    if C1 is None:
        return "err  C1 module not available"
    raw = _read_file(path)
    try:
        result = C1["decode_stream"](raw)
    except Exception as e:
        return f"err  C1 decode failed: {e}"
    data = b"".join(result)
    out_path = out or (os.path.splitext(os.path.basename(path))[0] + ".bin")
    _write_file(out_path, data)
    return f"ok  {os.path.basename(out_path)}  ({_nice_size(len(data))})"


# ── 7. C2 Compression ───────────────────────────────────────────────
@mcp.tool()
def pogls_compress_c2(path: str | None = None, data_b64: str | None = None,
                      window_size: int = 256, out: str = "") -> str:
    """C2: Seekable compression (C1 + chunk index). Supports partial reconstruct."""
    if C2 is None:
        return "err  C2 module not available"
    if path:
        raw = _read_file(path)
        base_name = os.path.splitext(os.path.basename(path))[0]
        default_dir = os.path.dirname(os.path.abspath(path))
    elif data_b64:
        raw = _from_b64(data_b64)
        base_name = "data"
        default_dir = "."
    else:
        return "err  provide path or data_b64"
    try:
        enc = C2["C2Encoder"](window_size=window_size)
        result = enc.encode(raw)
    except Exception as e:
        return f"err  C2 encode failed: {e}"
    out_path = out or os.path.join(default_dir, f"{base_name}.c2")
    _write_file(out_path, result)
    ratio = len(result) / len(raw) if len(raw) else 1
    return f"ok  {os.path.basename(out_path)}  orig={_nice_size(len(raw))}  c2={_nice_size(len(result))}  ratio={ratio:.2f}x  partial-reconstruct=yes"


# ── 8. Wallet ────────────────────────────────────────────────────────
@mcp.tool()
def pogls_wallet_build(path: str, drift_window: int = 2, mode: str = "path", out: str = "") -> str:
    """Build a .pogwallet from a file. Scans file → coord records → sealed wallet blob."""
    if WALLET is None:
        return "err  wallet module not available"
    if not os.path.isfile(path):
        return f"err  file not found: {path}"
    try:
        mode_i = 0 if mode == "path" else 1
        st = os.stat(path)
        builder = WALLET["WalletBuilder"](mode=mode_i)
        file_idx = builder.add_file(path, int(st.st_size), int(st.st_mtime), 0)
        with open(path, "rb") as fh:
            offset = 0
            while True:
                raw_chunk = fh.read(64)
                if not raw_chunk: break
                chunk = raw_chunk.ljust(64, b'\x00')
                seed = WALLET["chunk_seed"](chunk)
                cs = WALLET["chunk_checksum"](chunk)
                builder.add_coord(file_idx, offset, seed, cs, 0, 0, 0, drift_window)
                offset += len(raw_chunk)
        blob = builder.seal()
    except Exception as e:
        import traceback; tb = traceback.format_exc()
        return f"err  wallet build failed: {e} | {tb[:300]}"
    base_name = os.path.splitext(os.path.basename(path))[0]
    out_path = out or f"{base_name}.pogwallet"
    _write_file(out_path, blob)
    return f"ok  {os.path.basename(out_path)}  file_idx={file_idx}  size={_nice_size(len(blob))}"


@mcp.tool()
def pogls_wallet_info(path: str) -> str:
    """Inspect a .pogwallet file — metadata, file info, coord stats."""
    if WALLET is None:
        return "err  wallet module not available"
    try:
        blob = _read_file(path)
        reader = WALLET["WalletReader"](blob, skip_verify=True)
        stats = reader.stats()
    except Exception as e:
        return f"err  wallet read failed: {e}"
    lines = [f"ok  {os.path.basename(path)}"]
    for k, v in stats.items():
        lines.append(f"  {k}: {v}")
    return "\n".join(lines)


# ── 9. xxh64 ────────────────────────────────────────────────────────
@mcp.tool()
def pogls_xxh64(path: str = "", data_b64: str = "") -> str:
    """Compute xxh64 hash of a file or base64 data."""
    hasher = CHUNK_ANIM or _fallback_xxh64()
    if hasher is None:
        return "err  no xxh64 implementation available"
    if path and os.path.isfile(path):
        raw = _read_file(path)
        digest = hasher.xxh64(raw)
        return f"xxh64({os.path.basename(path)}) = {digest:016X}  ({_nice_size(len(raw))})"
    elif data_b64:
        raw = _from_b64(data_b64)
        digest = hasher.xxh64(raw)
        return f"xxh64(data) = {digest:016X}  ({_nice_size(len(raw))})"
    return "err  provide path or data_b64"


def _fallback_xxh64():
    """Minimal xxh64 if chunk_anim not available."""
    class _XXH:
        _H1 = 0x9e3779b97f4a7c15
        _H2 = 0x6c62272e07bb0142
        _M64 = (1 << 64) - 1
        @staticmethod
        def xxh64(data):
            h = (_XXH._H1 ^ len(data)) & _XXH._M64
            import struct
            for i in range(0, len(data), 8):
                w = struct.unpack_from("<Q", data, i)[0] if i + 8 <= len(data) else 0
                if i + 8 > len(data):
                    tail = data[i:] + b"\x00" * (8 - len(data[i:]))
                    w = struct.unpack("<Q", tail)[0]
                h = (((h ^ ((w * _XXH._H1) & _XXH._M64)) & _XXH._M64))
                h = (((h << 27) | (h >> 37)) & _XXH._M64)
                h = (h * _XXH._H2 + 0x94d049bb133111eb) & _XXH._M64
            h ^= h >> 33; h &= _XXH._M64
            h *= _XXH._H1; h &= _XXH._M64
            h ^= h >> 29; h &= _XXH._M64
            h *= _XXH._H2; h &= _XXH._M64
            h ^= h >> 32; h &= _XXH._M64
            return h
    return _XXH


# ── 10. Pipeline Status ─────────────────────────────────────────────
@mcp.tool()
def pogls_pipeline() -> str:
    """Show POGLS pipeline status — available modules and version info."""
    lines = ["POGLS Pipeline Status"]
    lines.append(f"  base: {BASE}")
    modules = [
        ("chunk_anim (GPX4)", CHUNK_ANIM),
        ("chunk2svg (SVG enc)", CHUNK2SVG),
        ("svg2chunk (SVG dec)", SVG2CHUNK),
        ("addr_codec", ADDR_CODEC),
        ("pogls_visual", VISUAL),
        ("c1 compress", C1),
        ("c2 compress", C2),
        ("wallet", WALLET),
    ]
    for name, mod in modules:
        status = "OK" if mod else "MISSING"
        lines.append(f"  [{status:7s}] {name}")
    return "\n".join(lines)


# ── run ──────────────────────────────────────────────────────────────
if __name__ == "__main__":
    mcp.run()
