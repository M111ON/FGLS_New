"""GPX4 codec — ZSTD + xxh64 (pure Python, no external binary).

Format (self-contained):
  [4B magic 'GPX4']
  [4B uncompressed_size  (uint32 LE)]
  [8B xxh64 checksum     (uint64 LE)]
  [ZSTD compressed payload]

Decompress: check magic → decompress zstd → verify xxh64.
"""
import struct
import zstandard
import xxhash

from ..codec import ChunkCodec

GPX4_MAGIC = b"GPX4"
GPX4_HEADER_FMT = "<4sI8s"  # magic, uncompressed_size, xxh64
GPX4_HEADER_SZ = 16


class GPX4Codec(ChunkCodec):

    @property
    def codec_id(self) -> str:
        return "gpx4"

    def __init__(self, zstd_level: int = 9):
        self.zstd_level = zstd_level
        self._cctx = zstandard.ZstdCompressor(level=zstd_level)
        self._dctx = zstandard.ZstdDecompressor()

    def compress(self, chunk: bytes) -> bytes:
        compressed = self._cctx.compress(chunk)
        h = xxhash.xxh64(chunk).digest()
        header = struct.pack(
            GPX4_HEADER_FMT,
            GPX4_MAGIC,
            len(chunk),
            h,
        )
        return header + compressed

    def decompress(self, data: bytes) -> bytes:
        magic, uncomp_sz, stored_hash = struct.unpack(
            GPX4_HEADER_FMT, data[:GPX4_HEADER_SZ])
        assert magic == GPX4_MAGIC, f"Bad GPX4 magic: {magic!r}"
        payload = data[GPX4_HEADER_SZ:]
        decompressed = self._dctx.decompress(payload, max_output_size=uncomp_sz)

        actual_hash = xxhash.xxh64(decompressed).digest()
        assert actual_hash == stored_hash, \
            f"GPX4 xxh64 mismatch: expected {stored_hash!r}, got {actual_hash!r}"

        return decompressed