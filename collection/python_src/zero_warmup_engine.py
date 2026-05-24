"""
zero_warmup_engine.py — Zero-Warmup Geometry Inference Engine
=============================================================
Strategy: CPU cold start → async GPU promote after token 1

Timeline:
  t=0ms    engine init  (mmap open only, no torch)
  t=0.4ms  first forward() call arrives
  t=~50ms  lazy CPU router built + first token computed
  t=~55ms  GPU promote starts async (background thread)
  t=~80ms  token 2 computed on CPU (GPU still warming)
  t=~300ms GPU context ready → token 3+ routed on GPU → 170 tok/s

Result:
  first token:  ~50ms   (vs 463ms GPU-eager, vs 297ms CPU-only)
  sustained:    170 tok/s on GPU  (vs 86 tok/s CPU-only)
  VRAM used:    ~1MB (encoder+codebook only, not full model)
"""

import sys, json, time, threading
import numpy as np
from pathlib import Path
from typing import Optional

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent))
sys.path.insert(0, str(_HERE))
sys.path.insert(0, str(_HERE / 'python_src'))

import torch
from bermuda_router_v1 import BermudaRouter, DEVICE
from geometry_store import GeometryStore


# ── Metadata builder ─────────────────────────────────────────

def build_meta(gguf_path: str, meta_out: str):
    try:
        from gguf import GGUFReader
    except ImportError:
        print("[ERROR] pip install gguf"); return
    r = GGUFReader(gguf_path)
    meta = {
        'model': Path(gguf_path).stem,
        'tensors': [{'name': t.name, 'shape': list(t.shape)} for t in r.tensors],
        'n_layers': sum(1 for t in r.tensors if 'attn_q' in t.name),
    }
    with open(meta_out, 'w') as f:
        json.dump(meta, f, indent=2)
    print(f"[build_meta] {len(meta['tensors'])} tensors → {meta_out}")
    return meta


# ── Router factory helpers ────────────────────────────────────

def _build_router_on_device(dim, gear, code_dim, gate_path, device: str):
    """Build BermudaRouter on specified device without triggering DEVICE global."""
    from bermuda_reshape_v3 import BermudaGate, GEAR_TABLE

    gate = BermudaGate(dim, code_dim=code_dim, gear=gear)
    if gate_path and Path(gate_path).exists():
        state = torch.load(gate_path, map_location='cpu', weights_only=True)
        gate.load_state_dict(state)
    gate = gate.to(device)
    gate.eval()

    # Pre-cache both gears: snap_gear(N<1024)=1, snap_gear(N>=1024)=2
    # Without this, first route() builds missing gear on-demand → ~1100ms hit
    other_gear = 1 if gear == 2 else 2
    gate_other = BermudaGate(dim, code_dim=code_dim, gear=other_gear)
    gate_other = gate_other.to(device)
    gate_other.eval()

    router = BermudaRouter.__new__(BermudaRouter)
    router.dim         = dim
    router.gear        = gear
    router.gate        = gate
    router._gate_cache = {gear: gate, other_gear: gate_other}
    router.slots       = GEAR_TABLE[gear]['slots']
    router.walk_len    = GEAR_TABLE[gear]['walk_len']
    router.face_sz     = router.walk_len // 12
    return router


def _route_on_device(router, x_np: np.ndarray, mode: int, device: str):
    """Route numpy array through router on given device. Returns (zone, shape_chr)."""
    t = torch.from_numpy(x_np)
    if device != 'cpu':
        t = t.to(device)
    with torch.no_grad():
        v = router.route(t, mode)
    return int(v.zone[0].item()), chr(int(v.shape[0].item()))


# ── Zero-Warmup Engine ───────────────────────────────────────

