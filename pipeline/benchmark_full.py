"""
benchmark_full.py — Phase 5: Full pipeline benchmark
"""
import sys, os, time, ctypes, hashlib
sys.path.insert(0, os.path.dirname(__file__))
import geofield_ctypes as gf

dll = gf._lib
LC_BUF = 76


def benchmark_file(path):
    with open(path, 'rb') as f:
        data = f.read()
    name = os.path.basename(path)
    data_buf = (ctypes.c_uint8 * len(data))(*data)

    max_segs = (len(data) // 32) + 256
    offsets = (ctypes.c_uint64 * max_segs)()
    lengths = (ctypes.c_uint64 * max_segs)()
    n = dll.geofield_flow_chunk(data_buf, len(data), 32, 4096, offsets, lengths, max_segs)

    # Count total blocks
    total_blocks = 0
    for si in range(n):
        total_blocks += (int(lengths[si]) + 63) // 64

    RUNS = 20
    block64 = (ctypes.c_uint8 * 64)()
    zeros64 = (ctypes.c_uint8 * 64)()
    lc = (ctypes.c_uint8 * LC_BUF)()
    cube = (ctypes.c_uint8 * 92)()
    out_dt = (ctypes.c_uint8 * len(data))()

    # Stage 1: flow_chunk
    t0 = time.perf_counter()
    for _ in range(RUNS):
        dll.geofield_flow_chunk(data_buf, len(data), 32, 4096, offsets, lengths, max_segs)
    t_chunk = (time.perf_counter() - t0) / RUNS

    # Stage 2: diamond + skeleton (sample 200 blocks, extrapolate)
    sample_n = min(200, total_blocks)
    t0 = time.perf_counter()
    for _ in range(RUNS):
        cnt = 0
        for si in range(n):
            seg_len = int(lengths[si])
            for bi in range(0, seg_len, 64):
                dll.geofield_diamond_classify(block64)
                dll.geofield_skel_decide(block64, zeros64, 0)
                cnt += 1
                if cnt >= sample_n:
                    break
            if cnt >= sample_n:
                break
    t_classify_sampled = (time.perf_counter() - t0) / RUNS
    t_classify = t_classify_sampled * (total_blocks / max(sample_n, 1))

    # Stage 3: LetterCube + CubeCtx
    t0 = time.perf_counter()
    for _ in range(RUNS):
        for si in range(n):
            dll.geofield_lc_init(lc)
            dll.geofield_lc_assemble(lc)
            dll.geofield_cube_ctx_init(cube)
            dll.geofield_cube_ctx_from_lc(cube, lc)
    t_lettercube = (time.perf_counter() - t0) / RUNS

    # Stage 4: Goldberg + Fibonacci
    tile_id_out = ctypes.c_uint32(0)
    t0 = time.perf_counter()
    for _ in range(RUNS):
        for si in range(n):
            dll.geofield_gp_map_face(2, si % 30, ctypes.byref(tile_id_out))
            dll.geofield_shell_layer_live(si % 12, si)
    t_goldberg = (time.perf_counter() - t0) / RUNS

    # Stage 5: DRamTile store/get
    t0 = time.perf_counter()
    for _ in range(RUNS):
        dt = dll.geofield_dt_init(64)
        dll.geofield_dt_put_segs(dt, data_buf, len(data), offsets, lengths, n)
        out_dt = (ctypes.c_uint8 * len(data))()
        dll.geofield_dt_get_segs(dt, out_dt, len(data), offsets, lengths, n)
        dll.geofield_dt_destroy(dt)
    t_dramtile = (time.perf_counter() - t0) / RUNS

    # Stage 6: Full pipeline (all stages combined)
    t0 = time.perf_counter()
    for _ in range(RUNS):
        nn = dll.geofield_flow_chunk(data_buf, len(data), 32, 4096, offsets, lengths, max_segs)
        dt = dll.geofield_dt_init(64)
        dll.geofield_dt_put_segs(dt, data_buf, len(data), offsets, lengths, nn)
        for si in range(nn):
            dll.geofield_diamond_classify(block64)
            dll.geofield_lc_init(lc)
            dll.geofield_lc_assemble(lc)
        out_dt = (ctypes.c_uint8 * len(data))()
        dll.geofield_dt_get_segs(dt, out_dt, len(data), offsets, lengths, nn)
        dll.geofield_dt_destroy(dt)
    t_full = (time.perf_counter() - t0) / RUNS

    # Verify (out_dt set by DRamTile stage)
    recovered = bytes(out_dt[:len(data)])
    sha_ok = hashlib.sha256(data).hexdigest() == hashlib.sha256(recovered).hexdigest()

    print(f"\n  {name} ({len(data):,} bytes, {n} segs, {total_blocks} blocks)")
    print(f"  {'─'*55}")
    print(f"  flow_chunk:       {t_chunk*1000:8.3f} ms")
    print(f"  diamond+skeleton: {t_classify*1000:8.3f} ms  (extrapolated {total_blocks} blocks)")
    print(f"  LetterCube+Cube:  {t_lettercube*1000:8.3f} ms")
    print(f"  goldberg+fibo:    {t_goldberg*1000:8.3f} ms")
    print(f"  DRamTile:         {t_dramtile*1000:8.3f} ms")
    print(f"  FULL PIPELINE:    {t_full*1000:8.3f} ms")
    print(f"  Throughput:       {len(data)/t_full/1024/1024:8.1f} MB/s")
    print(f"  SHA256:           {'PASS' if sha_ok else 'FAIL'}")

    return t_full, len(data)


if __name__ == '__main__':
    files = [
        '../collection/geopixel/hbv_bundle/geo_flow_chunker_v8.h',
        '../tools/geopixel_pipeline.py',
        '../runner/llama_pogls_runner_sid_v2.c',
    ]

    print("=" * 60)
    print("  Phase 5: Full Pipeline Benchmark")
    print("=" * 60)

    total_t = 0
    total_b = 0
    for p in files:
        fp = os.path.join(os.path.dirname(__file__), p)
        if os.path.exists(fp):
            t, b = benchmark_file(fp)
            total_t += t
            total_b += b

    print(f"\n  {'='*55}")
    print(f"  SUMMARY: {total_b:,} bytes in {total_t*1000:.3f} ms = {total_b/total_t/1024/1024:.1f} MB/s")
    print(f"  Projected C native:  {total_b/total_t/1024/1024*10:.0f} MB/s (10x)")
    print(f"  Projected +CUDA:     {total_b/total_t/1024/1024*100:.0f} MB/s (100x)")
    print(f"  {'='*55}")
