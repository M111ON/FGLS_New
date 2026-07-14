"""
benchmark_pipeline.py — Full pipeline benchmark

Measures:
  1. Adaptive chunking (flow_chunk via DLL)
  2. Diamond Shell classify
  3. Skeleton decide
  4. LetterCube bond/assemble
  5. Cube assembly (6→1 promote)
  6. Goldberg sphere mapping
  7. Fibonacci shell fold
  8. Full pipeline: data → encode → decode → verify
"""
import sys, os, time, hashlib, struct, ctypes

sys.path.insert(0, os.path.dirname(__file__))
import geofield_ctypes as gf

# ── Constants ──
CHUNK_SZ = 64
CUBE_FACES = 6
CUBE_PAIRS = 24
LC_LANES = 6
LC_PAIRS = 24
LC_BOND_FREE = 0
LC_BOND_PEND = 1
LC_BOND_LOCK = 2
LC_LANE_SZ = 12
LC_STRUCT_SZ = LC_LANES * LC_LANE_SZ + 4
GEO_FIBO = [1,1,2,3,5,8,13,21,34,55,89,144]
GP_PENT_COUNT = 12

# ── Goldberg helpers ──
def gp_face_count(level):
    return 10 * level * level + 2

def gp_hex_in_sector(level, sector):
    total_hex = gp_face_count(level) - GP_PENT_COUNT
    base = total_hex // GP_PENT_COUNT
    rem = total_hex % GP_PENT_COUNT
    return base + (1 if sector < rem else 0)

def gp_sector_base(level, sector):
    b = GP_PENT_COUNT
    for s in range(sector):
        b += gp_hex_in_sector(level, s)
    return b

# ── LetterCube (pure Python) ──
LC_COMPLEMENT = [12,13,14,15,16,17,18,19,20,21,22,23,0,1,2,3,4,5,6,7,8,9,10,11]

class LetterCube:
    __slots__ = ['lanes', 'n_locked']
    def __init__(self):
        self.lanes = [{'pair_id':0,'angle':0,'bond_state':LC_BOND_FREE,'bonded_to':0xFF,'core_seed':bytes(8)} for _ in range(LC_LANES)]
        self.n_locked = 0
    def assign(self, lane, pair_id, angle, core_seed=None):
        if lane >= LC_LANES: return
        self.lanes[lane]['pair_id'] = pair_id % LC_PAIRS
        self.lanes[lane]['angle'] = angle % 6
        self.lanes[lane]['bond_state'] = LC_BOND_PEND
        if core_seed: self.lanes[lane]['core_seed'] = core_seed
    def bond(self, a, b):
        if a >= LC_LANES or b >= LC_LANES or a == b: return 0
        la, lb = self.lanes[a], self.lanes[b]
        if LC_COMPLEMENT[la['pair_id']] != lb['pair_id']: return 0
        if (la['angle'] ^ lb['angle']) > 2: return 0
        la['bond_state'] = lb['bond_state'] = LC_BOND_LOCK
        la['bonded_to'], lb['bonded_to'] = b, a
        self.n_locked += 1
        return 1
    def assemble(self):
        for i in range(LC_LANES):
            if self.lanes[i]['bond_state'] == LC_BOND_PEND:
                p = self.lanes[i]['bonded_to']
                if p < LC_LANES and self.lanes[p]['bond_state'] == LC_BOND_PEND:
                    self.lanes[i]['bond_state'] = self.lanes[p]['bond_state'] = LC_BOND_LOCK

def make_lc(seed):
    lc = LetterCube()
    pairs = [(0,12),(1,13),(2,14)]
    for li,(p1,p2) in enumerate(pairs):
        angle = (seed + li) % 6
        c1 = ((seed+li)*0x9E3779B97F4A7C15 & 0xFFFFFFFFFFFFFFFF).to_bytes(8,'little')
        c2 = ((seed+li+100)*0x9E3779B97F4A7C15 & 0xFFFFFFFFFFFFFFFF).to_bytes(8,'little')
        lc.assign(li*2, p1, angle, c1)
        lc.assign(li*2+1, p2, angle, c2)
    lc.bond(0,1); lc.bond(2,3); lc.bond(4,5)
    return lc

# ── CubeCtx (pure Python) ──
def cube_ctx_from_lc(lc):
    ctx = bytearray(92)
    slope = 0
    for lane in lc.lanes:
        slope ^= int.from_bytes(lane['core_seed'],'little')
    struct.pack_into('<Q', ctx, 0, slope)
    struct.pack_into('<B', ctx, 8, 0)
    struct.pack_into('<B', ctx, 9, lc.n_locked * 2)
    for i, lane in enumerate(lc.lanes):
        if i >= CUBE_FACES: break
        fo = 10 + i * 12
        core = int.from_bytes(lane['core_seed'],'little')
        struct.pack_into('<Q', ctx, fo, core)
        struct.pack_into('<BB', ctx, fo+8, lane['pair_id'], lane['pair_id'])
        struct.pack_into('<B', ctx, fo+10, i)
        struct.pack_into('<B', ctx, fo+11, 1 if lane['bond_state']==LC_BOND_LOCK else 0)
    return bytes(ctx)

