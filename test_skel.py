import sys, os, time
sys.path.insert(0, 'tools')
from geopixel_pipeline import *

test_files = [
    ("random 256B", os.urandom(256)),
    ("zeros 128B", b'\x00' * 128),
    ("repeated AA 512B", b'\xAA' * 512),
    ("near-identical", bytes(range(64)) * 4 + bytes(range(63)) + b'\x01'),
]

for name, data in test_files:
    t0 = time.perf_counter()
    result = pipeline_encode(data, base=4)
    t1 = time.perf_counter()
    dec_result = pipeline_decode(result.encoded)
    dec = dec_result['data']
    t2 = time.perf_counter()
    ok = dec == data
    stats_str = " ".join(f"{n}={c}" for n, c in zip(SKEL_NAMES, result.skel_stats))
    print(f"{name:25s} {len(data):>6d}B -> {len(result.encoded):>6d}B  {result.ratio:.2f}x  {result.total_time*1000:.1f}ms  {stats_str}  verified={dec_result['verified']} failed={dec_result['failed']}  {'PASS' if ok else 'FAIL'}")

print("\n--- Real files ---")
for fn in ["runner/llama_pogls_runner_sid_v2.c", "collection/dgls/geo/include/skeleton_index.h", "collection/dgls/geo/include/geo_field_core.h"]:
    fpath = os.path.join("I:/FGLS_new", fn)
    if os.path.exists(fpath):
        with open(fpath, 'rb') as f:
            data = f.read()
        result = pipeline_encode(data, base=4)
        dec_result = pipeline_decode(result.encoded)
        dec = dec_result['data']
        ok = dec == data
        stats_str = " ".join(f"{n}={c}" for n, c in zip(SKEL_NAMES, result.skel_stats))
        print(f"{fn:50s} {len(data):>8d}B -> {len(result.encoded):>8d}B  {result.ratio:.2f}x  {stats_str}  verified={dec_result['verified']} failed={dec_result['failed']}  {'PASS' if ok else 'FAIL'}")
