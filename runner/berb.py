#!/usr/bin/env python3
"""Bermuda Pack single-file plugin — pack/unpack/list/cat.

Usage:
  berb pack    <src>... <dst.bpack>              # compress
  berb unpack  <src.bpack> <dst_dir>              # extract
  berb list    <src.bpack>                        # list files
  berb cat     <src.bpack> <path>                 # read one file
  berb verify  <src.bpack>                        # check integrity

Optional:
  --zstd           Use ZSTD+xxh64 instead of zlib (default: zlib)
  --level N        Compression level (default: 9)
"""
import argparse, json, os, struct, sys, xxhash, zlib
from pathlib import Path

try:
    import zstandard
    _HAS_ZSTD = True
except ImportError:
    _HAS_ZSTD = False

BMDA_MAGIC = b"BMDA"
BMDA_VER = 1
HDR_FMT = "<4sB8sII12s"
HDR_SZ = 33

GPX4_MAGIC = b"GPX4"
GPX4_HDR = "<4sI8s"
GPX4_SZ = 16


# ─── Codec ────────────────────────────────────────────────────────────
class _ZlibCodec:
    id = "zlib"
    def compress(self, d): return zlib.compress(d, 9)
    def decompress(self, d): return zlib.decompress(d)

class _GPX4Codec:
    id = "gpx4"
    def __init__(self, level=9):
        if not _HAS_ZSTD:
            sys.exit("zstandard not installed — use --zstd on a system with it, or omit for zlib")
        self._cctx = zstandard.ZstdCompressor(level=level)
        self._dctx = zstandard.ZstdDecompressor()
    def compress(self, d):
        c = self._cctx.compress(d)
        h = xxhash.xxh64(d).digest()
        return struct.pack(GPX4_HDR, GPX4_MAGIC, len(d), h) + c
    def decompress(self, data):
        m, usz, sh = struct.unpack(GPX4_HDR, data[:GPX4_SZ])
        assert m == GPX4_MAGIC
        d = self._dctx.decompress(data[GPX4_SZ:], max_output_size=usz)
        assert xxhash.xxh64(d).digest() == sh, "xxh64 mismatch"
        return d


# ─── Core ─────────────────────────────────────────────────────────────
def _resolve_codec(args):
    if args.zstd:
        return _GPX4Codec(level=getattr(args, 'level', 9))
    return _ZlibCodec if hasattr(args, 'level') else _ZlibCodec()

def _pack(args):
    codec = _GPX4Codec(level=args.level) if args.zstd else _ZlibCodec()
    files = {}
    for src in args.source:
        p = Path(src)
        if p.is_file():
            files[p.name] = p.read_bytes()
        elif p.is_dir():
            for f in sorted(p.rglob("*")):
                if f.is_file():
                    rel = str(f.relative_to(p.parent)).replace("\\", "/")
                    files[rel] = f.read_bytes()
    if not files:
        sys.exit("No files to pack")
    out = Path(args.output)
    entries, parts, off = [], [], HDR_SZ
    for name in sorted(files):
        c = codec.compress(files[name])
        entries.append({"path": name, "size": len(files[name]),
                        "offset": off, "xxh64": xxhash.xxh64(files[name]).hexdigest()})
        parts.append(c)
        off += len(c)
    codec_id = codec.id.encode().ljust(8, b"\x00")
    moff = HDR_SZ + sum(len(p) for p in parts)
    hdr = struct.pack(HDR_FMT, BMDA_MAGIC, BMDA_VER, codec_id, moff, 0, b"\x00"*12)
    man = json.dumps(entries, separators=(",", ":")).encode()
    hdr2 = struct.pack(HDR_FMT, BMDA_MAGIC, BMDA_VER, codec_id, moff, len(man), b"\x00"*12)
    with open(out, "wb") as f:
        f.write(hdr2)
        for p in parts: f.write(p)
        f.write(man)
    raw = sum(len(v) for v in files.values())
    print(f"Packed {len(files)} files → {out}  ({raw:,}B → {out.stat().st_size:,}B)")

