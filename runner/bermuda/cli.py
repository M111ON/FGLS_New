"""Bermuda Pack CLI — pack / unpack / list / cat

Usage:
  python -m bermuda pack <src>... <dst.bpack>     Pack files/dirs
  python -m bermuda unpack <src.bpack> <dst_dir>  Extract all
  python -m bermuda list <src.bpack>              List files
  python -m bermuda cat <src.bpack> <path>        Read single file
"""
import argparse
import sys
from pathlib import Path

from .codecs.zlib_codec import ZlibCodec
from .codecs.gpx4 import GPX4Codec
from .container import BermudaPack

_CODECS = {
    "zlib": ZlibCodec,
    "gpx4": GPX4Codec,
}

def _resolve_codec(name: str):
    cls = _CODECS.get(name)
    if cls is None:
        print(f"Unknown codec: {name!r}. Available: {', '.join(_CODECS)}", file=sys.stderr)
        sys.exit(1)
    return cls()


def _collect_files(sources: list) -> dict:
    """Collect files from sources (files or dirs) into {relative_path: bytes}."""
    files: dict = {}
    for src in sources:
        p = Path(src)
        if p.is_file():
            files[p.name] = p.read_bytes()
        elif p.is_dir():
            base = p.parent  # relative to parent, so dir name is prefix
            for f in sorted(p.rglob("*")):
                if f.is_file():
                    rel = str(f.relative_to(base)).replace("\\", "/")
                    files[rel] = f.read_bytes()
        else:
            print(f"Warning: {src} not found, skipping", file=sys.stderr)
    return files


def cmd_pack(args):
    sources = args.source
    out = Path(args.output)
    codec = _resolve_codec(args.codec)
    pack = BermudaPack(codec)

    files = _collect_files(sources)
    if not files:
        print("Error: no files to pack", file=sys.stderr)
        sys.exit(1)

    pack.pack(files, out)
    total_raw = sum(len(v) for v in files.values())
    out_size = out.stat().st_size
    ratio = total_raw / out_size if out_size > 0 else 0
    print(f"Packed {len(files)} files → {out}  ({total_raw:,}B → {out_size:,}B, {ratio:.1f}x)")


def cmd_unpack(args):
    src = Path(args.source)
    dst = Path(args.output_dir)
    codec = _resolve_codec(args.codec)
    pack = BermudaPack(codec)

    verify_only = args.verify_only if hasattr(args, 'verify_only') else False
    results = pack.unpack(src, dst, verify_only=verify_only)
    if verify_only:
        total = sum(1 for _ in results)
        print(f"Verified {total} files (no files written)")
    else:
        total = sum(1 for _ in results)
        print(f"Unpacked {total} files to {dst}")
    for path, h in results.items():
        print(f"  {path}  xxh64={h}")


def cmd_list(args):
    src = Path(args.source)
    pack = BermudaPack(ZlibCodec())  # codec not needed for list

    entries = pack.list_files(src)
    for e in entries:
        print(f"  {e['size']:>10,}  {e['xxh64']}  {e['path']}")
    print(f"\n{len(entries)} files")


def cmd_fingerprint(args):
    src = Path(args.source)
    codec = _resolve_codec(getattr(args, 'codec', 'zlib'))
    pack = BermudaPack(codec)
    out = Path(args.output) if args.output else src.with_suffix('.fingerprint.png')
    pack.fingerprint(src, out, args.size)
    print(f"Fingerprint → {out}  ({args.size}x{args.size})")


def cmd_cat(args):
    src = Path(args.source)
    codec = _resolve_codec(args.codec)
    pack = BermudaPack(codec)

    data = pack.cat(src, args.path)
    sys.stdout.buffer.write(data)


def main():
    parser = argparse.ArgumentParser(
        prog="bermuda",
        description="Bermuda Pack — POGLS-native work-tree packer",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_pack = sub.add_parser("pack", help="Pack files/dirs into .bpack")
    p_pack.add_argument("source", nargs="+", help="Files or dirs to pack")
    p_pack.add_argument("output", help="Output .bpack path")
    p_pack.add_argument("--codec", default="zlib", choices=list(_CODECS.keys()),
                        help="Compression codec (default: zlib)")
    p_pack.set_defaults(func=cmd_pack)

    p_unpack = sub.add_parser("unpack", help="Extract all files")
    p_unpack.add_argument("source", help="Input .bpack path")
    p_unpack.add_argument("output_dir", help="Output directory")
    p_unpack.add_argument("--codec", default="zlib", choices=list(_CODECS.keys()),
                          help="Codec used for pack (must match pack codec)")
    p_unpack.add_argument("--verify-only", action="store_true",
                          help="Check xxh64 without writing files")
    p_unpack.set_defaults(func=cmd_unpack)

    p_fp = sub.add_parser("fingerprint", help="Generate visual fingerprint PNG")
    p_fp.add_argument("source", help="Input .bpack path")
    p_fp.add_argument("--output", "-o", default=None, help="Output PNG path (default: {source}.fingerprint.png)")
    p_fp.add_argument("--size", type=int, default=512, help="Image size in pixels (default: 512)")
    p_fp.set_defaults(func=cmd_fingerprint)

    p_list = sub.add_parser("list", help="List files in pack")
    p_list.add_argument("source", help="Input .bpack path")
    p_list.set_defaults(func=cmd_list)

    p_cat = sub.add_parser("cat", help="Read single file from pack")
    p_cat.add_argument("source", help="Input .bpack path")
    p_cat.add_argument("path", help="File path inside pack")
    p_cat.add_argument("--codec", default="zlib", choices=list(_CODECS.keys()),
                       help="Codec used for pack (must match pack codec)")
    p_cat.set_defaults(func=cmd_cat)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
