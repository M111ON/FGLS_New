"""Zlib codec — pure-Python compression + xxh64 integrity.

Same pattern as GPX4: compress → decompress with integrity check.
Swappable with real GPX4/C1/C2/diamond_shell via ChunkCodec interface.
"""
import zlib
import xxhash
from ..codec import ChunkCodec


class ZlibCodec(ChunkCodec):
    """Zlib compression + xxh64 integrity. Drop-in for GPX4."""

    @property
    def codec_id(self) -> str:
        return "zlib"

    def compress(self, chunk: bytes) -> bytes:
        compressed = zlib.compress(chunk, level=9)
        return compressed

    def decompress(self, data: bytes) -> bytes:
        return zlib.decompress(data)