def _load_manifest(path):
    with open(path, "rb") as f:
        raw = f.read(HDR_SZ)
        magic, ver, cid, moff, msz, _ = struct.unpack(HDR_FMT, raw)
        assert magic == BMDA_MAGIC, f"Bad magic: {magic!r}"
        f.seek(moff)
        return json.loads(f.read(msz))

def _make_codec_from_manifest(path):
    with open(path, "rb") as f:
        raw = f.read(HDR_SZ)
        _, _, cid, _, _, _ = struct.unpack(HDR_FMT, raw)
    codec_name = cid.rstrip(b"\x00").decode()
    if codec_name == "gpx4":
        return _GPX4Codec()
    return _ZlibCodec()

def _list(args):
    for e in _load_manifest(args.source):
        print(f"  {e['size']:>10,}  {e['xxh64']}  {e['path']}")
    print(f"\n{len(_load_manifest(args.source))} files")

def _cat(args):
    codec = _make_codec_from_path(args.source)
    entries = _load_manifest(args.source)
    target = None
    for i, e in enumerate(entries):
        if e["path"] == args.path: target = (e, i); break
    if not target: sys.exit(f"{args.path!r} not found")
    e, idx = target
    with open(args.source, "rb") as f:
        f.seek(HDR_SZ)
        payload = f.read()
    start = e["offset"] - HDR_SZ
    end = entries[idx+1]["offset"] - HDR_SZ if idx+1 < len(entries) else len(payload)
    sys.stdout.buffer.write(codec.decompress(payload[start:end]))

def _make_codec_from_path(path):
    with open(path, "rb") as f:
        raw = f.read(HDR_SZ)
        _, _, cid, _, _, _ = struct.unpack(HDR_FMT, raw)
    cname = cid.rstrip(b"\x00").decode()
    return _GPX4Codec() if cname == "gpx4" else _ZlibCodec()

def _unpack(args):
    codec = _make_codec_from_path(args.source)
    entries = _load_manifest(args.source)
    dst = Path(args.output_dir)
    dst.mkdir(parents=True, exist_ok=True)
    verify_only = args.verify_only
    with open(args.source, "rb") as f:
        f.seek(HDR_SZ)
        payload = f.read()
    for i, e in enumerate(entries):
        start = e["offset"] - HDR_SZ
        end = entries[i+1]["offset"] - HDR_SZ if i+1 < len(entries) else len(payload)
        d = codec.decompress(payload[start:end])
        h = xxhash.xxh64(d).hexdigest()
        ok = "OK" if h == e["xxh64"] else "FAIL"
        if ok == "FAIL":
            print(f"  VERIFY FAIL {e['path']}: expected {e['xxh64']}, got {h}")
        if not verify_only:
            fp = dst / e["path"]; fp.parent.mkdir(parents=True, exist_ok=True)
            fp.write_bytes(d)
        print(f"  {h}  {e['path']}  [{ok}]")
    print(f"\nVerified {len(entries)} files" if verify_only else f"\nUnpacked {len(entries)} files to {dst}")

def main():
    p = argparse.ArgumentParser(prog="berb", description="Bermuda Pack plugin")
    sub = p.add_subparsers(dest="cmd", required=True)

    pk = sub.add_parser("pack")
    pk.add_argument("source", nargs="+"); pk.add_argument("output")
    pk.add_argument("--zstd", action="store_true"); pk.add_argument("--level", type=int, default=9)
    pk.set_defaults(func=_pack)

    up = sub.add_parser("unpack")
    up.add_argument("source"); up.add_argument("output_dir")
    up.add_argument("--verify-only", action="store_true")
    up.set_defaults(func=_unpack)

    ls = sub.add_parser("list")
    ls.add_argument("source"); ls.set_defaults(func=_list)

    ct = sub.add_parser("cat")
    ct.add_argument("source"); ct.add_argument("path")
    ct.set_defaults(func=_cat)

    vf = sub.add_parser("verify")
    vf.add_argument("source")
    vf.set_defaults(func=lambda a: _unpack(type('',(),{'source':a.source,'output_dir':os.devnull,'verify_only':True})()))

    args = p.parse_args()
    args.func(args)

if __name__ == "__main__":
    main()