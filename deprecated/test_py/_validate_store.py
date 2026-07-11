"""Store coverage analysis + FFN weight validation"""
import sys, numpy as np
sys.path.insert(0, 'python_src')
from geometry_store import GeometryStore

store = GeometryStore('build/qwen_geom_v2', read_only=True)
s = store.stats()
print("=== Store Coverage ===")
print(f"{s['n_keys']}/72 keys  {s['total_rows']:,} rows")

# Zone distribution
for z in sorted(s['keys_by_zone']):
    print(f"  zone {z}: {s['keys_by_zone'][z]} keys")
print(f"  missing zones: {[z for z in range(12) if z not in s['keys_by_zone']]}")
store.close()

# Now validate FFN weight assignments
print("\n=== FFN Weight Validation ===")
from gguf import GGUFReader
r = GGUFReader(r'I:\Vault\models\Qwen3-0.6B-Q8_0.gguf')

# Find tensors
tensors = {}
for t in r.tensors:
    n = t.name
    if 'blk.0.' in n:
        name = n.split('.', 2)[2]  # blk.0.attn_q.weight -> attn_q.weight
        tensors[name] = np.array(t.data, dtype=np.float32)

# Route
import torch
from zero_warmup_engine import _build_router_on_device
router = _build_router_on_device(128, 2, 32, 'build/bermuda_gate_g2_d128.pt', 'cpu')
with torch.no_grad():
    router.route(torch.randn(2, 128), 0)

MODE_NAMES = ['ORBITAL', 'CHIRAL', 'CROSS', 'HUB']
SHAPES = ['I', 'O', 'T', 'S', 'Z', 'L']
ZONES = 'ABCDEFGHIJKL'

print("\nLayer 0 routing for each weight type x mode:")
weight_names = ['attn_q.weight', 'attn_k.weight', 'attn_v.weight',
                'attn_o.weight', 'ffn_gate.weight', 'ffn_up.weight', 'ffn_down.weight']

for wn in weight_names:
    w = tensors.get(wn)
    if w is None:
        print(f"  {wn}: NOT FOUND")
        continue
    print(f"\n  {wn}: shape={w.shape}")
    for mode, mname in enumerate(MODE_NAMES):
        probe = w[0, :128].astype(np.float32)
        t = torch.from_numpy(probe[np.newaxis, :])
        with torch.no_grad():
            core = t - t.mean(dim=-1, keepdim=True)
            gate_g = router._get_gate(1)
            idx, _, _ = gate_g.encode_tokens(core)
            v = router.classify_idx(idx, mode, gear=1)
        z = int(v.zone[0].item())
        si = int(v.shape[0].item())
        s = SHAPES[si] if si < len(SHAPES) else '?'
        in_store = GeometryStore('build/qwen_geom_v2', read_only=True).has(z, s)
        print(f"    mode {mode} ({mname:7s}): zone={z:2d}({ZONES[z]}) shape={s}  in_store={in_store}")

# Multi-row consistency check
print("\n=== Routing consistency across rows ===")
for wn in ['attn_q.weight', 'ffn_gate.weight', 'ffn_up.weight']:
    w = tensors.get(wn)
    if w is None: continue
    routes = {}
    for row_idx in range(min(20, w.shape[0])):
        probe = w[row_idx, :128].astype(np.float32)
        t = torch.from_numpy(probe[np.newaxis, :])
        with torch.no_grad():
            core = t - t.mean(dim=-1, keepdim=True)
            gate_g = router._get_gate(1)
            idx, _, _ = gate_g.encode_tokens(core)
        routes[row_idx] = idx[0].item()
    unique = len(set(routes.values()))
    print(f"  {wn}: {w.shape[0]} rows, first 20 rows -> {unique} unique idx values")

print("\nDone")
