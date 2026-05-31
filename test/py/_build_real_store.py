"""
Build geometry store from GGUF with weight-type namespaces.
Each weight tensor routed through its native mode only:
  attn_q/ffn_up   → mode0 (ORBITAL)  → ns='Q'/'U'
  attn_k/ffn_down → mode1 (CHIRAL)   → ns='K'/'D'
  attn_v          → mode2 (CROSS)    → ns='V'
  attn_o/ffn_gate → mode3 (HUB)     → ns='O'/'G'
"""
import sys, torch, time, numpy as np
from pathlib import Path
sys.path.insert(0, str(Path('python_src')))
sys.path.insert(0, str(Path('.')))

from gguf import GGUFReader
from bermuda_router_v1 import BermudaRouter, DEVICE
from geometry_store import GeometryStore

GGUF = r'I:\Vault\models\Qwen3-0.6B-Q8_0.gguf'
DIM = 1024
ROUTE_DIM = 128
STORE = 'build/qwen_geom_v3'
GATE = 'build/bermuda_gate_g2_d128.pt'

# Native mode + namespace per weight type
TYPE_CONFIG = {
    'attn_q':      (0, 'Q'),
    'attn_k':      (1, 'K'),
    'attn_v':      (2, 'V'),
    'attn_o':      (3, 'O'),
    'ffn_gate':    (3, 'G'),
    'ffn_up':      (0, 'U'),
    'ffn_down':    (1, 'D'),
}

def class_for_tensor(name):
    if '_norm' in name:
        return None, None
    for prefix, (mode, ns) in TYPE_CONFIG.items():
        if prefix in name:
            return mode, ns
    return None, None

print('[1] Loading GGUF + gate...')
r = GGUFReader(GGUF)
# Collect all weight tensors matching a known type
tensors = []
for t in r.tensors:
    mode, ns = class_for_tensor(t.name)
    if mode is not None:
        tensors.append((t, mode, ns, t.name))
tensors.sort(key=lambda x: x[0].name)  # process in name order
print(f'  {len(tensors)} weight tensors found')

router = BermudaRouter(dim=ROUTE_DIM, gear=2, code_dim=32)
if Path(GATE).exists():
    sd = torch.load(GATE, map_location=DEVICE, weights_only=True)
    router.gate.load_state_dict(sd)
    router.gate.eval()
    print('  Gate loaded')

_ = router.route(torch.randn(1, ROUTE_DIM, device=DEVICE), 0)

n_blocks_needed = (DIM + 31) // 32
n_blocks_route  = (ROUTE_DIM + 31) // 32
store = GeometryStore(STORE, read_only=False)
total_rows = 0
t0 = time.time()

for ti, (t, mode, ns, tname) in enumerate(tensors):
    M = int(t.data.shape[0])
    B = int(t.data.shape[1]) if len(t.data.shape) > 1 else 1
    n_blocks = max(1, B // 34)
    blk = t.data.reshape(M, n_blocks, 34)

    # Full dim for storage
    blk_store = blk[:, :n_blocks_needed, :]
    i8_store = blk_store[:, :, :32].astype(np.int8).astype(np.float32)
    flat_store = i8_store.reshape(M, n_blocks_needed * 32)[:, :DIM]

    # First 128 for routing
    blk_route = blk[:, :n_blocks_route, :]
    i8_route = blk_route[:, :, :32].astype(np.int8).astype(np.float32)
    flat_route = i8_route.reshape(M, n_blocks_route * 32)[:, :ROUTE_DIM]

    x_store = torch.from_numpy(flat_store).float()
    x_route = torch.from_numpy(flat_route).float()

    for bi in range(0, min(M, 512), 64):
        batch_s = x_store[bi:bi+64].to(DEVICE)
        batch_r = x_route[bi:bi+64].to(DEVICE)
        if batch_r.shape[0] < 2:
            continue

        v = router.route(batch_r, mode)
        n = v.n_tokens
        zones_cpu = v.zone[:n].cpu().numpy()
        shapes_cpu = v.shape[:n].cpu().numpy()
        vals = batch_s[:n].detach().cpu().numpy()
        for ei in range(n):
            store.index(int(zones_cpu[ei]), chr(int(shapes_cpu[ei])), vals[ei], ns=ns)
            total_rows += 1
        del v
        del batch_s, batch_r

    if (ti + 1) % 10 == 0 or ti == len(tensors) - 1:
        print(f'  [{ti+1}/{len(tensors)}] {tname[-40:]:40s} mode={mode} ns={ns} rows={total_rows:,}')

print(f'  Indexed {total_rows:,} rows in {time.time()-t0:.1f}s')
store.flush()
store.close()

# Verify
print()
print('[2] Verification...')
with GeometryStore(STORE, read_only=True) as s:
    s.print_stats()
    # Sample queries per namespace
    for ns in ['Q', 'K', 'V', 'O', 'G', 'U', 'D']:
        hits = 0
        for z in range(12):
            for sh in ['I', 'O', 'T', 'S', 'Z', 'L']:
                if s.has(z, sh, ns=ns):
                    hits += 1
        print(f'  ns={ns}: {hits} keys present')
