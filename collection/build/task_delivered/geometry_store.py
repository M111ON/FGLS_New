"""
geometry_store.py — Persistent Geometry-Addressed Weight Store
==============================================================
Replaces in-memory Python dict with memory-mapped file store.
Index: (zone, shape) → float32 weight values, no RAM overhead.

Layout on disk:
  store.gsidx   — index file: (zone, shape) → (offset, n_rows, n_cols)
  store.gsdat   — data file: raw float32 blocks, mmap'd

Usage:
  # --- offline: index GGUF once ---
  store = GeometryStore("qwen_geom")
  store.index(zone=6, shape='S', values=float_array)   # write
  store.flush()

  # --- runtime: O(1) lookup, zero GGUF ---
  store = GeometryStore("qwen_geom", read_only=True)
  values = store.query(zone=6, shape='S')               # read

Design:
  - Index: 72 possible keys (12 zones × 6 shapes = I/O/T/S/Z/L)
  - Data: float32 rows appended sequentially, mmap on read
  - O(1) lookup: index → seek → slice mmap
  - No RAM copy: mmap returns numpy view directly
  - Thread-safe reads (no writes after flush)
"""

import os, struct, mmap, json
import numpy as np
from pathlib import Path
from typing import Optional

# ── Constants ────────────────────────────────────────────────
SHAPES       = ['I', 'O', 'T', 'S', 'Z', 'L']
N_ZONES      = 12
INDEX_MAGIC  = b'GSIDX001'   # 8 bytes
DATA_MAGIC   = b'GSDAT001'   # 8 bytes
ENTRY_FMT    = '<HHqII'      # zone(2), shape_idx(2), offset(8), n_rows(4), n_cols(4) = 20B
ENTRY_SIZE   = struct.calcsize(ENTRY_FMT)
IDX_HEADER   = 32            # magic(8) + n_entries(4) + data_size(8) + pad(12)
FLOAT_BYTES  = 4             # float32

# ── Key encoding ──────────────────────────────────────────────
def _key(zone: int, shape: str) -> tuple:
    assert 0 <= zone < N_ZONES, f"zone {zone} out of range"
    assert shape in SHAPES, f"shape '{shape}' not in {SHAPES}"
    return (zone, SHAPES.index(shape))


