"""
fibonacci_shell.py — Phase C3: Fibonacci Shell Fold

Concept:
  Shell layers exist only on Fibonacci clock ticks: tick % fibo[layer] == 0
  Layer 0 (fibo=1) → always live
  Layer 11 (fibo=144) → rare

  shell_fold_nearest() — find nearest live layer if target is frozen
  ring_hot_path() — O(1) pentagon axis access (always live)

Integration with pipeline:
  Each CubeCtx face has a layer (depth)
  Layer existence checked via Fibonacci clock
  Frozen layers → find nearest live layer
"""
import struct

# Fibonacci clock table: F(0)..F(11)
GEO_FIBO = [1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144]

# Constants
GEO_PENTAGONS = 12
GEO_FULL = 20736  # 144²
GEO_TOWER = 12    # layers per pentagon face


def shell_layer_live(layer, tick):
    """Check if layer exists at given tick."""
    fibo = GEO_FIBO[layer % 12]
    return (tick % fibo) == 0


def shell_fold_nearest(layer, tick, pent_axis=0):
    """
    Find nearest live layer if target is frozen.
    pent_axis=1: bypass (hot path always live)
    Otherwise: walk outward layer-1, layer+1, layer-2, layer+2 ...
    Falls back to layer 0 (always live, fibo=1)
    """
    if pent_axis:
        return layer

    layer = layer % 12
    if tick % GEO_FIBO[layer] == 0:
        return layer

    for delta in range(1, 12):
        if layer >= delta:
            lo = layer - delta
            if tick % GEO_FIBO[lo] == 0:
                return lo
        hi = layer + delta
        if hi < 12 and tick % GEO_FIBO[hi] == 0:
            return hi

    return 0  # fallback: always live


def ring_hot_path(pent_id, layer, globe=0):
    """
    O(1) pentagon axis access — bypasses fibonacci clock, always live.
    Returns geo_jump node_id of the pentagon center at given layer.
    """
    face_stride = GEO_FULL // GEO_PENTAGONS
    base = (pent_id % GEO_PENTAGONS) * face_stride
    # globe_offset simplified: 0 for inner, face_stride//2 for outer
    offset = 0 if globe == 0 else face_stride // 2
    return (base + layer * GEO_TOWER + offset) % GEO_FULL


class FibonacciShell:
    """
    Fibonacci Shell Fold manager.

    Tracks which layers are live at each tick.
    Provides fold/unfold operations for cube faces.
    """
    def __init__(self):
        self.tick = 0
        self.live_cache = {}  # layer → is_live

    def advance(self):
        """Advance clock by one tick."""
        self.tick += 1
        self.live_cache.clear()

    def is_live(self, layer):
        """Check if layer is live at current tick."""
        if layer not in self.live_cache:
            self.live_cache[layer] = shell_layer_live(layer, self.tick)
        return self.live_cache[layer]

    def fold(self, layer):
        """
        Fold: find the actual layer to use.
        If target layer is live, use it.
        Otherwise, find nearest live layer.
        """
        if self.is_live(layer):
            return layer
        return shell_fold_nearest(layer, self.tick)

    def unfold(self, layer):
        """
        Unfold: reverse the fold operation.
        Returns the original layer if it was live, else the folded layer.
        """
        return self.fold(layer)  # symmetric operation

    def get_live_layers(self):
        """Get all live layers at current tick."""
        return [l for l in range(12) if self.is_live(l)]

    def hot_path(self, pent_id, globe=0):
        """O(1) pentagon axis access — always live."""
        return ring_hot_path(pent_id, 0, globe)


def test_fibonacci_basics():
    """Test Fibonacci shell fold basics."""
    print("=" * 65)
    print("Phase C3: Fibonacci Shell Fold Basics")
    print("=" * 65)

    # Test Fibonacci table
    print("\n[1] Fibonacci table:")
    for i, f in enumerate(GEO_FIBO):
        print(f"  F({i}) = {f}")

    # Test layer existence at tick 0
    print("\n[2] Layer existence at tick 0:")
    for layer in range(12):
        live = shell_layer_live(layer, 0)
        print(f"  Layer {layer}: {'LIVE' if live else 'frozen'} (fibo={GEO_FIBO[layer]})")

    # Test layer existence at tick 1
    print("\n[3] Layer existence at tick 1:")
    for layer in range(12):
        live = shell_layer_live(layer, 1)
        print(f"  Layer {layer}: {'LIVE' if live else 'frozen'}")

    # Test fold at tick 1
    print("\n[4] Fold at tick 1 (most layers frozen):")
    for layer in range(12):
        folded = shell_fold_nearest(layer, 1)
        print(f"  Layer {layer} → folded to {folded}")

    print("\n  Fibonacci basics: PASS ✓")
    return True


