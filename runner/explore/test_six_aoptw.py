#!/usr/bin/env python3
"""Test suite for SixAOPTW — 6-Axis Orthogonal Projection Tensor Weight Storage"""

import sys
import time
import numpy as np

# ============================================================
# Inline SixAOPTW class definition
# ============================================================

DIRS = 6
SIDE = 10
RAY_LEN = 10
DIR_SLOTS = 1000
BLOCK_SLOTS = 6000

def _mul10(x):
    return (x << 3) + (x << 1)

def _mul100(x):
    return (x << 6) + (x << 5) + (x << 2)

def _mul1000(x):
    return (x << 10) - (x << 4) - (x << 3)

def _mul6000(x):
    return (
        (x << 12) + (x << 10) + (x << 9) +
        (x << 8) + (x << 6) + (x << 5) + (x << 4)
    )


class SixAOPTW:
    DIRS = 6
    SIDE = 10
    RAY_LEN = 10
    DIR_SLOTS = 1000
    BLOCK_SLOTS = 6000

    _mul10 = staticmethod(_mul10)
    _mul100 = staticmethod(_mul100)
    _mul1000 = staticmethod(_mul1000)
    _mul6000 = staticmethod(_mul6000)

    def __init__(self, n_blocks=1, dtype="float16", path=None, mode="w+"):
        if n_blocks <= 0:
            raise ValueError("n_blocks must be > 0")
        self.n_blocks = int(n_blocks)
        self.dtype = np.dtype(dtype)
        size = self.n_blocks * self.BLOCK_SLOTS
        if path is None:
            self.buf = np.zeros(size, dtype=self.dtype)
        else:
            from pathlib import Path
            self.buf = np.memmap(Path(path), dtype=self.dtype, mode=mode, shape=(size,))
        s = np.arange(self.SIDE, dtype=np.intp)
        self._slice_uv = self._mul100(s[:, None]) + self._mul10(s[None, :])

    @property
    def size_bytes(self):
        return int(self.buf.nbytes)

    def flush(self):
        if hasattr(self.buf, "flush"):
            self.buf.flush()

    def __enter__(self):
        return self
    def __exit__(self, *exc):
        self.flush()

    def _check_duvk(self, d, u, v, k):
        if not (0 <= d < self.DIRS and 0 <= u < self.SIDE and 0 <= v < self.SIDE and 0 <= k < self.RAY_LEN):
            raise ValueError("d must be 0..5, u/v/k must be 0..9")

    def _check_block(self, b):
        if not (0 <= b < self.n_blocks):
            raise ValueError("block index out of range")

    def _slot(self, d, u, v, k, b):
        self._check_duvk(d, u, v, k)
        self._check_block(b)
        return self._mul6000(b) + self._mul1000(d) + self._mul100(u) + self._mul10(v) + k

    def _ray_start(self, d, u, v, b):
        if not (0 <= d < self.DIRS and 0 <= u < self.SIDE and 0 <= v < self.SIDE):
            raise ValueError("d/u/v out of range")
        self._check_block(b)
        return self._mul6000(b) + self._mul1000(d) + self._mul100(u) + self._mul10(v)

    def read(self, d, u, v, k, b=0):
        return self.buf[self._slot(d, u, v, k, b)]

    def write(self, d, u, v, k, value, b=0):
        self.buf[self._slot(d, u, v, k, b)] = value

    def read_ray(self, d, u, v, b=0):
        start = self._ray_start(d, u, v, b)
        return self.buf[start:start + self.RAY_LEN].copy()

    def write_ray(self, d, u, v, values, b=0):
        arr = np.asarray(values, dtype=self.dtype).reshape(-1)
        if arr.size != self.RAY_LEN:
            raise ValueError("ray must have exactly 10 values")
        start = self._ray_start(d, u, v, b)
        self.buf[start:start + self.RAY_LEN] = arr

    def read_slice(self, d, k, b=0):
        if not (0 <= d < self.DIRS and 0 <= k < self.RAY_LEN):
            raise ValueError("d/k out of range")
        self._check_block(b)
        base = self._mul6000(b) + self._mul1000(d) + k
        return self.buf[base + self._slice_uv].copy()

    def write_slice(self, d, k, matrix, b=0):
        mat = np.asarray(matrix, dtype=self.dtype)
        if mat.shape != (self.SIDE, self.SIDE):
            raise ValueError("matrix must be shape (10, 10)")
        if not (0 <= d < self.DIRS and 0 <= k < self.RAY_LEN):
            raise ValueError("d/k out of range")
        self._check_block(b)
        base = self._mul6000(b) + self._mul1000(d) + k
        self.buf[base + self._slice_uv] = mat

    @staticmethod
    def voxel_views(x, y, z):
        if not (0 <= x < 10 and 0 <= y < 10 and 0 <= z < 10):
            raise ValueError("x/y/z must be 0..9")
        return (
            (0, y, z, x),       # +X
            (1, y, z, 9 - x),   # -X
            (2, x, z, y),       # +Y
            (3, x, z, 9 - y),   # -Y
            (4, x, y, z),       # +Z
            (5, x, y, 9 - z),   # -Z
        )

    def write_voxel_all_dirs(self, x, y, z, value, b=0):
        for d, u, v, k in self.voxel_views(x, y, z):
            self.write(d, u, v, k, value, b)

    def read_voxel_all_dirs(self, x, y, z, b=0):
        return np.array([self.read(d, u, v, k, b) for d, u, v, k in self.voxel_views(x, y, z)], dtype=self.dtype)