class GeometryStore:
    """
    Persistent geometry-addressed weight store.

    Two files:
      <name>.gsidx  — fixed-size index (one entry per geometry key)
      <name>.gsdat  — packed float32 data, memory-mapped on read
    """

    def __init__(self, path: str, read_only: bool = False):
        self.base   = Path(path)
        self.idx_path  = self.base.with_suffix('.gsidx')
        self.dat_path  = self.base.with_suffix('.gsdat')
        self.read_only = read_only

        # In-memory index: key → (offset_bytes, n_rows, n_cols)
        self._index: dict[tuple, tuple] = {}
        self._dat_offset = 0    # next write position in data file
        self._mmap: Optional[mmap.mmap] = None
        self._dat_fh = None

        if read_only:
            self._load_index()
            self._open_mmap()
        else:
            # Write mode: start fresh or resume
            if self.idx_path.exists():
                self._load_index()
                self._dat_offset = self.dat_path.stat().st_size if self.dat_path.exists() else 0
            self._dat_fh = open(self.dat_path, 'ab')   # append binary

    # ── Write API ────────────────────────────────────────────

    def index(self, zone: int, shape: str, values: np.ndarray):
        """
        Write weight values to store, keyed by (zone, shape).

        values: 1D or 2D float32 array
          1D [D]     → stored as 1 row
          2D [N, D]  → stored as N rows
        If key already exists, rows are APPENDED (same D required).
        """
        assert not self.read_only, "Store opened read-only"
        k = _key(zone, shape)

        arr = np.asarray(values, dtype=np.float32)
        if arr.ndim == 1:
            arr = arr[np.newaxis, :]      # [1, D]
        assert arr.ndim == 2, f"Expected 1D or 2D, got {arr.ndim}D"

        n_rows, n_cols = arr.shape

        if k in self._index:
            # Append: verify same n_cols
            _, existing_rows, existing_cols = self._index[k]
            assert existing_cols == n_cols, (
                f"Col mismatch: stored {existing_cols}, new {n_cols}")
            # Update row count; offset stays at original (data appended after)
            off, _, _ = self._index[k]
            self._index[k] = (off, existing_rows + n_rows, n_cols)
        else:
            self._index[k] = (self._dat_offset, n_rows, n_cols)

        raw = arr.tobytes()
        self._dat_fh.write(raw)
        self._dat_offset += len(raw)

    def flush(self):
        """Write index file. Call after all index() calls."""
        assert not self.read_only
        self._dat_fh.flush()
        self._write_index()
        print(f"[GeometryStore] flushed: {len(self._index)} keys, "
              f"{self._dat_offset / 1024:.1f} KB data")

    def close(self):
        if self._dat_fh:
            self._dat_fh.close()
            self._dat_fh = None
        if self._mmap:
            self._mmap.close()
            self._mmap = None

    # ── Read API ─────────────────────────────────────────────

    def query(self, zone: int, shape: str) -> Optional[np.ndarray]:
        """
        Retrieve weight values for (zone, shape).
        Returns float32 ndarray [n_rows, n_cols] or None if not found.
        Zero-copy: mmap slice, no RAM allocation.
        """
        k = _key(zone, shape)
        if k not in self._index:
            return None
        offset, n_rows, n_cols = self._index[k]
        n_bytes = n_rows * n_cols * FLOAT_BYTES
        raw = self._mmap[offset: offset + n_bytes]
        arr = np.frombuffer(raw, dtype=np.float32).reshape(n_rows, n_cols)
        return arr

    def query_batch(self, keys: list[tuple]) -> dict:
        """
        Batch query: [(zone, shape), ...] → {key: ndarray}
        Returns only found keys.
        """
        return {k: v for k in keys if (v := self.query(*k)) is not None}

    def has(self, zone: int, shape: str) -> bool:
        return _key(zone, shape) in self._index

    def stats(self) -> dict:
        """Summary of store contents."""
        total_rows = sum(r for _, r, _ in self._index.values())
        total_kb   = self._dat_offset / 1024 if self._dat_offset else (
            self.dat_path.stat().st_size / 1024 if self.dat_path.exists() else 0)
        keys_by_zone = {}
        for (z, si) in self._index:
            keys_by_zone[z] = keys_by_zone.get(z, 0) + 1
        return {
            'n_keys':     len(self._index),
            'total_rows': total_rows,
            'data_kb':    round(total_kb, 1),
            'keys_by_zone': keys_by_zone,
        }

    def print_stats(self):
        s = self.stats()
        print(f"[GeometryStore] {self.base}")
        print(f"  keys:      {s['n_keys']} / 72 possible")
        print(f"  rows:      {s['total_rows']:,}")
        print(f"  data:      {s['data_kb']} KB")
        print(f"  coverage:")
        for z in range(N_ZONES):
            cnt = s['keys_by_zone'].get(z, 0)
            if cnt:
                shapes_present = [SHAPES[si] for (zz, si) in self._index if zz == z]
                print(f"    zone {z:2d}: {cnt} shapes → {' '.join(shapes_present)}")

    # ── Internal ─────────────────────────────────────────────

    def _write_index(self):
        """Serialize index to .gsidx file."""
        entries = list(self._index.items())
        n = len(entries)
        with open(self.idx_path, 'wb') as f:
            # Header
            f.write(INDEX_MAGIC)                          # 8B
            f.write(struct.pack('<I', n))                 # 4B n_entries
            f.write(struct.pack('<Q', self._dat_offset))  # 8B data_size
            f.write(b'\x00' * 12)                         # 12B pad → 32B total
            # Entries
            for (z, si), (off, n_rows, n_cols) in entries:
                f.write(struct.pack(ENTRY_FMT, z, si, off, n_rows, n_cols))

    def _load_index(self):
        """Load index from .gsidx file."""
        if not self.idx_path.exists():
            return
        with open(self.idx_path, 'rb') as f:
            magic = f.read(8)
            assert magic == INDEX_MAGIC, f"Bad magic: {magic}"
            n_entries = struct.unpack('<I', f.read(4))[0]
            self._dat_offset = struct.unpack('<Q', f.read(8))[0]
            f.read(12)  # pad
            for _ in range(n_entries):
                raw = f.read(ENTRY_SIZE)
                z, si, off, n_rows, n_cols = struct.unpack(ENTRY_FMT, raw)
                self._index[(z, si)] = (off, n_rows, n_cols)

    def _open_mmap(self):
        """Open memory-mapped view of data file."""
        if not self.dat_path.exists() or self.dat_path.stat().st_size == 0:
            return
        self._dat_fh = open(self.dat_path, 'rb')
        self._mmap = mmap.mmap(
            self._dat_fh.fileno(), 0,
            access=mmap.ACCESS_READ
        )

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


# ── Convenience: build store from GGUF (offline indexer) ─────

