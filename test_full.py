import sys, os, time
sys.path.insert(0, 'tools')
from geopixel_pipeline import pipeline_encode, pipeline_decode

files = [
    ('runner/build.sh', 'shell script'),
    ('tools/geopixel_pipeline.py', 'Python source (39KB)'),
    ('tools/geopixel_gui.py', 'Python GUI (9KB)'),
    ('tools/geopixel_service_v2.py', 'Python service (13KB)'),
    ('collection/dgls/geo/include/geo_field_core.h', 'C header (33KB)'),
    ('collection/dgls/geo/include/skeleton_index.h', 'C header (9KB)'),
    ('collection/dgls/diamond/include/diamond_shell_v2.h', 'C header'),
]

print(f"{'File':30s} {'Input':>8s} {'Output':>8s} {'Ratio':>6s} {'Skeleton':30s} {'Enc':>8s} {'Dec':>8s} {'OK':>4s}")
print("-" * 110)

for path, desc in files:
    if not os.path.exists(path):
        continue
    data = open(path, 'rb').read()
    n_chunks = (len(data) + 63) // 64
    t0 = time.perf_counter()
    result = pipeline_encode(data)
    t_enc = time.perf_counter() - t0
    enc = result.encoded

    t0 = time.perf_counter()
    dec = pipeline_decode(enc)
    t_dec = time.perf_counter() - t0
    ok = dec['data'][:len(data)] == data

    skel = result.skel_stats
    skel_names = ['ID','FLAT','DIFF','BREF','GEOM','RAW']
    skel_str = ' '.join(f'{n}={c}' for n,c in zip(skel_names, skel) if c>0)

    ratio = len(data) / len(enc) if len(enc) > 0 else 0
    print(f"{desc:30s} {len(data):>7d}B {len(enc):>7d}B  {ratio:.2f}x  {skel_str:30s} {t_enc*1000:>7.1f}ms {t_dec*1000:>7.1f}ms  {'PASS' if ok else 'FAIL'}")
