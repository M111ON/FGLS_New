#!/usr/bin/env python3
"""
test_dramtile_pipeline.py — Phase 3: DRamTile-backed GeoField Pipeline

Tests: DRamTile store init → segment storage → retrieve → verify.
Compares DRamTile pipeline vs heap pipeline performance.
"""
import sys, os, time, ctypes

sys.path.insert(0, os.path.dirname(__file__))
import geofield_ctypes as gf

CHUNK_SZ = 64


def test_dramtile_store():
    """Test DRamTile store basic operations via C DLL."""
    print("=" * 65)
    print("Phase 3: DRamTile Pipeline Integration Test")
    print("=" * 65)

    # Test 1: Init DRamTile store (64MB capacity)
    print("\n[1] DRamTile store init (64MB)...", end=" ")
    dt_ctx = gf._lib.gfdt_init_test(64 * 1024 * 1024)
    assert dt_ctx is not None, "gfdt_init_test failed"
    print("OK")

    # Test 2: Store segments
    test_data = [b'\x00' * 64 * i for i in range(1, 17)]  # 16 segments, 64-1024 bytes each
    t0 = time.perf_counter()
    for i, data in enumerate(test_data):
        ptr = gf._lib.gfdt_store_segment_test(dt_ctx, i, data, len(data))
        assert ptr is not None, f"Failed to store segment {i}"
    t_store = time.perf_counter() - t0
    print(f"[2] Stored {len(test_data)} segments: {t_store*1000:.3f}ms")

    # Test 3: Retrieve segments (zero-copy pointer)
    t0 = time.perf_counter()
    for i, data in enumerate(test_data):
        ptr = gf._lib.gfdt_get_segment_test(dt_ctx, i)
        assert ptr is not None, f"Failed to get segment {i}"
        # Verify data matches (read from mmap pointer)
        stored = bytes(ctypes.cast(ptr, ctypes.POINTER(ctypes.c_uint8 * len(data))).contents)
        assert stored == data, f"Segment {i} data mismatch"
    t_get = time.perf_counter() - t0
    print(f"[3] Retrieved {len(test_data)} segments: {t_get*1000:.3f}ms (zero-copy)")

    # Test 4: DRamTile stats
    gf._lib.gfdt_print_stats_test(dt_ctx)

    # Test 5: Destroy
    gf._lib.gfdt_destroy_test(dt_ctx)
    print("[5] DRamTile store destroyed")

    print(f"\n  Store: {t_store*1000:.3f}ms  Get: {t_get*1000:.3f}ms")
    print("=" * 65)
    return True


def test_full_pipeline_dramtile():
    """Test full encode pipeline with DRamTile backing."""
    print("\n" + "=" * 65)
    print("Phase 3: Full Pipeline with DRamTile Backing")
    print("=" * 65)

    test_files = [
        '../tools/geopixel_pipeline.py',
        '../runner/llama_pogls_runner_sid_v2.c',
        '../collection/geo_field_core.h',
    ]

    for path in test_files:
        full = os.path.join(os.path.dirname(__file__), path)
        if not os.path.exists(full):
            continue

        with open(full, 'rb') as f:
            data = f.read()

        name = os.path.basename(full)
        print(f"\n  {name} ({len(data):,} bytes)")

        # Heap pipeline (current)
        t0 = time.perf_counter()
        max_segs = (len(data) // 32) + 256
        offsets = (ctypes.c_uint64 * max_segs)()
        lengths = (ctypes.c_uint64 * max_segs)()
        n_segs = gf._lib.geofield_flow_chunk(
            (ctypes.c_uint8 * len(data))(*data), len(data),
            32, 4096, offsets, lengths, max_segs
        )
        # Store in heap
        heap_ptrs = []
        for si in range(n_segs):
            seg_data = bytes(data[offsets[si]:offsets[si] + lengths[si]])
            buf = (ctypes.c_uint8 * len(seg_data))(*seg_data)
            heap_ptrs.append(buf)
        t_heap = time.perf_counter() - t0

        # DRamTile pipeline
        dt_ctx = gf._lib.gfdt_init_test(16 * 1024 * 1024)  # 16MB store
        t0 = time.perf_counter()
        for si in range(n_segs):
            seg_data = bytes(data[offsets[si]:offsets[si] + lengths[si]])
            ptr = gf._lib.gfdt_store_segment_test(dt_ctx, si, seg_data, len(seg_data))
            assert ptr is not None
        t_dramtile = time.perf_counter() - t0

        # Verify all segments retrievable
        for si in range(n_segs):
            ptr = gf._lib.gfdt_get_segment_test(dt_ctx, si)
            assert ptr is not None, f"Segment {si} not found in DRamTile"

        gf._lib.gfdt_destroy_test(dt_ctx)

        print(f"    Heap:      {t_heap*1000:.2f}ms")
        print(f"    DRamTile:  {t_dramtile*1000:.2f}ms")
        print(f"    Speedup:   {t_heap/t_dramtile:.1f}x")

    print("=" * 65)
    return True


if __name__ == '__main__':
    ok1 = test_dramtile_store()
    ok2 = test_full_pipeline_dramtile()
    print(f"\nRESULT: {'ALL PASS' if ok1 and ok2 else 'FAILED'}")
