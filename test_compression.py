import sys, time, os
sys.path.insert(0, 'tools')
from geopixel_pipeline import pipeline_encode, pipeline_decode, SEED_BACKEND

print("Backend:", SEED_BACKEND)
print()
print(f"{'File':<40} {'Size':>10} {'Chunks':>7} {'Encoded':>10} {'Ratio':>7} {'Enc':>8} {'Dec':>8} {'Pass':>5}")
print("-" * 100)

files = [
    ("geopixel_pipeline.py",    "tools/geopixel_pipeline.py"),
    ("dramtile_store.h",        "runner/dramtile_store.h"),
    ("addr_space.h",            "runner/addr_space.h"),
    ("llama_runner_sid_v2.c",   "runner/llama_pogls_runner_sid_v2.c"),
    ("gear_shift.h",            "runner/gear_shift.h"),
    ("kv_remap.h",              "runner/kv_remap.h"),
    ("HANDOFF.md",              "docs/HANDOFF.md"),
    ("geo_field.md",            "docs/geofield.md"),
    ("dramtile.md",             "docs/dramtile.md"),
    ("pogls_meta.h",            "runner/pogls_meta.h"),
    ("geopixel_gui.py",         "tools/geopixel_gui.py"),
    ("icosa_twin_bridge.cu",    "collection/src/icosa_twin_bridge.cu"),
    ("compile_commands.json",   "compile_commands.json"),
    ("wallet_seed_c.dll",       "collection/wallet_seed_c.dll"),
    ("POGLS_MASTER_SCHEMATIC.pdf", "docs/POGLS_MASTER_SCHEMATIC.pdf"),
]

for name, path in files:
    full = os.path.join('I:/FGLS_new', path)
    if not os.path.exists(full):
        continue
    data = open(full, 'rb').read()
    sz = len(data)
    n_chunks = (sz + 63) // 64

    t0 = time.perf_counter()
    result = pipeline_encode(data)
    t_enc = time.perf_counter() - t0

    t0 = time.perf_counter()
    dec = pipeline_decode(result.encoded)
    t_dec = time.perf_counter() - t0

    ok = dec['data'] == data
    tag = "PASS" if ok else "FAIL"

    if sz >= 1024 * 1024:
        sz_str = f"{sz/1024/1024:.1f}MB"
    elif sz >= 1024:
        sz_str = f"{sz/1024:.1f}KB"
    else:
        sz_str = f"{sz}B"

    enc_sz = len(result.encoded)
    if enc_sz >= 1024 * 1024:
        enc_str = f"{enc_sz/1024/1024:.1f}MB"
    elif enc_sz >= 1024:
        enc_str = f"{enc_sz/1024:.1f}KB"
    else:
        enc_str = f"{enc_sz}B"

    print(f"{name:<40} {sz_str:>10} {n_chunks:>7} {enc_str:>10} {result.ratio:>6.2f}x {t_enc*1000:>7.1f}ms {t_dec*1000:>7.1f}ms {tag:>5}")
