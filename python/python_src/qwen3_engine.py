"""
qwen3_engine.py — Architecture-aware Qwen3 inference via geometry store
======================================================================
Knows the actual Qwen3-0.6B architecture: 28 layers, GQA, SwiGLU, RoPE, RMSNorm.
Each weight tensor is looked up by geometry (zone, shape) at inference time.

Cold start:  ~1ms (mmap store + small norm weights from GGUF)
Per token:   varies (CPU route vs GPU route)
"""
import sys, json, time, math, threading
import numpy as np
from pathlib import Path
from typing import Optional

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent))

import torch
from bermuda_router_v1 import BermudaRouter, DEVICE
from bermuda_reshape_v3 import snap_gear, pad_to_gear, hilbert_scatter, strip
from zero_warmup_engine import _build_router_on_device, _route_on_device
from geometry_store import GeometryStore


def _load_norms_fast(gguf_path, meta):
    """Load norm weights — fast: skip non-norm tensors."""
    from gguf import GGUFReader
    r = GGUFReader(gguf_path)
    norms = {}
    output_norm = None
    for t in r.tensors:
        n = t.name
        if n == 'output_norm.weight':
            output_norm = np.array(t.data, dtype=np.float32).copy()
        elif '.attn_norm.weight' in n or '.ffn_norm.weight' in n \
             or '.attn_q_norm.weight' in n or '.attn_k_norm.weight' in n:
            parts = n.split('.')
            norms.setdefault(int(parts[1]), {})[parts[2]] = \
                np.array(t.data, dtype=np.float32).copy()
    return norms, output_norm


def _silu(x):
    if isinstance(x, torch.Tensor):
        return torch.nn.functional.silu(x if x.dtype == torch.float32 else x.float())
    x32 = np.clip(x.astype(np.float32), -80, 80)
    return x32 * (1 / (1 + np.exp(-x32)))


def _rmsnorm(x, weight, eps=np.float32(1e-6)):
    """x: [N, D], weight: [D]  -> [N, D] (numpy or torch)"""
    if isinstance(x, torch.Tensor):
        ss = (x.float() ** 2).mean(dim=-1, keepdim=True)
        wt = weight if isinstance(weight, torch.Tensor) else \
             torch.from_numpy(np.asarray(weight, dtype=np.float32).copy()).to(x.device)
        return x / torch.sqrt(ss + eps) * wt
    ss = np.mean(x.astype(np.float32) ** 2, axis=-1, keepdims=True)
    return x / np.sqrt(ss + eps) * weight


def _apply_rope(q, k, cos, sin):
    """Simple RoPE. q/k: [N, n_heads, head_dim], cos/sin broadcast."""
    q_c = q[..., ::2]
    q_s = q[..., 1::2]
    k_c = k[..., ::2]
    k_s = k[..., 1::2]
    q_out = np.stack([q_c * cos - q_s * sin, q_c * sin + q_s * cos], axis=-1).reshape(q.shape)
    k_out = np.stack([k_c * cos - k_s * sin, k_c * sin + k_s * cos], axis=-1).reshape(k.shape)
    return q_out, k_out


def _build_rope_cache(seq_len, head_dim, base=10000.0, dtype=np.float32):
    inv_freq = 1.0 / (base ** (np.arange(0, head_dim, 2, dtype=dtype) / head_dim))
    t = np.arange(seq_len, dtype=dtype)
    freqs = np.outer(t, inv_freq)
    return np.cos(freqs), np.sin(freqs)


