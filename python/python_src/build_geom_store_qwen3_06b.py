"""
Build Geometry Store for Qwen3-0.6B (F16) from GGUF.
"""

import os, sys, time, gc, argparse
from math import gcd
import numpy as np

p = argparse.ArgumentParser()
p.add_argument("--gguf", default="Qwen3-Embedding-0.6B.F16.gguf", help="GGUF model path")
p.add_argument("--out", default="build/", help="output directory")
p.add_argument("--model-name", default="qwen3-0.6b", help="model identifier for geo_key")
args = p.parse_args()

GGUF_PATH  = args.gguf
STORE_PATH = os.path.join(args.out, 'qwen3_06b_geom')
GATE_DIR   = os.path.join(args.out, 'gate_export')
EXPORT_DIR = os.environ.get('TENSOR_EXPORT_DIR', '')

STRIDE     = 37
N_ZONES    = 12

def _gear_walk_len(slots):
    wl = slots
    while wl % 12 != 0 or gcd(37, wl) != 1:
        wl += 1
    return wl

WALK_LEN = _gear_walk_len(1024)
FACE_SZ  = WALK_LEN // N_ZONES

TENSOR_NS = {
    'attn_q.weight':      'Q',
    'attn_k.weight':      'K',
    'attn_v.weight':      'V',
    'attn_output.weight': 'O',
    'ffn_gate.weight':    'G',
    'ffn_up.weight':      'U',
    'ffn_down.weight':    'D',
}

MAX_ROWS = 4096

def dequant_q8_0_batch(blocks, n_cols):
    """Q8_0: 34B/block = 2B f16 scale + 32 int8 quants. val = quant * scale"""
    B = blocks.shape[0]
    scale_bytes = blocks[:, 0:2].reshape(B, 2)
    scales = scale_bytes.view(np.dtype('<f2')).reshape(-1, 1).astype(np.float32)
    scales = np.where(np.isfinite(scales), scales, 1.0)
    quants = blocks[:, 2:34].astype(np.float32)
    out = quants * scales
    return out[:, :n_cols]

def load_and_dequant(t, max_rows=None):
    from gguf import GGMLQuantizationType
    m = int(t.shape[0])
    n = int(t.shape[1])
    if max_rows and m > max_rows:
        m = max_rows

    qtype = t.tensor_type
    if qtype == GGMLQuantizationType.Q8_0:
        block_bytes = 34
        dequant_fn = dequant_q8_0_batch
    else:
        print(f'  SKIP: unsupported quant type {qtype}')
        return None

    blocks_per_row = (n + 31) // 32
    bytes_per_row = blocks_per_row * block_bytes

    data = np.array(t.data, dtype=np.uint8).flatten()
    total_rows_data = data.size // bytes_per_row
    m = min(m, total_rows_data)
    if m == 0:
        return None

    n_blocks = m * blocks_per_row
    needed = n_blocks * block_bytes
    if data.size < needed:
        return None
    blocks = data[:needed].reshape(n_blocks, block_bytes)
    result = dequant_fn(blocks, n)
    return result.reshape(m, n)


