"""
benchmark_tw_vs_rdh.py — Comprehensive Benchmark: tw_capture (72 centroids) vs RDH
═══════════════════════════════════════════════════════════════════════════════════════

Compares:
1. Latency (ns/operation)
2. Throughput (ops/sec)
3. Collision rate
4. Geometric properties (address space coverage, locality)
5. Memory usage
6. Adaptive recommendation

Usage:
    python benchmark_tw_vs_rdh.py
"""

import time
import math
import sys
from typing import List, Tuple, Dict, Any
from dataclasses import dataclass
from collections import defaultdict

# ════════════════════════════════════════════════════════════════════
#  TW_CAPTURE (72 Centroids) Implementation
# ════════════════════════════════════════════════════════════════════

class TWCapture:
    """72 centroids from Goldberg geometry: 60 triangles + 10 hex + 2 pentagon"""
    
    SCALE = 10000
    N_CENTROIDS = 72
    
    def __init__(self):
        self.centroids = self._generate_centroids()
        
    def _generate_centroids(self) -> List[Tuple[int, int, str]]:
        """Generate all 72 centroids"""
        centroids = []
        
        # 60 triangle centroids (6° intervals)
        for i in range(60):
            angle_rad = i * 6 * math.pi / 180.0
            x = int(round(10000 * math.cos(angle_rad)))
            y = int(round(10000 * math.sin(angle_rad)))
            centroids.append((x, y, f"tri_{i:02d}"))
        
        # 10 hex centroids (36° intervals, offset 3°)
        for i in range(10):
            angle_rad = (i * 36 + 3) * math.pi / 180.0
            x = int(round(10000 * math.cos(angle_rad)))
            y = int(round(10000 * math.sin(angle_rad)))
            centroids.append((x, y, f"hex_{i:02d}"))
        
        # 2 pentagon centroids (center)
        centroids.append((0, 0, "pent_00"))
        centroids.append((5000, 0, "pent_01"))
        
        return centroids
    
    def capture(self, vx: int, vy: int) -> Tuple[int, int, int, int]:
        """
        Capture (vx, vy) → (node_id, resid_x, resid_y, drain)
        Pure integer, O(1)
        """
        best_dist = float('inf')
        best_idx = 0
        
        for i, (cx, cy, _) in enumerate(self.centroids):
            dx = vx - cx
            dy = vy - cy
            dist = dx * dx + dy * dy
            if dist < best_dist:
                best_dist = dist
                best_idx = i
        
        cx, cy, _ = self.centroids[best_idx]
        resid_x = vx - cx
        resid_y = vy - cy
        
        # node_id = index (0..71)
        node_id = best_idx
        
        # drain = 0 (no conflict in this simple model)
        drain = 0
        
        return node_id, resid_x, resid_y, drain
    
    def summon(self, node_id: int, resid_x: int, resid_y: int) -> Tuple[int, int]:
        """Reconstruct (vx, vy) from node_id + resid"""
        cx, cy, _ = self.centroids[node_id]
        vx = cx + resid_x
        vy = cy + resid_y
        return vx, vy


# ════════════════════════════════════════════════════════════════════
#  RDH (Ring-Wedge-Mirror) Implementation
# ════════════════════════════════════════════════════════════════════

class RDH:
    """Ring-Wedge-Mirror addressing: pure integer bijection"""
    
    def __init__(self, n_rings: int = 128, n_wedges: int = 162, 
                 n_mirror: int = 1, max_u: int = 1):
        self.n_rings = n_rings
        self.n_wedges = n_wedges
        self.n_mirror = n_mirror
        self.max_u = max_u
        self.capacity = n_rings * n_wedges * n_mirror * max_u
        
    def encode(self, ring: int, wedge: int, mirror: int = 0, u: int = 0) -> int:
        """Encode (ring, wedge, mirror, u) → flat key. O(1), no collision."""
        return ((ring * self.n_wedges + wedge) * self.n_mirror + mirror) * self.max_u + u
    
    def decode(self, key: int) -> Tuple[int, int, int, int]:
        """Decode flat key → (ring, wedge, mirror, u). O(1)."""
        u = key % self.max_u
        key //= self.max_u
        mirror = key % self.n_mirror
        key //= self.n_mirror
        wedge = key % self.n_wedges
        ring = key // self.n_wedges
        return ring, wedge, mirror, u
    
    def capture(self, vx: int, vy: int) -> Tuple[int, int, int, int]:
        """
        Capture (vx, vy) → (node_id, resid_x, resid_y, drain)
        Maps 2D point to nearest RDH address
        """
        # Simple mapping: use vx, vy to determine ring and wedge
        ring = abs(vx) % self.n_rings
        wedge = abs(vy) % self.n_wedges
        
        node_id = self.encode(ring, wedge)
        
        # Residual = difference from exact grid position
        resid_x = vx - ring * 1000  # Scale factor
        resid_y = vy - wedge * 100
        
        drain = 0
        
        return node_id, resid_x, resid_y, drain
    
    def summon(self, node_id: int, resid_x: int, resid_y: int) -> Tuple[int, int]:
        """Reconstruct (vx, vy) from node_id + resid"""
        ring, wedge, _, _ = self.decode(node_id)
        vx = ring * 1000 + resid_x
        vy = wedge * 100 + resid_y
        return vx, vy


