"""Test adaptive flow chunking on real files."""
import sys, os, time, ctypes
sys.path.insert(0, os.path.dirname(__file__))
import geofield_ctypes as gf

test_files = [
    '../tools/geopixel_pipeline.py',
    '../runner/llama_pogls_runner_sid_v2.c',
    '../core/geo_field_core.h',
    '../collection/geopixel/hbv_bundle/geo_flow_chunker_v8.h',
]

for path in test_files:
    full = os.path.join(os.path.dirname(__file__), path)
    if not os.path.exists(full):
        print(f"\n{path}: NOT FOUND")
        continue
    with open(full, 'rb') as f:
        data = f.read()
    sz = len(data)

    print(f"\n{os.path.basename(path)} ({sz:,} bytes)")

    max_segs = 16384
    offsets = (ctypes.c_uint64 * max_segs)()
    lengths = (ctypes.c_uint64 * max_segs)()

    t0 = time.perf_counter()
    n = gf._lib.geofield_flow_chunk(data, sz, 32, 4096, offsets, lengths, max_segs)
    t1 = time.perf_counter()

    if n > 0:
        sizes = [lengths[i] for i in range(n)]
        avg = sum(sizes) / len(sizes)
        mn = min(sizes)
        mx = max(sizes)
        unique = len(set(sizes))
        print(f"  Segments: {n}  avg={avg:.0f}B  min={mn}B  max={mx}B  unique_sizes={unique}  time={((t1-t0)*1000):.1f}ms")
        for i in range(min(8, n)):
            print(f"    [{i:4d}] offset={offsets[i]:8d}  len={lengths[i]:6d}")
        if n > 8:
            print(f"    ... ({n-8} more)")
    else:
        print(f"  Flow chunk returned {n}")
