"""
global_exec_cache.py — O(1) execution reuse via (pattern, ent_bucket, state_hash).

Key = (pattern, entropy_bucket, state_hash)
Value = cached numpy ndarray (engine.execute output)

Hot path (hit >= HOT_THRESHOLD): eagerly cache on first miss
Cold path: lazy store on first execution
"""

import threading
from typing import Optional, Tuple

import numpy as np

HOT_THRESHOLD = 3


class GlobalExecCache:
    def __init__(self):
        self._lock = threading.Lock()
        self._cache: dict[Tuple[int, int, int], dict] = {}
        self._hits: dict[Tuple[int, int, int], int] = {}
        self._hot: set[Tuple[int, int, int]] = set()
        self._total_hits = 0
        self._total_misses = 0

    def _key(self, card, state_hash: int) -> Tuple[int, int, int]:
        pattern = card.pattern if hasattr(card, 'pattern') else card.get('pattern', 0)
        ent = card.entropy if hasattr(card, 'entropy') else card.get('entropy', 128)
        ent_bucket = ent // 16
        return (pattern, ent_bucket, state_hash)

    def is_hot(self, card, state_hash: int) -> bool:
        key = self._key(card, state_hash)
        with self._lock:
            return key in self._hot

    def lookup(self, card, state_hash: int) -> Optional[np.ndarray]:
        key = self._key(card, state_hash)
        with self._lock:
            if key in self._cache:
                self._hits[key] = self._hits.get(key, 0) + 1
                self._total_hits += 1
                if self._hits[key] >= HOT_THRESHOLD:
                    self._hot.add(key)
                return self._cache[key]["output"]
            self._total_misses += 1
            return None

    def store(self, card, state_hash: int, output: np.ndarray,
              metadata: dict = None):
        key = self._key(card, state_hash)
        with self._lock:
            self._cache[key] = {
                "output": output,
                "metadata": metadata or {},
                "shape": output.shape,
                "dtype": str(output.dtype),
            }
            if key not in self._hits:
                self._hits[key] = 0

    def ensure(self, card, state_hash: int, output: np.ndarray,
               metadata: dict = None):
        """Store only if not already cached (no-overwrite)."""
        key = self._key(card, state_hash)
        with self._lock:
            if key not in self._cache:
                self._cache[key] = {
                    "output": output,
                    "metadata": metadata or {},
                    "shape": output.shape,
                    "dtype": str(output.dtype),
                }
                if key not in self._hits:
                    self._hits[key] = 0

    def clear(self):
        with self._lock:
            self._cache.clear()
            self._hits.clear()
            self._hot.clear()
            self._total_hits = 0
            self._total_misses = 0

    def stats(self) -> dict:
        with self._lock:
            hottest = sorted(self._hits.items(), key=lambda x: -x[1])[:5]
            return {
                "size": len(self._cache),
                "total_hits": self._total_hits,
                "total_misses": self._total_misses,
                "hit_rate": round(
                    self._total_hits / max(1, self._total_hits + self._total_misses), 4
                ),
                "hot_keys": len(self._hot),
                "hottest": [
                    {"key": f"{k[0]:02x}/{k[1]:02x}/{k[2]:016x}", "hits": v}
                    for k, v in hottest
                ],
                "hot_threshold": HOT_THRESHOLD,
            }


_GLOBAL_CACHE = GlobalExecCache()


def get_exec_cache() -> GlobalExecCache:
    return _GLOBAL_CACHE


def reset_exec_cache():
    _GLOBAL_CACHE.clear()
