"""ChunkCodec interface — Bermuda Pack codec abstraction.

Every codec implements:
  compress(chunk: bytes) -> bytes
  decompress(data: bytes) -> bytes
  codec_id -> str

Bermuda Pack container never knows the implementation details.
"""
from abc import ABC, abstractmethod


class ChunkCodec(ABC):
    """Abstract base for Bermuda Pack codecs."""

    @property
    @abstractmethod
    def codec_id(self) -> str:
        """Unique codec identifier (e.g., 'gpx4', 'c1', 'c2', 'diamond')."""
        ...

    @abstractmethod
    def compress(self, chunk: bytes) -> bytes:
        """Compress a single chunk. Returns codec-specific bytes."""
        ...

    @abstractmethod
    def decompress(self, data: bytes) -> bytes:
        """Decompress a chunk back to original bytes."""
        ...

    def __repr__(self):
        return f"<{self.__class__.__name__} id={self.codec_id!r}>"