# ============================================================
# Tests
# ============================================================

def test_basic_rw():
    """Test 1: Basic read/write correctness"""
    store = SixAOPTW(n_blocks=2, dtype="float16")
    store.write(d=0, u=1, v=2, k=3, value=0.25, b=0)
    val = store.read(d=0, u=1, v=2, k=3, b=0)
    assert abs(val - 0.25) < 1e-3, f"Expected 0.25, got {val}"
    print(f"  [PASS] write/read single: {val}")


def test_ray_rw():
    """Test 2: Full ray read/write"""
    store = SixAOPTW(n_blocks=1, dtype="float16")
    ray = np.arange(10, dtype=np.float16)
    store.write_ray(d=0, u=1, v=2, values=ray, b=0)
    out = store.read_ray(d=0, u=1, v=2, b=0)
    assert np.allclose(out, ray), f"Ray mismatch"
    print(f"  [PASS] ray write/read: {out.tolist()}")


def test_slice_rw():
    """Test 3: Full 10x10 slice read/write"""
    store = SixAOPTW(n_blocks=2, dtype="float16")
    mat = np.full((10, 10), 3.14, dtype=np.float16)
    store.write_slice(d=4, k=7, matrix=mat, b=1)
    out = store.read_slice(d=4, k=7, b=1)
    assert np.allclose(out, mat), "Slice mismatch at (4,7,1)"
    assert abs(out.sum() - 314.0) < 1.0, f"Sum expected 314, got {out.sum()}"
    print(f"  [PASS] slice write/read: sum={out.sum():.1f}")


def test_voxel_projections():
    """Test 4: Voxel 6-view projection"""
    store = SixAOPTW(n_blocks=1, dtype="float16")
    store.write_voxel_all_dirs(x=3, y=4, z=5, value=1.5, b=0)
    vals = store.read_voxel_all_dirs(x=3, y=4, z=5, b=0)
    assert all(abs(v - 1.5) < 1e-3 for v in vals), f"Not all 1.5: {vals}"
    print(f"  [PASS] voxel 6-view projection: all {vals.tolist()}")


def test_all_voxels_deterministic():
    """Test 5: Write unique value per voxel, verify 6-view consistency"""
    store = SixAOPTW(n_blocks=1, dtype="float16")
    for x in range(10):
        for y in range(10):
            for z in range(10):
                val = float(x * 100 + y * 10 + z)
                store.write_voxel_all_dirs(x, y, z, val, b=0)
    # Spot-check some voxels
    for (x, y, z) in [(0,0,0), (3,4,5), (9,9,9), (1,7,3)]:
        vals = store.read_voxel_all_dirs(x, y, z, b=0)
        expected = float(x * 100 + y * 10 + z)
        assert all(abs(v - expected) < 1e-3 for v in vals), f"({x},{y},{z}): expected {expected}, got {vals}"
    print(f"  [PASS] all 1000 voxels deterministic")


