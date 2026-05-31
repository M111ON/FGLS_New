"""Generate test .gsidx/.gsdat for C reader verification."""
import sys
sys.path.insert(0, 'python_src')
import numpy as np
from geometry_store import GeometryStore, NAMESPACE_SHIFT, SHAPES

store = GeometryStore('build/test_geom', read_only=False)

# Write deterministic test data
rng = np.random.RandomState(42)
dim = 64  # small dim for easy verification

for ns_name, ns_shift in sorted(NAMESPACE_SHIFT.items()):
    for z in range(4):  # 4 zones per ns
        for s in ['I', 'O', 'S']:
            vals = (rng.randn(8, dim) * 10 + ns_shift + z).astype(np.float32)
            store.index(z, s, vals, ns=ns_name)

store.flush()
store.close()

# Verify with Python reader
store_r = GeometryStore('build/test_geom', read_only=True)
s = store_r.stats()
print(f"Store: {s['n_keys']} keys, {s['total_rows']} rows, {s['data_kb']} KB")

# Dump headers for C reader reference
with open('build/test_geom.gsidx', 'rb') as f:
    raw = f.read()
print(f"\n.gsidx: {len(raw)} bytes")
print(f"  header: {raw[:32].hex()}")
print(f"  magic={raw[:8]}")
import struct
n_entries = struct.unpack_from('<I', raw, 8)[0]
data_size = struct.unpack_from('<Q', raw, 12)[0]
print(f"  n_entries={n_entries} data_size={data_size}")

for i in range(n_entries):
    off = 32 + i * 20
    entry = raw[off:off+20]
    z, si, d_off, n_rows, n_cols = struct.unpack_from('<HHqII', entry, 0)
    s_char = SHAPES[si] if si < len(SHAPES) else '?'
    print(f"  [{i}] zone={z} shape={s_char} offset={d_off} rows={n_rows} cols={n_cols}")

store_r.close()
print("\nPython reader: OK")
