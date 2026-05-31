"""Export raw quantized bytes from GGUF for Q/G/U/D tensors."""
import os, sys, time
import numpy as np
from gguf import GGUFReader

GGUF_PATH = r'I:\model\Qwen3.5-0.8B-Q4_0.gguf'
OUT_DIR   = r'I:\FGLS_new\collection\build\qwen35_geom_tensors_raw'

TENSOR_NS = {
    'attn_qkv.weight': 'Q',
    'ffn_gate.weight': 'G',
    'ffn_up.weight':   'U',
    'ffn_down.weight': 'D',
}

os.makedirs(OUT_DIR, exist_ok=True)

reader = GGUFReader(GGUF_PATH)
count, total_bytes = 0, 0

for t in reader.tensors:
    name = t.name
    ns = None
    for pattern, ns_val in TENSOR_NS.items():
        if name.endswith(pattern):
            ns = ns_val
            break
    if ns is None:
        continue

    raw = np.array(t.data, dtype=np.uint8).flatten()
    safe_name = name.replace('/', '_').replace('\\', '_')
    out_path = os.path.join(OUT_DIR, f'{safe_name}.qdat')
    raw.tofile(out_path)
    count += 1
    total_bytes += raw.size
    print(f'  [{count:2d}] {name:45s} -> {raw.size/1024:8.1f} KB')

print(f'\nExported {count} tensors, {total_bytes/1024/1024:.1f} MB to {OUT_DIR}')