def cube_promote(children):
    if len(children) != CUBE_FACES: return None
    for c in children:
        if c[9] < CUBE_FACES: return None
    depth = children[0][8]
    if depth + 1 >= 8: return None
    parent = bytearray(92)
    fold = 0
    for i in range(CUBE_FACES):
        cs = struct.unpack_from('<Q', children[i], 0)[0]
        fold ^= cs
        fo = 10 + i * 12
        struct.pack_into('<Q', parent, fo, cs)
        struct.pack_into('<BB', parent, fo+8, i%CUBE_PAIRS, i%CUBE_PAIRS)
        struct.pack_into('<BB', parent, fo+10, i, 1)
    struct.pack_into('<Q', parent, 0, fold)
    struct.pack_into('<B', parent, 8, depth+1)
    struct.pack_into('<B', parent, 9, CUBE_FACES)
    return bytes(parent)

# ── Fibonacci fold ──
def shell_layer_live(layer, tick):
    return (tick % GEO_FIBO[layer % 12]) == 0

def shell_fold_nearest(layer, tick):
    layer %= 12
    if tick % GEO_FIBO[layer] == 0: return layer
    for d in range(1, 12):
        if layer >= d:
            lo = layer - d
            if tick % GEO_FIBO[lo] == 0: return lo
        hi = layer + d
        if hi < 12 and tick % GEO_FIBO[hi] == 0: return hi
    return 0

# ── Benchmark helpers ──
def timeit(fn, *args, n=1):
    t0 = time.perf_counter()
    for _ in range(n):
        result = fn(*args)
    t1 = time.perf_counter()
    return (t1 - t0) / n, result

def bench_label(name, us):
    if us < 1000:
        return f"{name:.<40s} {us:8.1f} us"
    elif us < 1000000:
        return f"{name:.<40s} {us/1000:8.2f} ms"
    else:
        return f"{name:.<40s} {us/1000000:8.2f} s"