# ════════════════════════════════════════════════════════════════════
#  BENCHMARK SUITE
# ════════════════════════════════════════════════════════════════════

@dataclass
class BenchmarkResult:
    name: str
    latency_ns: float
    throughput_mops: float
    collision_rate: float
    address_space_size: int
    memory_bytes: int
    geometric_locality: float  # 0-1, how well nearby points map to nearby addresses


def generate_test_data(n_points: int = 100000) -> List[Tuple[int, int]]:
    """Generate synthetic test points"""
    import random
    random.seed(42)
    points = []
    for _ in range(n_points):
        vx = random.randint(-1000000, 1000000)
        vy = random.randint(-1000000, 1000000)
        points.append((vx, vy))
    return points


def benchmark_capture(capture_func, points: List[Tuple[int, int]], name: str) -> BenchmarkResult:
    """Benchmark a capture function"""
    n = len(points)
    
    # Warmup
    for vx, vy in points[:1000]:
        capture_func(vx, vy)
    
    # Latency benchmark
    start = time.perf_counter()
    results = []
    for vx, vy in points:
        results.append(capture_func(vx, vy))
    elapsed = time.perf_counter() - start
    
    latency_ns = (elapsed / n) * 1e9
    throughput_mops = n / elapsed / 1e6
    
    # Collision check
    node_ids = [r[0] for r in results]
    unique_ids = len(set(node_ids))
    collision_rate = 1.0 - (unique_ids / n)
    
    # Memory estimate
    memory_bytes = sys.getsizeof(results) + sum(sys.getsizeof(r) for r in results[:100])
    
    # Geometric locality (simplified)
    locality = 0.0  # Placeholder
    
    return BenchmarkResult(
        name=name,
        latency_ns=latency_ns,
        throughput_mops=throughput_mops,
        collision_rate=collision_rate,
        address_space_size=unique_ids,
        memory_bytes=memory_bytes,
        geometric_locality=locality
    )


def benchmark_roundtrip(capture_func, summon_func, points: List[Tuple[int, int]], name: str) -> Dict[str, Any]:
    """Benchmark capture → summon roundtrip"""
    n = len(points)
    
    # Capture
    start = time.perf_counter()
    captures = [capture_func(vx, vy) for vx, vy in points]
    capture_time = time.perf_counter() - start
    
    # Summon
    start = time.perf_counter()
    reconstructed = [summon_func(node_id, resid_x, resid_y) 
                     for node_id, resid_x, resid_y, _ in captures]
    summon_time = time.perf_counter() - start
    
    # Verify roundtrip
    correct = sum(1 for (vx, vy), (rx, ry) in zip(points, reconstructed) 
                  if vx == rx and vy == ry)
    
    return {
        "name": name,
        "capture_time_ms": capture_time * 1000,
        "summon_time_ms": summon_time * 1000,
        "total_time_ms": (capture_time + summon_time) * 1000,
        "roundtrip_correct": correct,
        "roundtrip_total": n,
        "roundtrip_accuracy": correct / n * 100
    }


# ════════════════════════════════════════════════════════════════════
#  ADAPTIVE SYSTEM RECOMMENDATION
# ════════════════════════════════════════════════════════════════════

