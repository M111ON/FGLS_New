"""Bermuda Pack container — codec-agnostic archive format.

Format (.bpack):
  [Header 32B]
    magic: "BMDA" (3B)
    version: uint8 = 1
    codec_id: 8B (null-padded)
    manifest_offset: uint32 (from file start)
    manifest_size: uint32
    reserved: 12B (zero)
  [Payload]
    compressed chunks (codec-specific, one per file)
  [Manifest]
    JSON: [{path, size, offset, xxh64}, ...]

No install needed. No codec-specific knowledge in container.
"""
import json
import struct
import xxhash
from pathlib import Path
from typing import Dict, List, Optional

from .codec import ChunkCodec

BMDA_MAGIC = b"BMDA"
BMDA_VERSION = 1
HEADER_SIZE = 33
HEADER_FMT = "<4sB8sII12s"  # magic, version, codec_id(8), manifest_off, manifest_sz, reserved(12)


class BermudaPack:
    """Bermuda Pack container: pack/unpack/list/cat with pluggable codec."""

    def __init__(self, codec: ChunkCodec):
        self.codec = codec

    # ─── Pack ────────────────────────────────────────────────────────
    def pack(self, files: Dict[str, bytes], out_path: Path) -> None:
        """Pack {relative_path: data} into a .bpack file.

        files: dict mapping relative path (str) to file content (bytes).
        """
        # Compress each file's content
        entries: List[dict] = []
        payload_parts: List[bytes] = []
        offset = HEADER_SIZE  # payload starts after header

        for rel_path in sorted(files.keys()):
            data = files[rel_path]
            compressed = self.codec.compress(data)
            h = xxhash.xxh64(data).hexdigest()
            entries.append({
                "path": rel_path,
                "size": len(data),
                "offset": offset,
                "xxh64": h,
            })
            payload_parts.append(compressed)
            offset += len(compressed)

        # Serialize manifest
        manifest_bytes = json.dumps(entries, separators=(",", ":")).encode()

        # Build header
        codec_id_bytes = self.codec.codec_id.encode().ljust(8, b"\x00")
        manifest_offset = HEADER_SIZE + sum(len(p) for p in payload_parts)
        header = struct.pack(
            HEADER_FMT,
            BMDA_MAGIC,
            BMDA_VERSION,
            codec_id_bytes,
            manifest_offset,
            len(manifest_bytes),
            b"\x00" * 12,
        )

        # Write file
        with open(out_path, "wb") as f:
            f.write(header)
            for part in payload_parts:
                f.write(part)
            f.write(manifest_bytes)

    # ─── Load ────────────────────────────────────────────────────────
    def _load_manifest(self, pack_path: Path) -> List[dict]:
        """Read manifest from .bpack file."""
        with open(pack_path, "rb") as f:
            f.seek(0)
            raw = f.read(HEADER_SIZE)
            (magic, ver, codec_id_b, manifest_off, manifest_sz, _res) = \
                struct.unpack(HEADER_FMT, raw)
            assert magic == BMDA_MAGIC, f"Bad magic: {magic!r}"
            assert ver == BMDA_VERSION, f"Unsupported version: {ver}"
            f.seek(manifest_off)
            manifest_raw = f.read(manifest_sz)
        return json.loads(manifest_raw)

    def _read_chunk(self, pack_path: Path, offset: int) -> bytes:
        """Read compressed chunk at offset (reads until next entry or EOF)."""
        with open(pack_path, "rb") as f:
            f.seek(offset)
            # Read all remaining payload (we'll trim by manifest)
            return f.read()

    # ─── List ────────────────────────────────────────────────────────
    def list_files(self, pack_path: Path) -> List[dict]:
        """List files in pack: [{path, size, xxh64}, ...]"""
        entries = self._load_manifest(pack_path)
        return [{"path": e["path"], "size": e["size"], "xxh64": e["xxh64"]} for e in entries]

    # ─── Unpack ──────────────────────────────────────────────────────
    def unpack(self, pack_path: Path, out_dir: Path, verify_only: bool = False) -> dict:
        """Extract all files from pack to out_dir.

        If verify_only=True, checks xxh64 without writing files.

        Returns: {path: xxh64} for verification.
        """
        entries = self._load_manifest(pack_path)
        out_dir.mkdir(parents=True, exist_ok=True)

        with open(pack_path, "rb") as f:
            f.seek(HEADER_SIZE)
            payload = f.read()

        results: Dict[str, str] = {}
        for i, entry in enumerate(entries):
            start = entry["offset"] - HEADER_SIZE
            if i + 1 < len(entries):
                end = entries[i + 1]["offset"] - HEADER_SIZE
            else:
                end = len(payload)
            compressed = payload[start:end]
            decompressed = self.codec.decompress(compressed)

            actual_hash = xxhash.xxh64(decompressed).hexdigest()
            ok = actual_hash == entry["xxh64"]
            if not ok:
                ok = False
                print(f"VERIFY FAIL {entry['path']}: expected {entry['xxh64']}, got {actual_hash}")

            results[entry["path"]] = actual_hash
            if not verify_only:
                file_path = out_dir / entry["path"]
                file_path.parent.mkdir(parents=True, exist_ok=True)
                file_path.write_bytes(decompressed)

        return results

    # ─── Cat ─────────────────────────────────────────────────────────
    def cat(self, pack_path: Path, file_path: str) -> bytes:
        """Read a single file from pack without extracting everything.

        file_path: relative path as shown by list_files().
        """
        entries = self._load_manifest(pack_path)

        # Find the entry
        target = None
        target_idx = None
        for i, e in enumerate(entries):
            if e["path"] == file_path:
                target = e
                target_idx = i
                break
        if target is None:
            raise FileNotFoundError(f"{file_path!r} not found in pack")

        # Read payload and extract just this chunk
        with open(pack_path, "rb") as f:
            f.seek(HEADER_SIZE)
            payload = f.read()

        start = target["offset"] - HEADER_SIZE
        if target_idx + 1 < len(entries):
            end = entries[target_idx + 1]["offset"] - HEADER_SIZE
        else:
            end = len(payload)
        compressed = payload[start:end]
        return self.codec.decompress(compressed)