class GateFromExport:
    def __init__(self, gate_dir):
        def load_f32(name):
            return np.fromfile(os.path.join(gate_dir, name), dtype=np.float32)
        w0 = load_f32('encoder_0_weight.f32').reshape(256, 128)
        b0 = load_f32('encoder_0_bias.f32')
        w1 = load_f32('encoder_1_weight.f32')
        b1 = load_f32('encoder_1_bias.f32')
        w3 = load_f32('encoder_3_weight.f32').reshape(32, 256)
        b3 = load_f32('encoder_3_bias.f32')
        codes = load_f32('codebook_codes.f32').reshape(1024, 32)
        self.enc_w = w0
        self.enc_b = b0
        self.ln_w  = w1
        self.ln_b  = b1
        self.out_w = w3
        self.out_b = b3
        self.codes = codes

    def encode(self, x):
        h = x @ self.enc_w.T + self.enc_b
        mu = h.mean(axis=1, keepdims=True)
        var = h.var(axis=1, keepdims=True)
        h = (h - mu) / np.sqrt(np.maximum(var, 1e-10))
        h = h * self.ln_w + self.ln_b
        h = 0.5 * h * (1 + np.tanh(np.sqrt(2/np.pi) * (h + 0.044715 * h**3)))
        h = h @ self.out_w.T + self.out_b
        dist = np.sum((h[:, np.newaxis, :] - self.codes[np.newaxis, :, :])**2, axis=2)
        return dist.argmin(axis=1)

    @staticmethod
    def classify_idx(idx):
        enc = (idx * STRIDE) % WALK_LEN
        zone = np.array(enc // FACE_SZ, dtype=np.int64)
        return zone, 'I'


def main():
    t_start = time.time()
    print('=== Build Geometry Store for Qwen3-0.6B (Q8_0) ===')
    print(f'  GGUF: {GGUF_PATH}')
    print(f'  Store: {STORE_PATH}')
    print(f'  Gear-2 geometry: walk_len={WALK_LEN}, face_sz={FACE_SZ}')

    print('\n[1/4] Loading gate from export...')
    gate = GateFromExport(GATE_DIR)
    print(f'  encoder: {gate.enc_w.shape} -> LN -> GELU -> {gate.out_w.shape}')
    print(f'  codebook: {gate.codes.shape}')

    print('\n[2/4] Opening GGUF...')
    from gguf import GGUFReader
    t0 = time.time()
    reader = GGUFReader(GGUF_PATH)
    print(f'  Tensors: {len(reader.tensors)} ({time.time()-t0:.1f}s)')

    print('\n[3/4] Processing weight tensors...')
    from geometry_store import GeometryStore

    store = GeometryStore(STORE_PATH, read_only=False)
    total_rows = 0
    total_tensors = 0

    zones_counts = np.zeros(N_ZONES, dtype=np.int64)
    for i in range(1024):
        z = (i * STRIDE) % WALK_LEN // FACE_SZ
        zones_counts[z] += 1
    print(f'  Codebook zone distribution (all 1024 slots):')
    for z in range(N_ZONES):
        print(f'    zone {z}: {zones_counts[z]} slots')

    for ti, t in enumerate(reader.tensors):
        name = t.name
        ns = None
        for pattern, ns_name in TENSOR_NS.items():
            if name.endswith(pattern):
                ns = ns_name
                break
        if ns is None:
            continue

        if len(t.shape) < 2:
            continue

        m = int(t.shape[0])
        n = int(t.shape[1])

        t0 = time.time()
        f32_data = load_and_dequant(t, max_rows=MAX_ROWS if m > MAX_ROWS else None)
        if f32_data is None:
            continue
        actual_rows = f32_data.shape[0]

        if n >= 1024:
            pooled = f32_data[:, :1024].reshape(actual_rows, 128, 8).mean(axis=2)
        else:
            pooled = np.zeros((actual_rows, 128), dtype=np.float32)
            k = min(n, 128)
            pooled[:, :k] = f32_data[:, :k]

        code_idxs = gate.encode(pooled)
        zones, shapes = gate.classify_idx(code_idxs)

        if EXPORT_DIR:
            safe_name = name.replace('/', '_').replace('\\', '_')
            raw_dir = EXPORT_DIR.replace('geom_tensors', 'geom_tensors_raw')
            if not os.path.exists(raw_dir):
                os.makedirs(raw_dir, exist_ok=True)
            raw_path = os.path.join(raw_dir, f'{safe_name}.qdat')
            raw_bytes = np.array(t.data, dtype=np.uint8).flatten()
            raw_bytes.tofile(raw_path)

        for ri in range(actual_rows):
            store.index(int(zones[ri]), 'I', f32_data[ri], ns=ns)

        total_rows += actual_rows
        total_tensors += 1

        dur = time.time() - t0
        elapsed = time.time() - t_start
        if total_tensors <= 5 or total_tensors % 5 == 0:
            zone_dist = np.bincount(zones, minlength=N_ZONES)
            dist_str = ' '.join(f'{zone_dist[z]}' for z in range(N_ZONES))
            print(f'  [{total_tensors:3d}] {name:40s} {m:5d}x{n:<5d} '
                  f'-> {actual_rows:4d} rows ({dur:.2f}s) zones=[{dist_str}] '
                  f'total_rows={total_rows:,}')

        if total_rows % 20000 == 0:
            store.flush()
            gc.collect()

    print('\n[4/4] Finalizing store...')
    store.flush()
    store.close()

    elapsed = time.time() - t_start
    print(f'\n=== Done in {elapsed:.1f}s ===')
    print(f'  Tensors processed: {total_tensors}')
    print(f'  Total rows indexed: {total_rows:,}')

    with GeometryStore(STORE_PATH, read_only=True) as s:
        s.print_stats()


if __name__ == '__main__':
    main()