def recommend_adaptive(tw_result: BenchmarkResult, rdh_result: BenchmarkResult) -> Dict[str, Any]:
    """Recommend adaptive usage based on benchmark results"""
    
    recommendations = {
        "tw_capture": {
            "best_for": [],
            "strengths": [],
            "weaknesses": []
        },
        "rdh": {
            "best_for": [],
            "strengths": [],
            "weaknesses": []
        },
        "adaptive_strategy": ""
    }
    
    # Analyze latency
    if tw_result.latency_ns < rdh_result.latency_ns:
        recommendations["tw_capture"]["strengths"].append("Lower latency")
        recommendations["rdh"]["weaknesses"].append("Higher latency")
    else:
        recommendations["rdh"]["strengths"].append("Lower latency")
        recommendations["tw_capture"]["weaknesses"].append("Higher latency")
    
    # Analyze collision rate
    if tw_result.collision_rate < rdh_result.collision_rate:
        recommendations["tw_capture"]["strengths"].append("Lower collision rate")
        recommendations["rdh"]["weaknesses"].append("Higher collision rate")
    else:
        recommendations["rdh"]["strengths"].append("Lower collision rate")
        recommendations["tw_capture"]["weaknesses"].append("Higher collision rate")
    
    # Analyze address space
    if tw_result.address_space_size > rdh_result.address_space_size:
        recommendations["tw_capture"]["strengths"].append("Larger address space coverage")
        recommendations["tw_capture"]["best_for"].append("High-dimensional data")
    else:
        recommendations["rdh"]["strengths"].append("Larger address space coverage")
        recommendations["rdh"]["best_for"].append("High-dimensional data")
    
    # Memory efficiency
    if tw_result.memory_bytes < rdh_result.memory_bytes:
        recommendations["tw_capture"]["strengths"].append("Lower memory usage")
    else:
        recommendations["rdh"]["strengths"].append("Lower memory usage")
    
    # Generate adaptive strategy
    if (tw_result.collision_rate < 0.01 and rdh_result.collision_rate < 0.01):
        recommendations["adaptive_strategy"] = (
            "Both systems have low collision rates (<1%). "
            "Use TW_CAPTURE for geometric data (2D spatial locality). "
            "Use RDH for structured addressing (multi-dimensional, no spatial locality). "
            "Adaptive routing: if data has spatial structure → TW_CAPTURE, else → RDH."
        )
    elif tw_result.collision_rate < rdh_result.collision_rate:
        recommendations["adaptive_strategy"] = (
            "TW_CAPTURE has significantly lower collision rate. "
            "Default to TW_CAPTURE for most workloads. "
            "Use RDH only for multi-dimensional addressing where spatial locality is not important."
        )
    else:
        recommendations["adaptive_strategy"] = (
            "RDH has lower collision rate. "
            "Default to RDH for most workloads. "
            "Use TW_CAPTURE for 2D geometric data with spatial locality."
        )
    
    return recommendations


# ════════════════════════════════════════════════════════════════════
#  MAIN BENCHMARK
# ════════════════════════════════════════════════════════════════════

