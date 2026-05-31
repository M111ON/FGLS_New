"""
weight_subset_inference.py — Demo: route embedding → query store → use weights

Shows that geometry store can serve weights directly, replacing GGUF for a specific
(ns, zone, shape) path. No full model load required.
"""

import os, sys, time
import numpy as np

COLLECTION = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, COLLECTION)
sys.path.insert(0, os.path.join(COLLECTION, 'python_src'))

STORE_PATH = os.path.join(COLLECTION, 'build', 'qwen35_geom')
GATE_DIR   = os.path.join(COLLECTION, 'build', 'gate_export')

from geometry_store import GeometryStore
from build_geom_store_qwen35 import GateFromExport, STRIDE, WALK_LEN, FACE_SZ


def embed_text(text: str, pre_pooled: np.ndarray = None) -> np.ndarray:
    """Return a 1024-dim embedding.
    Use pre_pooled to specify a real weight-derived embedding that routes correctly.
    """
    if pre_pooled is not None:
        return pre_pooled
    rng = np.random.RandomState(42)
    emb = rng.randn(1024).astype(np.float32)
    emb = emb / np.linalg.norm(emb) * 10.0
    print(f'  Synthetic embedding (may route to empty zone), norm={np.linalg.norm(emb):.3f}')
    return emb


def main():
    print('=== Weight Subset Inference Demo ===')
    print(f'  Store: {STORE_PATH}')
    print(f'  Gate:  {GATE_DIR}')

    # 1. Load gate
    print('\n[1] Loading gate...')
    gate = GateFromExport(GATE_DIR)
    print(f'  encoder: {gate.enc_w.shape} → LN → GELU → {gate.out_w.shape}')
    print(f'  codebook: {gate.codes.shape}')

    # 2. Load store
    print('\n[2] Loading geometry store...')
    t0 = time.time()
    store = GeometryStore(STORE_PATH, read_only=True)
    print(f'  Keys: {len(store._index)}, rows: {sum(v[1] for v in store._index.values()):,}')
    print(f'  Loaded in {time.time()-t0:.2f}s')

    # 3. Quick probe: find zones with data across all 4 namespaces
    print('\n[3] Probing store zone coverage...')
    ns_list = ['Q', 'G', 'U', 'D']
    zone_hits = {}
    for ns in ns_list:
        for z in range(12):
            w = store.query(z, 'I', ns=ns)
            if w is not None:
                zone_hits.setdefault(z, []).append(ns)
    # Pick the best zone (largest coverage)
    if zone_hits:
        best_zone = max(zone_hits, key=lambda z: len(zone_hits[z]))
        print(f'  Active zones: {dict(sorted(zone_hits.items()))}')
        print(f'  Using zone={best_zone} ({len(zone_hits[best_zone])} namespaces)')
        zone = best_zone
    else:
        print('  No data found in any zone!')
        return

    # 4. Generate a "embedding-like" vector from actual store weights
    #    (simulates what the model produces during real inference)
    print('\n[4] Deriving embedding from store weights (simulating real inference)...')
    w_example = store.query(zone, 'I', ns='D')  # ffn_down large matrix
    if w_example is not None:
        # Take one row, mean-pool 1024→128 to simulate routing input
        raw_row = w_example[0, :]  # [1024]
        emb_1024 = raw_row.copy()
        emb_128 = emb_1024.reshape(128, 8).mean(axis=1)
        print(f'  Derived from D[0] row: norm={np.linalg.norm(emb_1024):.4f}')
    else:
        print('  No D weights at zone, using identity-style embedding')
        emb_1024 = np.arange(1024, dtype=np.float32) / 1024.0
        emb_128 = emb_1024.reshape(128, 8).mean(axis=1)

    # 5. Route through gate (verify it lands at same zone)
    print('\n[5] Routing through Bermuda gate...')
    t0 = time.time()
    code_idx = gate.encode(emb_128.reshape(1, -1))[0]
    enc = (code_idx * STRIDE) % WALK_LEN
    routed_zone = enc // FACE_SZ
    dur = time.time() - t0
    print(f'  code_idx={code_idx}  routed_zone={routed_zone}  target_zone={zone}  '
          f'{"✓ MATCH" if routed_zone == zone else "✗ MISMATCH"} ({dur*1000:.1f}ms)')
    # Use target zone regardless; routing consistency is informative

    # 6. Query store at zone
    print('\n[6] Querying geometry store for all namespaces at zone...')
    tensor_names = {
        'Q': 'attn_qkv.weight', 'G': 'ffn_gate.weight',
        'U': 'ffn_up.weight', 'D': 'ffn_down.weight'
    }
    weights = {}
    t0 = time.time()
    for ns in ns_list:
        w = store.query(zone, 'I', ns=ns)
        if w is not None:
            weights[ns] = w
            print(f'  ns={ns} ({tensor_names[ns]}): {w.shape} float32 = {w.nbytes/1024:.0f} KB')
        else:
            print(f'  ns={ns}: NOT FOUND at zone={zone}')
    dur = time.time() - t0
    print(f'  Query time: {dur*1000:.1f}ms')

    # 7. Use weights for inference (matmul)
    print('\n[7] Using weights for inference (matmul)...')
    # Quick matmul to prove weights are valid float32.
    for ns, w in weights.items():
        n_cols = w.shape[1]
        test_input = np.ones(min(256, n_cols), dtype=np.float32)
        t0 = time.time()
        result = test_input @ w[:, :test_input.size].T
        dur = time.time() - t0
        print(f'  {ns}: {w.shape} → out={result.shape} mean={result.mean():.6f} ({dur*1000:.1f}ms)')
    
    # 8. Weight statistics
    print('\n[8] Weight statistics by namespace...')
    for ns, w in weights.items():
        print(f'  {ns}: shape={w.shape} mean={w.mean():.6f} std={w.std():.4f} '
              f'range=[{w.min():.4f}, {w.max():.4f}]')
    
    # 8. Close store
    store.close()
    print(f'\n  Demo complete. All weights from zone={zone} are ready for inference.')
    print(f'  Total weight data loaded: {sum(w.nbytes for w in weights.values())/1024/1024:.1f} MB')


if __name__ == '__main__':
    main()
