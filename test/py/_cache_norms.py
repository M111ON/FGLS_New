"""
_cache_norms.py — Extract norm weights from Qwen3 GGUF once, cache as .npz.

Usage: python _cache_norms.py <gguf_path> [output_path]

Output: norms_cache.npz containing:
  - norms[L][type]: dict of {layer_idx: {attn_norm/ffn_norm/attn_q_norm/attn_k_norm: ndarray}}
  - output_norm: ndarray [dim]
"""
import sys, json, numpy as np
from pathlib import Path
from gguf import GGUFReader

def cache_norms(gguf_path: str, out_path: str = None):
    gguf_path = str(Path(gguf_path).resolve())
    if out_path is None:
        out_path = str(Path(gguf_path).with_suffix('.norms.npz'))

    print(f"Scanning {gguf_path} ...")
    r = GGUFReader(gguf_path)
    norms = {}
    output_norm = None

    for t in r.tensors:
        n = t.name
        if n == 'output_norm.weight':
            output_norm = np.array(t.data, dtype=np.float32).copy()
            print(f"  output_norm.weight  shape={t.data.shape}")
        elif '.attn_norm.weight' in n or '.ffn_norm.weight' in n \
             or '.attn_q_norm.weight' in n or '.attn_k_norm.weight' in n:
            parts = n.split('.')
            layer_idx = int(parts[1])
            norm_type = parts[2]
            norms.setdefault(layer_idx, {})[norm_type] = \
                np.array(t.data, dtype=np.float32).copy()
            if len(norms) <= 2:
                print(f"  {n}  shape={t.data.shape}")

    # Save as flat arrays
    save = {}
    for lidx in sorted(norms):
        for k, v in norms[lidx].items():
            save[f"norm_{lidx}_{k}"] = v
    if output_norm is not None:
        save['output_norm'] = output_norm
    save['_meta'] = json.dumps({
        'n_layers': len(norms),
        'norm_keys': list(save.keys())
    }).encode()
    np.savez_compressed(out_path, **save)
    print(f"Cached {len(save)-1} arrays to {out_path}")
    return out_path

if __name__ == '__main__':
    gguf = sys.argv[1] if len(sys.argv) > 1 else r'I:\Vault\models\Qwen3-0.6B-Q8_0.gguf'
    out = sys.argv[2] if len(sys.argv) > 2 else None
    cache_norms(gguf, out)