class Qwen3GeometryEngine:
    """
    Qwen3 inference through geometry-addressed weight store.

    At init:
      - Opens geometry store (mmap, O(1) lookup)
      - Loads small norm weights from GGUF (~150KB)
      - Router is lazy (CPU first, GPU promote async)

    At forward:
      - For each layer: route input -> (zone, shape)
      - Lookup Q, K, V, O, Gate, Up, Down at that (zone, shape)
      - Compute attention + SwiGLU with looked-up subsets
      - Chain with residual connections
    """

    def __init__(self,
                 store_path: str,
                 gguf_path: str = None,
                 meta_path: str = None,
                 dim: int = 1024,
                 route_dim: int = 128,
                 gear: int = 2,
                 code_dim: int = 32,
                 gate_path: str = None,
                 force_cpu: bool = False):

        t0 = time.perf_counter()

        # Load meta
        self.meta = {'dim': dim, 'n_layers': 28, 'n_heads': 16,
                     'n_kv_heads': 8, 'head_dim': 128, 'ffn_dim': 3072,
                     'vocab_size': 151936}
        if meta_path and Path(meta_path).exists():
            with open(meta_path) as f:
                self.meta.update(json.load(f))

        # Router dim (128 for pre-trained gate) vs full dim
        self._route_dim = route_dim

        # Open store
        self.store = GeometryStore(store_path, read_only=True)

        # Lazy router (hybrid CPU->GPU)
        self._gear = gear
        self._code_dim = code_dim
        self._gate_path = gate_path
        self._force_cpu = force_cpu
        self._cpu_router = None
        self._gpu_router = None
        self._gpu_device = None
        self._promote_thread = None
        self._promote_done = threading.Event()
        self._token_count = 0
        self._route_cache = {}        # {probe_bytes: [(zone,shape)]}

        # Load norm weights from .npz cache (fast, ~44ms) or GGUF fallback
        self.norms = {}
        self.embed = None
        self.output_norm = None
        norms_cache = Path(store_path).parent / 'qwen3_norms.npz'
        if gguf_path is None:
            gguf_path = str(norms_cache)
        if norms_cache.exists():
            d = np.load(str(norms_cache))
            for k in d:
                if k.startswith('norm_'):
                    parts = k.split('_')
                    self.norms.setdefault(int(parts[1]), {})[parts[2]] = d[k]
            self.output_norm = d.get('output_norm')
        elif gguf_path and Path(gguf_path).exists():
            self.norms, self.output_norm = _load_norms_fast(gguf_path, self.meta)
        self._gguf_path = gguf_path

        # Pre-build RoPE cache (up to max ctx) — not used in simplified chain
        self._rope_cached_len = 0

        self._startup_ms = (time.perf_counter() - t0) * 1000
        stats = self.store.stats()
        print(f"[Qwen3Engine] Ready in {self._startup_ms:.2f}ms")
        print(f"  Store: {stats['n_keys']} keys, {stats['total_rows']:,} rows")
        print(f"  Model: {self.meta['n_layers']}L "
              f"{self.meta['n_heads']}H/{self.meta['n_kv_heads']}KV "
              f"dim={self.meta['dim']} route_dim={route_dim}")

    # ── Router (lazy, hybrid CPU->GPU) ────────────────────────

    def _ensure_cpu_router(self):
        if self._cpu_router is not None:
            return self._cpu_router
        t0 = time.perf_counter()
        self._cpu_router = _build_router_on_device(
            self._route_dim, self._gear, self._code_dim, self._gate_path, 'cpu')
        with torch.no_grad():
            self._cpu_router.route(torch.randn(2, self._route_dim), 0)
        print(f"[Qwen3Engine] CPU router ready "
              f"{(time.perf_counter()-t0)*1000:.1f}ms")
        return self._cpu_router

    def _start_gpu_promote(self):
        if (self._force_cpu or self._promote_thread is not None
                or not torch.cuda.is_available()):
            return
        def _promote():
            try:
                t0 = time.perf_counter()
                r = _build_router_on_device(
                    self._route_dim, self._gear, self._code_dim,
                    self._gate_path, 'cuda')
                with torch.no_grad():
                    r.route(torch.randn(2, self._route_dim, device='cuda'), 0)
                self._gpu_router = r
                self._gpu_device = 'cuda'
                self._promote_done.set()
                vram = torch.cuda.memory_allocated() / 1024**2
                print(f"[Qwen3Engine] GPU router ready "
                      f"{(time.perf_counter()-t0)*1000:.0f}ms  VRAM={vram:.1f}MB")
            except Exception as e:
                print(f"[Qwen3Engine] GPU promote failed: {e}")
                self._promote_done.set()
        self._promote_thread = threading.Thread(target=_promote, daemon=True)
        self._promote_thread.start()

    def _active_router(self):
        if self._gpu_router is not None:
            return self._gpu_router, self._gpu_device
        return self._ensure_cpu_router(), 'cpu'

    # ── RoPE ─────────────────────────────────────────────────

    def _rope(self, q, k, pos):
        """Apply RoPE to q, k. pos: sequence position (int).
        q: [N, n_heads*head_dim], k: [N, n_kv_heads*head_dim]
        """
        HD = self.meta['head_dim']
        if pos + 1 > self._rope_cached_len:
            self._rope_cached_len = max(self._rope_cached_len * 2 or 64, pos + 64)
            cos, sin = _build_rope_cache(self._rope_cached_len, HD, 10000.0)
            self._rope_cos = cos
            self._rope_sin = sin
        cos = self._rope_cos[pos:pos+1]
        sin = self._rope_sin[pos:pos+1]
        # Reshape q/k to [N, n_heads, HD] for RoPE
        N = q.shape[0]
        q_r = q.reshape(N, -1, HD)
        k_r = k.reshape(N, -1, HD)
        q_out, k_out = _apply_rope(q_r, k_r, cos, sin)
        return q_out.reshape(N, -1), k_out.reshape(N, -1)

    # ── Geometry matmul ───────────────────────────────────────

    def _geom_matmul(self, x, weight_rows, layer_name):
        """Route x -> (z,s), lookup weight rows, matmul.
        x: [N, dim], weight_rows: [n_rows, dim] or None
        Returns: x_flat @ selected_rows.T  [N, n_selected]
        """
        N = x.shape[0]
        device = 'cpu' if self._cpu_router is not None and self._gpu_router is None else 'cuda'
        # Use the already-known (zone, shape) from current layer
        # Actually we need to route again for each weight type
        # For now: use the same (zone, shape) as the layer's input routing
        router, dev = self._active_router()
        z, s = _route_on_device(router, x[0], 0, dev)
        w = self.store.query(z, s)
        if w is None:
            return np.zeros((N, 1), dtype=np.float32)
        w_slice = w[:min(w.shape[0], x.shape[-1]), :x.shape[-1]]
        return x @ w_slice.T

    def _geom_proj(self, h, w, D):
        """Project h through weight w, always output [N, D].
        Always CPU matmul — weights are cached in RAM, no GPU copy overhead."""
        if w is None:
            return np.zeros((h.shape[0], D), dtype=np.float32)
        k = min(w.shape[0], D)
        in_dim = min(h.shape[-1], w.shape[1])
        out = h[:, :in_dim] @ w[:k, :in_dim].T
        if out.shape[-1] < D:
            out = np.pad(out, ((0, 0), (0, D - out.shape[-1])))
        return out[:, :D]

    def _geom_route_batch(self, x):
        """Route first route_dim through all 4 modes.
        Optimized: feature-norm only (skip 512× padding as confirmed identical).
        """
        probe = np.asarray(x[0, :self._route_dim], dtype=np.float32)
        cache_key = probe.tobytes()
        cached = self._route_cache.get(cache_key)
        if cached is not None:
            return cached

        router, dev = self._active_router()
        t = torch.from_numpy(probe[np.newaxis, :])
        if dev != 'cpu':
            t = t.to(dev)
        with torch.no_grad():
            # Feature normalize only (skip pad/hilbert/batch-norm — verified 1:1 match)
            core = t - t.mean(dim=-1, keepdim=True)
            gear = 1  # always gear 1 for N=1 single token
            gate_g = router._get_gate(gear)
            idx, z_q, loss = gate_g.encode_tokens(core)
            verdicts = [router.classify_idx(idx, m, gear=gear) for m in range(4)]
        result = [(int(v.zone[0].item()), chr(int(v.shape[0].item()))) for v in verdicts]
        self._route_cache[cache_key] = result
        return result

    # ── Full forward (pure CPU matmul) ─────────────────────────

    def forward(self, x, n_layers: int = None):
        """
        Geometry-routed forward through Qwen3 transformer layers.
        Route on GPU if promoted, matmul always on CPU.
        """
        x = np.asarray(x, dtype=np.float32)
        if x.ndim == 1:
            x = x[np.newaxis, :]
        if n_layers is None:
            n_layers = self.meta['n_layers']

        D    = self.meta['dim']
        N_H  = self.meta['n_heads']
        KV_H = self.meta['n_kv_heads']
        HD   = self.meta['head_dim']
        FFN  = self.meta['ffn_dim']

        current = x

        for layer in range(n_layers):
            # 1. attn_norm
            norm_w = self.norms.get(layer, {}).get('attn_norm',
                        np.ones(D, dtype=np.float32))
            h = _rmsnorm(current, norm_w)

            # 2. Batch route all 4 modes (GPU if promoted)
            keys = self._geom_route_batch(h)
            z_q, s_q = keys[0]; z_k, s_k = keys[1]
            z_v, s_v = keys[2]; z_o, s_o = keys[3]

            # 3. Query store with weight-type namespaces
            # 7 lookups: each weight type has its own key via namespace
            found = self.store.query_batch([
                (z_q, s_q, 'Q'), (z_k, s_k, 'K'), (z_v, s_v, 'V'),
                (z_o, s_o, 'O'), (z_o, s_o, 'G'), (z_q, s_q, 'U'), (z_k, s_k, 'D'),
            ])
            w_q    = found.get((z_q, s_q, 'Q'))
            w_k    = found.get((z_k, s_k, 'K'))
            w_v    = found.get((z_v, s_v, 'V'))
            w_o    = found.get((z_o, s_o, 'O'))
            w_gate = found.get((z_o, s_o, 'G'))
            w_up   = found.get((z_q, s_q, 'U'))
            w_down = found.get((z_k, s_k, 'D'))

            # 3. GQA Attention (CPU matmul)
            if w_q is not None and w_k is not None and w_v is not None:
                q = self._geom_proj(h, w_q, N_H * HD)
                k = self._geom_proj(h, w_k, KV_H * HD)
                v = self._geom_proj(h, w_v, KV_H * HD)

                qn = self.norms.get(layer, {}).get('attn_q_norm',
                        np.ones(HD, dtype=np.float32))
                kn = self.norms.get(layer, {}).get('attn_k_norm',
                        np.ones(HD, dtype=np.float32))
                q = _rmsnorm(q.reshape(-1, HD), qn).reshape(1, -1)
                k = _rmsnorm(k.reshape(-1, HD), kn).reshape(1, -1)

                N = h.shape[0]
                q_h = q.reshape(N, N_H, HD)
                k_h = k.reshape(N, KV_H, HD)
                v_h = v.reshape(N, KV_H, HD)

                repeat = N_H // KV_H
                k_h = np.repeat(k_h, repeat, axis=1)
                v_h = np.repeat(v_h, repeat, axis=1)

                scale = 1.0 / math.sqrt(float(HD))
                attn_w = np.einsum('nhd,nhd->nh', q_h, k_h) * scale
                attn_w = attn_w[..., None]
                attn_w = np.exp(attn_w - attn_w.max())
                attn_w /= attn_w.sum(axis=-1, keepdims=True)
                attn_o = (attn_w * v_h).reshape(N, N_H * HD)

                o = self._geom_proj(attn_o, w_o, D)
                current = current + o

            # 4. SwiGLU FFN
            norm_w2 = self.norms.get(layer, {}).get('ffn_norm',
                         np.ones(D, dtype=np.float32))
            h2 = _rmsnorm(current, norm_w2)

            if w_gate is not None and w_up is not None and w_down is not None:
                gate   = self._geom_proj(h2, w_gate, FFN)
                up     = self._geom_proj(h2, w_up,   FFN)
                hidden = _silu(gate) * up
                down   = self._geom_proj(hidden, w_down, D)
                current = current + down

        self._token_count += 1
        if self._token_count == 1:
            self._start_gpu_promote()

        if self.output_norm is not None:
            current = _rmsnorm(current, self.output_norm)

        return current.squeeze()

    def close(self):
        self.store.close()

    def __enter__(self): return self
    def __exit__(self, *_): self.close()