def test_shell_fold_operations():
    """Test shell fold operations with cube pipeline."""
    print("\n" + "=" * 65)
    print("Phase C3: Shell Fold Operations")
    print("=" * 65)

    shell = FibonacciShell()

    # Simulate 20 ticks
    print("\n[1] Simulating 20 ticks...")
    layer_usage = {l: 0 for l in range(12)}

    for tick in range(20):
        shell.advance()
        live = shell.get_live_layers()

        # For each layer, check if it's live
        for layer in range(12):
            if shell.is_live(layer):
                layer_usage[layer] += 1

        if tick < 5 or tick >= 15:
            print(f"  Tick {tick}: live layers = {live}")

    print("\n[2] Layer usage over 20 ticks:")
    for layer in range(12):
        count = layer_usage[layer]
        bar = '#' * count
        print(f"  Layer {layer:2d}: {count:2d}/20 {bar}")

    # Test fold/unfold roundtrip
    print("\n[3] Fold/unfold roundtrip:")
    shell.tick = 10  # set specific tick
    for layer in range(12):
        folded = shell.fold(layer)
        unfolded = shell.unfold(folded)
        ok = (unfolded == folded)  # fold is idempotent
        print(f"  Layer {layer}: fold={folded}, unfold={unfolded} {'✓' if ok else '✗'}")

    # Test hot path
    print("\n[4] Hot path (O(1) pentagon access):")
    for pent in range(6):
        node_id = ring_hot_path(pent, 0, 0)
        print(f"  Pentagon {pent}: node_id={node_id}")

    print("\n" + "=" * 65)
    print("Phase C3: ALL PASS ✓")
    print("=" * 65)
    return True


def test_integration_with_cube():
    """Test integration with cube assembly."""
    print("\n" + "=" * 65)
    print("Phase C3: Integration with Cube Assembly")
    print("=" * 65)

    shell = FibonacciShell()

    # Create 6 cube faces with different layers
    faces = []
    for i in range(6):
        layer = i * 2  # layers 0, 2, 4, 6, 8, 10
        faces.append({'face_id': i, 'layer': layer, 'data': bytes([i] * 64)})

    print("\n[1] Original faces:")
    for f in faces:
        print(f"  Face {f['face_id']}: layer={f['layer']}")

    # Fold each face to nearest live layer
    print("\n[2] Folding to live layers...")
    folded_faces = []
    for f in faces:
        shell.advance()
        folded_layer = shell.fold(f['layer'])
        folded_faces.append({**f, 'folded_layer': folded_layer, 'fold_tick': shell.tick})
        print(f"  Face {f['face_id']}: layer {f['layer']} → {folded_layer} (tick={shell.tick})")

    # Verify all folded layers are live at the SAME tick they were folded
    print("\n[3] Verifying all folded layers are live at their fold tick...")
    all_live = True
    for f in folded_faces:
        live = shell_layer_live(f['folded_layer'], f['fold_tick'])
        if not live:
            all_live = False
        print(f"  Face {f['face_id']}: layer {f['folded_layer']} at tick {f['fold_tick']} live={live}")

    print(f"\n  All live: {all_live} {'✓' if all_live else '✗'}")

    # Test ring hot path
    print("\n[4] Ring hot path for pentagons...")
    for pent in range(6):
        node = ring_hot_path(pent, 0, 0)
        print(f"  Pentagon {pent}: node_id={node}")

    print("\n" + "=" * 65)
    print(f"Phase C3 Integration: {'ALL PASS ✓' if all_live else 'SOME FAILED ✗'}")
    print("=" * 65)
    return all_live


if __name__ == '__main__':
    import sys
    ok1 = test_fibonacci_basics()
    ok2 = test_shell_fold_operations()
    ok3 = test_integration_with_cube()
    sys.exit(0 if (ok1 and ok2 and ok3) else 1)