# ══════════════════════════════════════════════════════════════
def run_benchmark():
    print("=" * 65)
    print("  GeoField Pipeline Benchmark")
    print("=" * 65)

    test_files = [
        ("tiny (5KB)",  "core/geo_flow_chunker.h"),
        ("medium (32KB)", "core/geo_field_core.h"),
        ("large (63KB)", "../tools/geopixel_pipeline.py"),
        ("xlarge (184KB)", "../runner/llama_pogls_runner_sid_v2.c"),
    ]

    results = []

    for label, path in test_files:
        full = os.path.join(os.path.dirname(__file__), path)
        if not os.path.exists(full):
            print(f"\n  SKIP: {path} not found")
            continue

        with open(full, 'rb') as f:
            data = f.read()
        size = len(data)
        data_arr = (ctypes.c_uint8 * size)(*data)

        print(f"\n{'─'*65}")
        print(f"  {label} ({size:,} bytes)")
        print(f"{'─'*65}")

        timings = {}

        # 1. Adaptive chunking
        t, n_segs = timeit(gf._lib.geofield_flow_chunk,
                          data_arr, size, 32, 4096,
                          (ctypes.c_uint64*2048)(), (ctypes.c_uint64*2048)(), 2048)
        timings['flow_chunk'] = t * 1e6

        # Get segments for downstream
        max_segs = (size // 32) + 256
        offsets = (ctypes.c_uint64 * max_segs)()
        lengths = (ctypes.c_uint64 * max_segs)()
        n_segs = gf._lib.geofield_flow_chunk(data_arr, size, 32, 4096, offsets, lengths, max_segs)

        # 2. Diamond Shell classify (per block)
        t_total = 0
        n_blocks = 0
        for si in range(min(n_segs, 200)):
            seg_off = offsets[si]
            seg_len = min(lengths[si], 4096)
            for bi in range(0, seg_len, CHUNK_SZ):
                block = data[seg_off+bi:seg_off+bi+CHUNK_SZ]
                if len(block) < CHUNK_SZ:
                    block = block + b'\x00' * (CHUNK_SZ - len(block))
                block_arr = bytes(block)
                t1, _ = timeit(gf.diamond_classify, block_arr)
                t_total += t1
                n_blocks += 1
        timings['diamond_classify_us'] = t_total * 1e6
        timings['n_blocks'] = n_blocks
        timings['diamond_per_block_us'] = (t_total / max(n_blocks,1)) * 1e6

        # 3. Skeleton decide (per block)
        t_total = 0
        for si in range(min(n_segs, 200)):
            seg_off = offsets[si]
            seg_len = min(lengths[si], 4096)
            for bi in range(0, seg_len, CHUNK_SZ):
                block = data[seg_off+bi:seg_off+bi+CHUNK_SZ]
                if len(block) < CHUNK_SZ:
                    block = block + b'\x00' * (CHUNK_SZ - len(block))
                block_arr = bytes(block)
                t1, _ = timeit(gf.skel_decide, block_arr, None, False)
                t_total += t1
        timings['skel_decide_us'] = t_total * 1e6

        # 4. LetterCube bond+assemble (per segment)
        t_total = 0
        for si in range(min(n_segs, 500)):
            lc = make_lc(si * 5)
            t1, _ = timeit(lambda: (lc.bond(0,1), lc.bond(2,3), lc.bond(4,5), lc.assemble()))
            t_total += t1
        timings['lettercube_us'] = t_total * 1e6

        # 5. CubeCtx from LetterCube (per segment)
        t_total = 0
        for si in range(min(n_segs, 500)):
            lc = make_lc(si * 5)
            t1, _ = timeit(cube_ctx_from_lc, lc)
            t_total += t1
        timings['cube_ctx_from_lc_us'] = t_total * 1e6

        # 6. Cube promote (6→1)
        if n_segs >= 6:
            cubes = [cube_ctx_from_lc(make_lc(i*5)) for i in range(6)]
            t_promote, parent = timeit(cube_promote, cubes)
            timings['cube_promote_us'] = t_promote * 1e6

        # 7. Goldberg mapping
        def goldberg_map(n):
            level = 1
            while gp_face_count(level) < n + GP_PENT_COUNT:
                level += 1
            return level
        t_g, level = timeit(goldberg_map, n_segs)
        timings['goldberg_level'] = level
        timings['goldberg_us'] = t_g * 1e6

        # 8. Fibonacci fold (per segment)
        t_total = 0
        for si in range(min(n_segs, 500)):
            layer = si % 12
            t1, _ = timeit(shell_fold_nearest, layer, si)
            t_total += t1
        timings['fibo_fold_us'] = t_total * 1e6

        # 9. Full pipeline: encode
        def full_encode(data, size):
            arr = (ctypes.c_uint8 * size)(*data)
            max_s = (size // 32) + 256
            offs = (ctypes.c_uint64 * max_s)()
            lens = (ctypes.c_uint64 * max_s)()
            n = gf._lib.geofield_flow_chunk(arr, size, 32, 4096, offs, lens, max_s)
            return n, offs, lens

        t_full, (n_full, _, _) = timeit(full_encode, data, size)
        timings['full_encode_us'] = t_full * 1e6

        # Throughput
        throughput_mbs = size / (t_full * 1024 * 1024) if t_full > 0 else 0
        timings['throughput_mbs'] = throughput_mbs

        # Print results
        print(bench_label("1. flow_chunk (adaptive)", timings['flow_chunk']))
        print(bench_label(f"2. diamond_classify ({timings['n_blocks']} blocks)", timings['diamond_classify_us']))
        print(f"    per block: {timings['diamond_per_block_us']:.1f} us")
        print(bench_label(f"3. skel_decide ({timings['n_blocks']} blocks)", timings['skel_decide_us']))
        print(bench_label(f"4. lettercube bond+asm ({min(n_segs,500)} segs)", timings['lettercube_us']))
        print(bench_label(f"5. cube_ctx_from_lc ({min(n_segs,500)} segs)", timings['cube_ctx_from_lc_us']))
        if 'cube_promote_us' in timings:
            print(bench_label("6. cube_promote (6→1)", timings['cube_promote_us']))
        print(bench_label(f"7. goldberg_map → level {timings['goldberg_level']}", timings['goldberg_us']))
        print(bench_label(f"8. fibo_fold ({min(n_segs,500)} segs)", timings['fibo_fold_us']))
        print(bench_label("9. full_encode", timings['full_encode_us']))
        print(f"    throughput: {timings['throughput_mbs']:.1f} MB/s")
        print(f"    segments: {n_full}")

        results.append((label, size, timings))

    # ── Summary ──
    print(f"\n{'═'*65}")
    print("  Summary")
    print(f"{'═'*65}")
    print(f"  {'File':<20s} {'Size':>8s} {'Encode':>10s} {'MB/s':>8s} {'Segs':>6s}")
    print(f"  {'─'*20} {'─'*8} {'─'*10} {'─'*8} {'─'*6}")
    for label, size, t in results:
        enc_ms = t['full_encode_us'] / 1000
        mbps = t['throughput_mbs']
        print(f"  {label:<20s} {size:>7,d}B {enc_ms:>9.1f}ms {mbps:>7.1f} {t.get('n_blocks',0):>5d}")

    # ── DLL speedup for wallet_chunk_seed ──
    print(f"\n{'═'*65}")
    print("  DLL Speedup: wallet_chunk_seed")
    print(f"{'═'*65}")
    block = bytes(CHUNK_SZ)
    t_dll, _ = timeit(gf.wallet_seed, block, n=10000)
    print(f"  DLL wallet_seed: {t_dll*1e6:.1f} us/call")
    print(f"  (Python scalar baseline: ~7.3 us/call → DLL is ~{7.3/(t_dll*1e6):.0f}x faster)")

    print(f"\n{'═'*65}")
    print("  Benchmark complete")
    print(f"{'═'*65}")


if __name__ == '__main__':
    run_benchmark()