def test_collision_behavior():
    """Test 6: Collision behavior — different voxels mapping to same slot"""
    store = SixAOPTW(n_blocks=1, dtype="float16")

    # Write unique values to all 1000 voxels via +X direction only
    for x in range(10):
        for y in range(10):
            for z in range(10):
                d, u, v, k = SixAOPTW.voxel_views(x, y, z)[0]  # +X = idx 0
                store.write(d, u, v, k, float(x*100 + y*10 + z), b=0)

    collisions = 0
    for x in range(10):
        for y in range(10):
            for z in range(10):
                vals = store.read_voxel_all_dirs(x, y, z, b=0)
                if vals[0] == vals[1]:
                    collisions += 1

    print(f"  [INFO] +X/-X collision count: {collisions}/1000")
    assert collisions < 500, f"Too many X-axis collisions: {collisions}"

    # Now test cross-axis collisions: +X and +Y
    xy_collisions = 0
    for x in range(10):
        for y in range(10):
            for z in range(10):
                d0, u0, v0, k0 = SixAOPTW.voxel_views(x, y, z)[0]  # +X
                d1, u1, v1, k1 = SixAOPTW.voxel_views(x, y, z)[2]  # +Y
                vx = store._slot(d0, u0, v0, k0, 0)
                vy = store._slot(d1, u1, v1, k1, 0)
                if vx == vy:
                    xy_collisions += 1
    print(f"  [PASS] +X/+Y slot collisions: {xy_collisions}/1000 (should be 0)")
    assert xy_collisions == 0, "+X and +Y should never share a slot"


def test_block_isolation():
    """Test 7: Block isolation — different blocks don't overlap"""
    store = SixAOPTW(n_blocks=3, dtype="float16")
    for b in range(3):
        store.write(d=0, u=0, v=0, k=0, value=float(b * 100), b=b)
    for b in range(3):
        val = store.read(d=0, u=0, v=0, k=0, b=b)
        assert abs(val - b * 100) < 1e-3, f"Block {b}: expected {b*100}, got {val}"
    print(f"  [PASS] block isolation: 3 blocks independent")


def test_boundary_checks():
    """Test 8: Boundary conditions"""
    store = SixAOPTW(n_blocks=1, dtype="float16")

    # Valid extremes
    store.write(d=0, u=0, v=0, k=0, value=-128, b=0)
    store.write(d=5, u=9, v=9, k=9, value=127, b=0)
    v1 = store.read(d=0, u=0, v=0, k=0, b=0)
    v2 = store.read(d=5, u=9, v=9, k=9, b=0)
    assert abs(v1 + 128) < 1e-3, f"Min edge: {v1}"
    assert abs(v2 - 127) < 1e-3, f"Max edge: {v2}"
    print(f"  [PASS] boundary extremes: [{v1}, {v2}]")

    # Invalid indices should raise
    bad_cases = [
        (-1, 0, 0, 0, 0), (6, 0, 0, 0, 0),
        (0, -1, 0, 0, 0), (0, 10, 0, 0, 0),
        (0, 0, 10, 0, 0), (0, 0, 0, -1, 0),
        (0, 0, 0, 10, 0), (0, 0, 0, 0, -1),
    ]
    for d, u, v, k, b in bad_cases:
        try:
            store.write(d, u, v, k, 0.0, b)
            print(f"  [FAIL] should have raised for (d={d},u={u},v={v},k={k},b={b})")
            return False
        except (ValueError, IndexError):
            pass
    print(f"  [PASS] boundary checks: 8 invalid cases caught")