class ZeroWarmupEngine:
    """
    Hybrid CPU→GPU inference engine.

    Phase 1 (cold):  CPU router, 0ms CUDA penalty, ~12ms/tok
    Phase 2 (warm):  GPU router promoted async, 170 tok/s sustained

    VRAM cost: ~1MB (gate encoder+codebook only, not full model weights).
    Weight store stays memory-mapped on CPU/disk throughout.
    """

    # Token threshold before attempting GPU promote
    _GPU_PROMOTE_AFTER = 1   # start promote thread after first token

    def __init__(self,
                 store_path: str,
                 dim: int = 128,
                 gear: int = 2,
                 code_dim: int = 32,
                 gate_path: str = None,
                 meta_path: str = None,
                 force_cpu: bool = False):

        t0 = time.perf_counter()

        self.store      = GeometryStore(store_path, read_only=True)
        self.dim        = dim
        self._gear      = gear
        self._code_dim  = code_dim
        self._gate_path = gate_path
        self._force_cpu = force_cpu

        # Router state machine
        self._cpu_router = None          # built on first forward()
        self._gpu_router = None          # built async after token 1
        self._gpu_device = None          # 'cuda' once confirmed available
        self._promote_thread: Optional[threading.Thread] = None
        self._promote_done  = threading.Event()
        self._token_count   = 0

        self.meta = None
        if meta_path and Path(meta_path).exists():
            with open(meta_path) as f:
                self.meta = json.load(f)

        t1 = time.perf_counter()
        self._startup_ms = (t1 - t0) * 1000

        stats = self.store.stats()
        cuda_info = f"cuda={torch.cuda.get_device_name(0)}" \
                    if torch.cuda.is_available() and not force_cpu else "cpu-only"
        print(f"[ZeroWarmupEngine] Ready in {self._startup_ms:.2f}ms  [{cuda_info}]")
        print(f"  Store: {stats['n_keys']} keys, "
              f"{stats['total_rows']:,} rows, {stats['data_kb']} KB")
        if self.meta:
            print(f"  Model: {self.meta.get('model','?')} "
                  f"layers={self.meta.get('n_layers','?')}")

    # ── Lazy CPU router ───────────────────────────────────────

    def _ensure_cpu_router(self):
        if self._cpu_router is not None:
            return self._cpu_router
        t0 = time.perf_counter()
        self._cpu_router = _build_router_on_device(
            self.dim, self._gear, self._code_dim, self._gate_path, 'cpu')
        # Warm call on CPU (no CUDA penalty)
        with torch.no_grad():
            self._cpu_router.route(torch.randn(2, self.dim), 0)
        print(f"[ZeroWarmupEngine] CPU router ready  "
              f"{(time.perf_counter()-t0)*1000:.1f}ms")
        return self._cpu_router

    # ── Async GPU promote ─────────────────────────────────────

    def _start_gpu_promote(self):
        """Kick off background thread to build GPU router while CPU serves tokens."""
        if (self._force_cpu
                or self._promote_thread is not None
                or not torch.cuda.is_available()):
            return

        def _promote():
            try:
                t0 = time.perf_counter()
                # CUDA context init happens here, in background
                dev = 'cuda'
                router = _build_router_on_device(
                    self.dim, self._gear, self._code_dim, self._gate_path, dev)
                # Warm: one GPU route call to JIT the kernel
                with torch.no_grad():
                    router.route(torch.randn(2, self.dim, device=dev), 0)
                self._gpu_router = router
                self._gpu_device = dev
                self._promote_done.set()
                vram_mb = torch.cuda.memory_allocated() / 1024**2
                print(f"\n[ZeroWarmupEngine] GPU router ready  "
                      f"{(time.perf_counter()-t0)*1000:.0f}ms  "
                      f"VRAM={vram_mb:.1f}MB")
            except Exception as e:
                print(f"\n[ZeroWarmupEngine] GPU promote failed: {e} — staying on CPU")
                self._promote_done.set()

        self._promote_thread = threading.Thread(target=_promote, daemon=True)
        self._promote_thread.start()

    # ── Active router (CPU or GPU) ────────────────────────────

    def _active_router(self) -> tuple:
        """Returns (router, device_str) — GPU if ready, else CPU."""
        if self._gpu_router is not None:
            return self._gpu_router, self._gpu_device
        return self._ensure_cpu_router(), 'cpu'

    def prime(self, promote_gpu: bool = False) -> None:
        """
        Warm the CPU router immediately and optionally start GPU promote.

        Useful when a model is about to be accessed by coordinate and the
        first request should not pay the router build cost.
        """
        self._ensure_cpu_router()
        if promote_gpu:
            self._start_gpu_promote()

    def query(self, zone: int, shape: str, ns: str = None):
        """Direct geometry-store lookup without running inference."""
        return self.store.query(zone, shape, ns=ns)

    def query_batch(self, keys: list[tuple]) -> dict:
        """Batch geometry-store lookup without running inference."""
        return self.store.query_batch(keys)

    # ── Core forward ─────────────────────────────────────────

    def forward(self, x: np.ndarray, mode: int = 0,
                n_layers: int = 1) -> Optional[np.ndarray]:
        """
        Geometry-routed forward pass.

        x:        [dim] or [N, dim] float32
        mode:     0=ORBITAL 1=CHIRAL 2=CROSS 3=HUB
        n_layers: chained geometry lookups
        """
        x = np.asarray(x, dtype=np.float32)
        if x.ndim == 1:
            x = x[np.newaxis, :]

        current = x
        timings = []

        for layer in range(n_layers):
            t0 = time.perf_counter()

            # Pick router (GPU if promoted, else CPU)
            router, dev = self._active_router()

            # Route: numpy → geometry key
            z, s = _route_on_device(router, current, mode, dev)

            # O(1) mmap weight lookup (always CPU/RAM — no VRAM)
            weights = self.store.query(z, s)
            if weights is None:
                print(f"[forward] L{layer}: no weights zone={z} shape={s}")
                return None

            t1 = time.perf_counter()

            # Matmul on numpy (CPU) — weights stay mmap'd, no copy
            w = weights[:self.dim, :self.dim] if weights.shape[0] >= self.dim \
                else weights
            out = current @ w.T
            out = out[:, :self.dim] if out.shape[1] >= self.dim \
                else np.pad(out, ((0, 0), (0, self.dim - out.shape[1])))

            current = out.astype(np.float32)
            t2 = time.perf_counter()
            timings.append({
                'layer': layer, 'zone': z, 'shape': s, 'device': dev,
                'n_weights': weights.shape[0],
                'route_ms':  round((t1 - t0) * 1000, 3),
                'matmul_ms': round((t2 - t1) * 1000, 3),
            })

        self._token_count += 1
        self._last_timings = timings

        # Kick off GPU promote after first successful token
        if self._token_count == self._GPU_PROMOTE_AFTER:
            self._start_gpu_promote()

        return current.squeeze()

    def forward_verbose(self, x: np.ndarray, mode: int = 0,
                        n_layers: int = 1) -> Optional[np.ndarray]:
        result = self.forward(x, mode, n_layers)
        if result is not None and hasattr(self, '_last_timings'):
            print(f"\n[ZeroWarmupEngine] Forward trace:")
            total_ms = 0
            for t in self._last_timings:
                ms = t['route_ms'] + t['matmul_ms']
                total_ms += ms
                print(f"  L{t['layer']}: [{t['device']:4s}] "
                      f"zone={t['zone']:2d} shape={t['shape']} "
                      f"w={t['n_weights']:4d}  "
                      f"route={t['route_ms']}ms matmul={t['matmul_ms']}ms")
            print(f"  Total: {total_ms:.2f}ms")
        return result

    def wait_gpu(self, timeout: float = 10.0) -> bool:
        """Block until GPU router is ready (or timeout). Returns True if GPU active."""
        if self._promote_thread is None:
            return False
        self._promote_done.wait(timeout=timeout)
        return self._gpu_router is not None

    def benchmark(self, n_tokens: int = 200, mode: int = 0,
                  wait_gpu_after: int = 5):
        """
        Benchmark with GPU promote mid-run.
        wait_gpu_after: wait for GPU promote after this many tokens.
        """
        print(f"\n[benchmark] {n_tokens} tokens, mode={mode}")
        inputs = np.random.randn(n_tokens, self.dim).astype(np.float32)

        phase_times = {'cpu': [], 'cuda': []}
        t_start = time.perf_counter()

        for i in range(n_tokens):
            # Wait for GPU after threshold (simulates real usage)
            if i == wait_gpu_after and self._promote_thread is not None:
                self._promote_done.wait(timeout=5.0)

            t0 = time.perf_counter()
            out = self.forward(inputs[i], mode)
            t1 = time.perf_counter()

            if out is not None:
                dev = self._last_timings[-1]['device']
                phase_times[dev].append((t1 - t0) * 1000)

        total_s = time.perf_counter() - t_start
        n_cpu  = len(phase_times['cpu'])
        n_cuda = len(phase_times['cuda'])

        print(f"  CPU  tokens: {n_cpu:3d}  "
              f"avg={np.mean(phase_times['cpu']):.1f}ms" if n_cpu else
              f"  CPU  tokens: 0")
        if n_cuda:
            print(f"  GPU  tokens: {n_cuda:3d}  "
                  f"avg={np.mean(phase_times['cuda']):.1f}ms  "
                  f"→ {1000/np.mean(phase_times['cuda']):.0f} tok/s")
        hits = n_cpu + n_cuda
        print(f"  Total: {total_s*1000:.0f}ms  "
              f"overall {hits/total_s:.0f} tok/s")

    @property
    def device(self) -> str:
        return self._gpu_device if self._gpu_router else 'cpu'

    def close(self):
        self.store.close()
        if self._gpu_router and self._gpu_device == 'cuda':
            torch.cuda.empty_cache()

    def __enter__(self): return self
    def __exit__(self, *_): self.close()