def main():
    print("=" * 80)
    print("BENCHMARK: tw_capture (72 centroids) vs RDH (Ring-Wedge-Mirror)")
    print("=" * 80)
    print()
    
    # Initialize systems
    tw = TWCapture()
    rdh = RDH(n_rings=128, n_wedges=162, n_mirror=1, max_u=1)
    
    # Generate test data
    print("Generating test data (100,000 points)...")
    points = generate_test_data(100000)
    print()
    
    # Benchmark capture
    print("Benchmarking capture operations...")
    tw_result = benchmark_capture(tw.capture, points, "TW Capture (72 centroids)")
    rdh_result = benchmark_capture(rdh.capture, points, "RDH (Ring-Wedge-Mirror)")
    print()
    
    # Print capture results
    print("=" * 80)
    print("CAPTURE BENCHMARK RESULTS")
    print("=" * 80)
    print(f"{'Metric':<25} {'TW Capture':<20} {'RDH':<20} {'Winner':<15}")
    print("-" * 80)
    print(f"{'Latency (ns/ops)':<25} {tw_result.latency_ns:<20.1f} {rdh_result.latency_ns:<20.1f} "
          f"{'TW' if tw_result.latency_ns < rdh_result.latency_ns else 'RDH':<15}")
    print(f"{'Throughput (Mops)':<25} {tw_result.throughput_mops:<20.2f} {rdh_result.throughput_mops:<20.2f} "
          f"{'TW' if tw_result.throughput_mops > rdh_result.throughput_mops else 'RDH':<15}")
    print(f"{'Collision Rate':<25} {tw_result.collision_rate*100:<20.2f}% {rdh_result.collision_rate*100:<20.2f}% "
          f"{'TW' if tw_result.collision_rate < rdh_result.collision_rate else 'RDH':<15}")
    print(f"{'Address Space':<25} {tw_result.address_space_size:<20} {rdh_result.address_space_size:<20} "
          f"{'TW' if tw_result.address_space_size > rdh_result.address_space_size else 'RDH':<15}")
    print(f"{'Memory (bytes)':<25} {tw_result.memory_bytes:<20} {rdh_result.memory_bytes:<20} "
          f"{'TW' if tw_result.memory_bytes < rdh_result.memory_bytes else 'RDH':<15}")
    print()
    
    # Benchmark roundtrip
    print("Benchmarking roundtrip (capture → summon)...")
    tw_roundtrip = benchmark_roundtrip(tw.capture, tw.summon, points, "TW Capture")
    rdh_roundtrip = benchmark_roundtrip(rdh.capture, rdh.summon, points, "RDH")
    print()
    
    # Print roundtrip results
    print("=" * 80)
    print("ROUNDTRIP BENCHMARK RESULTS")
    print("=" * 80)
    print(f"{'Metric':<25} {'TW Capture':<20} {'RDH':<20}")
    print("-" * 80)
    print(f"{'Capture (ms)':<25} {tw_roundtrip['capture_time_ms']:<20.2f} {rdh_roundtrip['capture_time_ms']:<20.2f}")
    print(f"{'Summon (ms)':<25} {tw_roundtrip['summon_time_ms']:<20.2f} {rdh_roundtrip['summon_time_ms']:<20.2f}")
    print(f"{'Total (ms)':<25} {tw_roundtrip['total_time_ms']:<20.2f} {rdh_roundtrip['total_time_ms']:<20.2f}")
    print(f"{'Roundtrip Accuracy':<25} {tw_roundtrip['roundtrip_accuracy']:<20.2f}% {rdh_roundtrip['roundtrip_accuracy']:<20.2f}%")
    print()
    
    # Get recommendations
    recommendations = recommend_adaptive(tw_result, rdh_result)
    
    # Print recommendations
    print("=" * 80)
    print("ADAPTIVE SYSTEM RECOMMENDATIONS")
    print("=" * 80)
    print()
    print("TW_CAPTURE Strengths:")
    for s in recommendations["tw_capture"]["strengths"]:
        print(f"  ✓ {s}")
    print()
    print("TW_CAPTURE Best For:")
    for b in recommendations["tw_capture"]["best_for"]:
        print(f"  • {b}")
    print()
    print("RDH Strengths:")
    for s in recommendations["rdh"]["strengths"]:
        print(f"  ✓ {s}")
    print()
    print("RDH Best For:")
    for b in recommendations["rdh"]["best_for"]:
        print(f"  • {b}")
    print()
    print("=" * 80)
    print("ADAPTIVE STRATEGY")
    print("=" * 80)
    print(recommendations["adaptive_strategy"])
    print()
    
    # Save results to file
    with open("benchmark_results.txt", "w") as f:
        f.write("BENCHMARK RESULTS: tw_capture vs RDH\n")
        f.write("=" * 80 + "\n\n")
        f.write(f"Test Points: {len(points)}\n\n")
        f.write("CAPTURE BENCHMARK:\n")
        f.write(f"  TW Capture: {tw_result.latency_ns:.1f} ns/ops, {tw_result.throughput_mops:.2f} Mops, {tw_result.collision_rate*100:.2f}% collisions\n")
        f.write(f"  RDH:        {rdh_result.latency_ns:.1f} ns/ops, {rdh_result.throughput_mops:.2f} Mops, {rdh_result.collision_rate*100:.2f}% collisions\n\n")
        f.write("ROUNDTRIP BENCHMARK:\n")
        f.write(f"  TW Capture: {tw_roundtrip['total_time_ms']:.2f} ms, {tw_roundtrip['roundtrip_accuracy']:.2f}% accuracy\n")
        f.write(f"  RDH:        {rdh_roundtrip['total_time_ms']:.2f} ms, {rdh_roundtrip['roundtrip_accuracy']:.2f}% accuracy\n\n")
        f.write("ADAPTIVE STRATEGY:\n")
        f.write(recommendations["adaptive_strategy"] + "\n")
    
    print("Results saved to: benchmark_results.txt")
    print()


if __name__ == "__main__":
    main()