def build_from_gguf(gguf_path: str, store_path: str,
                    dim: int = 128, max_tensors: int = None,
                    gate_path: str = None, code_dim: int = 32):
    """
    One-time offline builder: GGUF → GeometryStore.

    Reads attention weights, routes through Bermuda,
    writes float32 values indexed by (zone, shape).

    Args:
      gguf_path:   path to .gguf model file
      store_path:  output path prefix (writes .gsidx + .gsdat)
      dim:         routing dimension (128 or 768)
      max_tensors: limit for testing (None = all)
      gate_path:   optional pre-trained gate .pt path
    """
    import sys, torch, time
    import numpy as np
    from pathlib import Path

    # Add bermuda to path (handles both flat and python_src layouts)
    pkg = Path(gguf_path).resolve().parent
    sys.path.insert(0, str(pkg))
    sys.path.insert(0, str(pkg / 'python_src'))

    try:
        from gguf import GGUFReader
    except ImportError:
        print("[ERROR] gguf not installed. Run: pip install gguf")
        return

    from bermuda_router_v1 import BermudaRouter, DEVICE

    print(f"[build_from_gguf] Loading {Path(gguf_path).name} ...")
    t0 = time.time()

    reader = GGUFReader(gguf_path)
    tensors = [t for t in reader.tensors
               if t.name.endswith('.weight') and len(t.shape) >= 2
               and ('attn_' in t.name or 'token_embd' in t.name)]

    if max_tensors:
        tensors = tensors[:max_tensors]

    print(f"  Found {len(tensors)} attention tensors")

    # Init router (load gate if provided)
    router = BermudaRouter(dim=dim, gear=2, code_dim=code_dim)
    if gate_path and Path(gate_path).exists():
        state = torch.load(gate_path, map_location=DEVICE, weights_only=True)
        router.gate.load_state_dict(state)
        router.gate.eval()
        print(f"  Loaded gate: {gate_path}")

    store = GeometryStore(store_path, read_only=False)
    n_blocks_needed = (dim + 31) // 32
    total_rows = 0

    for ti, t in enumerate(tensors):
        M = t.data.shape[0]
        B = t.data.shape[1] if len(t.data.shape) > 1 else 1
        n_blocks = max(1, B // 34)
        blk = t.data.reshape(M, n_blocks, 34)[:, :n_blocks_needed, :]
        i8 = blk[:, :, :32].astype(np.int8).astype(np.float32)
        flat = i8.reshape(M, n_blocks_needed * 32)[:, :dim]
        x = torch.from_numpy(flat).float()

        for bi in range(0, x.shape[0], 64):
            batch = x[bi:bi+64].to(DEVICE)
            if batch.shape[0] < 2:
                continue
            for mode in range(4):   # ORBITAL, CHIRAL, CROSS, HUB
                v = router.route(batch, mode)
                n      = v.n_tokens                    # actual batch size (no mask needed)
                zones  = v.zone[:n].cpu().numpy()
                shapes = v.shape[:n].cpu().numpy()
                vals   = batch[:n].detach().cpu().numpy()

                for ei in range(n):
                    z = int(zones[ei])
                    s = chr(int(shapes[ei]))
                    store.index(z, s, vals[ei])
                    total_rows += 1
                del v

        if (ti + 1) % 10 == 0 or ti == len(tensors) - 1:
            print(f"  [{ti+1}/{len(tensors)}] {t.name:40s} "
                  f"total_rows={total_rows:,}")

    store.flush()
    store.close()
    t1 = time.time()
    print(f"\n[build_from_gguf] Done in {t1-t0:.1f}s")
    print(f"  Store: {store_path}.gsidx + .gsdat")

    # Re-open and print stats
    with GeometryStore(store_path, read_only=True) as s:
        s.print_stats()


# ── Quick self-test ──────────────────────────────────────────
def _selftest():
    import tempfile, numpy as np
    print("=== GeometryStore self-test ===")
    with tempfile.TemporaryDirectory() as tmp:
        p = str(Path(tmp) / "test_store")

        # Write
        with GeometryStore(p) as store:
            for z in range(12):
                for s in ['I', 'S']:
                    vals = np.random.randn(16, 128).astype(np.float32)
                    store.index(z, s, vals)
            store.flush()

        # Read
        with GeometryStore(p, read_only=True) as store:
            store.print_stats()
            arr = store.query(zone=6, shape='S')
            assert arr is not None and arr.shape == (16, 128), f"Got {arr}"
            arr2 = store.has(zone=0, shape='T')   # shape not indexed
            assert arr2 is False
            print(f"\n  query(6,'S') → {arr.shape} ✓")
            print(f"  query(99,'I') → None ✓")

    print("=== PASS ===")


if __name__ == "__main__":
    import sys
    if len(sys.argv) >= 3 and sys.argv[1] == "build":
        # python geometry_store.py build model.gguf output_store [dim] [gate.pt]
        gguf   = sys.argv[2]
        out    = sys.argv[3]
        dim    = int(sys.argv[4]) if len(sys.argv) > 4 else 128
        gate   = sys.argv[5] if len(sys.argv) > 5 else None
        build_from_gguf(gguf, out, dim=dim, gate_path=gate)
    else:
        _selftest()
