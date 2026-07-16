"""Real-data compression benchmark using GFCS pipeline."""
import sys, os, time, struct, hashlib
sys.path.insert(0, '.')
import geofield_ctypes as gf

TEMP = 'bench_temp'
files = [
    'test01.bmp',
    'test_pywrite.pogls',
    'ses_concat.bin',
    'qwen25_head_20m.bin',
    'fusion_head_20m.bin',
]

def fmt_size(n):
    for unit in ['B','KB','MB']:
        if n < 1024: return '%.2f %s' % (n, unit)
        n /= 1024.0
    return '%.2f GB' % (n)

def bench_file(path):
    data = open(os.path.join(TEMP, path), 'rb').read()
    sz = len(data)

    t0 = time.time()
    comp, stats = gf.full_compress(data)
    t1 = time.time()
    if comp is None:
        print('  GFCS: COMPRESS FAILED (stats: n_segs=%d n_blocks=%d)' % (
            stats.n_segments, stats.n_blocks))
        return None

    comp_sz = len(comp)
    ratio = comp_sz / sz
    comp_mbs = sz / (t1 - t0) / 1024 / 1024 if (t1 - t0) > 0 else 0

    t2 = time.time()
    dec = gf.full_decompress(comp)
    t3 = time.time()
    if dec is None:
        print('  GFCS: DECOMPRESS FAILED')
        return None
    dec_data = dec[0]
    decomp_mbs = sz / (t3 - t2) / 1024 / 1024 if (t3 - t2) > 0 else 0

    sha_ok = hashlib.sha256(data).hexdigest() == hashlib.sha256(dec_data).hexdigest()

    ds = stats.diamond_hits
    total_ds = ds[0] + ds[1] + ds[2]
    pct_flat = 100.0 * ds[0] / total_ds if total_ds > 0 else 0

    return {
        'path': path, 'size': sz, 'comp_sz': comp_sz,
        'ratio': comp_sz / sz, 'inverse': sz / comp_sz,
        'comp_mbs': comp_mbs, 'decomp_mbs': decomp_mbs,
        'n_segs': stats.n_segments, 'n_blocks': stats.n_blocks,
        'n_patterns': stats.n_patterns, 'wall_ms': stats.wall_ms,
        'ds_flat': ds[0], 'ds_sparse': ds[1], 'ds_dense': ds[2],
        'pct_flat': pct_flat, 'sha_ok': sha_ok,
        'comp_time': t1 - t0, 'decomp_time': t3 - t2,
    }

print('GeoField GFCS Compression Benchmark — Real Data')
print('=' * 70)

results = []
for f in files:
    print('')
    print('--- %s ---' % f)
    sys.stdout.flush()
    r = bench_file(f)
    if r:
        results.append(r)
        print('  Size: %s -> %s  (%.2fx, %.1f MB/s comp, %.1f MB/s decomp)' % (
            fmt_size(r['size']), fmt_size(r['comp_sz']),
            r['inverse'], r['comp_mbs'], r['decomp_mbs']))
        print('  Blocks: %d  Patterns: %d  Segs: %d  FLAT: %.1f%%' % (
            r['n_blocks'], r['n_patterns'], r['n_segs'], r['pct_flat']))
        print('  Integrity: %s' % ('PASS' if r['sha_ok'] else 'FAIL'))

print('')
print('=' * 70)
print('SUMMARY')
print('=' * 70)
print('%-27s %10s %10s %7s %8s %8s %6s %7s' % (
    'File', 'Size', 'Comp', 'Ratio', 'Comp/s', 'Decom/s', 'Blocks', 'Flat%'))
print('-' * 80)
for r in results:
    print('%-27s %10s %10s %6.2fx %7.0f %7.0f %6d %6.0f%%' % (
        r['path'], fmt_size(r['size']), fmt_size(r['comp_sz']),
        r['inverse'], r['comp_mbs'], r['decomp_mbs'],
        r['n_blocks'], r['pct_flat']))