# ── Demo ─────────────────────────────────────────────────────

def demo_synthetic(store_path: str = "/tmp/demo_hybrid_store", dim: int = 128):
    from geometry_store import GeometryStore, SHAPES

    print("=" * 60)
    print("Zero-Warmup Engine — CPU→GPU Hybrid Demo")
    print("=" * 60)

    # Build store once
    print("\n[Phase 1] Build geometry store (offline)...")
    with GeometryStore(store_path) as store:
        rng = np.random.default_rng(42)
        for z in range(12):
            for s in SHAPES:
                store.index(z, s, rng.standard_normal((120, dim)).astype('f4'))
        store.flush()

    # Cold start
    print("\n[Phase 2] Cold start...")
    t0 = time.perf_counter()
    engine = ZeroWarmupEngine(store_path=store_path, dim=dim)
    t1 = time.perf_counter()
    print(f"  Engine init: {(t1-t0)*1000:.2f}ms  ← store open only")

    # First token (lazy CPU router builds here)
    print("\n[Phase 3] First token (CPU router lazy build)...")
    x = np.random.randn(dim).astype('f4')
    t2 = time.perf_counter()
    out = engine.forward_verbose(x, mode=0, n_layers=2)
    t3 = time.perf_counter()
    print(f"  First token total: {(t3-t2)*1000:.1f}ms  "
          f"(includes router build)")

    # Benchmark: CPU tokens, then GPU kicks in
    print("\n[Phase 4] Sustained benchmark (GPU promote async)...")
    engine.benchmark(n_tokens=100, wait_gpu_after=3)

    # Summary
    print(f"\n[Summary]")
    print(f"  Startup:     {engine._startup_ms:.2f}ms")
    print(f"  First token: {(t3-t2)*1000:.0f}ms")
    print(f"  Final device: {engine.device}")
    gpu_note = (f"  VRAM: {torch.cuda.memory_allocated()/1024**2:.1f}MB"
                if engine.device == 'cuda' else "  (no GPU / CPU-only mode)")
    print(gpu_note)

    engine.close()


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args or args[0] == "demo":
        demo_synthetic()
    elif args[0] == "build-meta" and len(args) >= 3:
        build_meta(args[1], args[2])
    elif args[0] == "run" and len(args) >= 2:
        store = args[1]
        dim   = int(args[2]) if len(args) > 2 else 128
        gate  = args[3] if len(args) > 3 else None
        with ZeroWarmupEngine(store_path=store, dim=dim, gate_path=gate) as eng:
            eng.benchmark(n_tokens=100)
    else:
        print("Usage:")
        print("  python zero_warmup_engine.py demo")
        print("  python zero_warmup_engine.py build-meta model.gguf meta.json")
        print("  python zero_warmup_engine.py run store_path [dim] [gate.pt]")