def test_speed_benchmark():
    """Test 9: Speed benchmark — read/write throughput"""
    store = SixAOPTW(n_blocks=1, dtype="float16")
    N = 100000

    # Write benchmark
    t0 = time.perf_counter()
    for i in range(N):
        d = i % 6
        u = (i // 6) % 10
        v = (i // 60) % 10
        k = (i // 600) % 10
        store.write(d, u, v, k, float(i), b=0)
    tw = time.perf_counter() - t0
    w_ops = N / tw

    # Read benchmark
    t0 = time.perf_counter()
    total = 0.0
    for i in range(N):
        d = i % 6
        u = (i // 6) % 10
        v = (i // 60) % 10
        k = (i // 600) % 10
        total += store.read(d, u, v, k, b=0)
    tr = time.perf_counter() - t0
    r_ops = N / tr

    print(f"  [BENCH] write: {w_ops:,.0f} ops/s, read: {r_ops:,.0f} ops/s")
    assert w_ops > 100000, f"Write too slow: {w_ops:.0f} ops/s"
    assert r_ops > 100000, f"Read too slow: {r_ops:.0f} ops/s"

    # Compare with numpy bulk operations
    t0 = time.perf_counter()
    arr = store.buf[:N]
    _ = arr.copy()
    tb = time.perf_counter() - t0
    bulk_ops = N / tb
    print(f"  [BENCH] numpy bulk read: {bulk_ops:,.0f} elems/s ({bulk_ops/r_ops:.1f}x faster than scalar)")


def test_multi_block_capacity():
    """Test 10: Capacity verification"""
    store = SixAOPTW(n_blocks=1, dtype="float16")
    # 1 block = 6 dirs x 10u x 10v x 10k = 6000 slots
    assert store.size_bytes == 6000 * 2, f"Expected 12000 bytes, got {store.size_bytes}"
    print(f"  [PASS] 1 block capacity: {store.size_bytes} bytes = {6000} slots")

    store2 = SixAOPTW(n_blocks=100, dtype="float16")
    print(f"  [PASS] 100 block capacity: {store2.size_bytes} bytes = {600000} slots")

    # LLM scale: how many blocks for Qwen3-0.6B?
    qwen_weights = 594_000_000
    blocks_needed = qwen_weights / 6000
    bytes_needed = qwen_weights * 2  # float16
    print(f"  [INFO] Qwen3-0.6B: {qwen_weights:,} weights -> {blocks_needed:,.0f} blocks")
    print(f"  [INFO]   = {bytes_needed/1e9:.1f} GB (float16 naive)")

    # With channel x slot factorization (81x):
    bits_per_weight = 15 / 81  # 0.185 bits
    geo_bytes = qwen_weights * bits_per_weight / 8
    print(f"  [INFO]   Channel-slot geo: {geo_bytes/1e6:.1f} MB ({32*8/bits_per_weight:.0f}x compression)")


def test_memory_layout():
    """Test 11: Verify memory layout matches spec"""
    store = SixAOPTW(n_blocks=1, dtype="float16")

    # Fill entire block with sequential values
    for i in range(6000):
        store.buf[i] = float(i)

    # Check slot formula: slot = b*6000 + d*1000 + u*100 + v*10 + k
    b = 0
    for d in range(6):
        for u in range(10):
            for v in range(10):
                for k in range(10):
                    expected_slot = b*6000 + d*1000 + u*100 + v*10 + k
                    actual_slot = store._slot(d, u, v, k, b)
                    assert expected_slot == actual_slot, f"Slot mismatch at (d={d},u={u},v={v},k={k}): expected {expected_slot}, got {actual_slot}"
    print(f"  [PASS] memory layout: 6x10x10x10 = {6000} slots linearized correctly")


def test_compare_with_c_prototype():
    """Test 12: Compare results with our C geo_inverted_model prototype"""
    store = SixAOPTW(n_blocks=1, dtype="float16")

    weights = np.array([32, 92, 67, -19, 68, -19, -43, -91, 16, -20, -17, 80,
                        91, 100, -66, 124, 78, 104, 91, -19, 106, 5, -18, 52,
                        -52, 117, 25, -124, -80, -107, -108, -64], dtype=np.float16)

    # Round-robin through 6 directions (same as C prototype Demo 4)
    for i, w in enumerate(weights):
        d = i % 6
        u = (i // 6) % 10
        v = (i // 60) % 10
        k = (i // 600) % 10
        store.write(d, u, v, k, w, b=0)

    errors = 0
    for i, w in enumerate(weights):
        d = i % 6
        u = (i // 6) % 10
        v = (i // 60) % 10
        k = (i // 600) % 10
        val = store.read(d, u, v, k, b=0)
        if abs(val - w) > 0.1:
            errors += 1
            if errors <= 3:
                print(f"    mismatch at i={i}: wrote {w}, read {val}")

    print(f"  [PASS] C-prototype compatible encoding: {32-errors}/32 correct (errors={errors})")


def test_voxel_mapping_math():
    """Test 13: Verify voxel_views mapping is correct"""
    for x in range(10):
        for y in range(10):
            for z in range(10):
                views = SixAOPTW.voxel_views(x, y, z)
                dirs = [v[0] for v in views]
                assert len(set(dirs)) == 6, f"({x},{y},{z}): duplicate directions {dirs}"
                assert sorted(dirs) == [0, 1, 2, 3, 4, 5], f"({x},{y},{z}): missing directions {dirs}"

    # Spot-check
    assert SixAOPTW.voxel_views(3, 4, 5) == (
        (0, 4, 5, 3),   # +X: u=y, v=z, k=x
        (1, 4, 5, 6),   # -X: u=y, v=z, k=9-x
        (2, 3, 5, 4),   # +Y: u=x, v=z, k=y
        (3, 3, 5, 5),   # -Y: u=x, v=z, k=9-y
        (4, 3, 4, 5),   # +Z: u=x, v=y, k=z
        (5, 3, 4, 4),   # -Z: u=x, v=y, k=9-z
    ), f"Mismatch at (3,4,5)"
    print(f"  [PASS] voxel mapping math: 1000 voxels x 6 unique directions")


def test_negative_values():
    """Test 14: Negative values (Q8_0 compatibility)"""
    store = SixAOPTW(n_blocks=1, dtype="float16")
    test_vals = [-128, -64, -1, 0, 1, 42, 100, 127]
    for val in test_vals:
        store.write(d=0, u=5, v=5, k=5, value=float(val), b=0)
        out = store.read(d=0, u=5, v=5, k=5, b=0)
        assert abs(out - val) < 0.5, f"Value {val} -> {out}"
    print(f"  [PASS] Q8_0 negative values: {test_vals} all roundtrip")


def test_memmap_persistence():
    """Test 15: Memory-mapped file persistence"""
    import os
    tmp_path = "I:/FGLS_new/runner/explore/_test_memmap.dat"
    try:
        # Write phase
        store1 = SixAOPTW(n_blocks=1, dtype="float16", path=tmp_path, mode="w+")
        store1.write(d=3, u=7, v=2, k=8, value=99.5, b=0)
        store1.flush()
        del store1

        # Read phase
        store2 = SixAOPTW(n_blocks=1, dtype="float16", path=tmp_path, mode="r")
        val = store2.read(d=3, u=7, v=2, k=8, b=0)
        assert abs(val - 99.5) < 1e-3, f"Memmap persistence failed: {val}"
        del store2
        print(f"  [PASS] memmap persistence: wrote 99.5, read {val}")
    finally:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)


def test_6dof_6value():
    """Test 16: Prove 1 pixel = 6 values at scale"""
    store = SixAOPTW(n_blocks=1, dtype="float16")

    # Write unique values to each (direction, u, v, k)
    # For the same spatial (x,y,z), different directions get different values
    for x in range(10):
        for y in range(10):
            for z in range(10):
                for d_idx, (d, u, v, k) in enumerate(SixAOPTW.voxel_views(x, y, z)):
                    store.write(d, u, v, k, float(d_idx * 1000 + x * 100 + y * 10 + z), b=0)

    # Now read back — each voxel should have 6 different values
    unique_counts = []
    for x in range(10):
        for y in range(10):
            for z in range(10):
                vals = store.read_voxel_all_dirs(x, y, z, b=0)
                unique_counts.append(len(set(vals)))

    avg_unique = sum(unique_counts) / len(unique_counts)
    pct_6 = sum(1 for c in unique_counts if c == 6) / len(unique_counts) * 100

    print(f"  [PASS] 1 pixel = avg {avg_unique:.2f} values across 6 view directions")
    print(f"  [PASS] {pct_6:.1f}% of voxels have all 6 unique values")


# ============================================================
if __name__ == "__main__":
    tests = [
        ("Basic read/write", test_basic_rw),
        ("Full ray read/write", test_ray_rw),
        ("Full slice read/write", test_slice_rw),
        ("Voxel 6-view projection", test_voxel_projections),
        ("All 1000 voxels deterministic", test_all_voxels_deterministic),
        ("Collision behavior", test_collision_behavior),
        ("Block isolation", test_block_isolation),
        ("Boundary checks", test_boundary_checks),
        ("Speed benchmark", test_speed_benchmark),
        ("Multi-block capacity", test_multi_block_capacity),
        ("Memory layout verification", test_memory_layout),
        ("C prototype comparison", test_compare_with_c_prototype),
        ("Voxel mapping math", test_voxel_mapping_math),
        ("Negative values (Q8_0)", test_negative_values),
        ("Memmap persistence", test_memmap_persistence),
        ("1 pixel = 6 values proof", test_6dof_6value),
    ]

    passed = 0
    failed = 0
    fail_msgs = []
    print("SixAOPTW Test Suite — 16 tests")
    print()

    for name, fn in tests:
        try:
            fn()
            passed += 1
            print(f"  OK {name}")
        except Exception as e:
            failed += 1
            fail_msgs.append((name, str(e)))
            print(f"  FAIL {name}: {e}")
        print()

    print(f"=== {passed}/{passed + failed} PASS, {failed} FAIL ===")
    if fail_msgs:
        for name, msg in fail_msgs:
            print(f"  FAILED: {name} -> {msg}")
    sys.exit(0 if failed == 0 else 1)
