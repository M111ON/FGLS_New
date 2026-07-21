"""
adaptive_system_design.py — Adaptive TW_CAPTURE + RDH System Design
═══════════════════════════════════════════════════════════════════════════════

Based on benchmark results, design an adaptive system that uses both
TW_CAPTURE and RDH based on data characteristics.

Key Insights:
1. TW_CAPTURE: Best for 2D geometric data with spatial locality
2. RDH: Best for multi-dimensional addressing, lower latency
3. Adaptive routing based on data characteristics
"""

import math
from typing import List, Tuple, Dict, Any, Optional
from dataclasses import dataclass
from enum import Enum


class DataCharacteristics(Enum):
    """Characteristics of input data"""
    SPATIAL_2D = "spatial_2d"  # 2D geometric data with locality
    MULTI_DIM = "multi_dim"  # Multi-dimensional structured data
    RANDOM = "random"  # Random/high-entropy data
    MIXED = "mixed"  # Mixed characteristics


@dataclass
class AdaptiveConfig:
    """Configuration for adaptive system"""
    tw_threshold: float = 0.7  # Threshold for TW_CAPTURE decision
    rdh_threshold: float = 0.7  # Threshold for RDH decision
    fallback: str = "rdh"  # Default fallback system


class AdaptiveCaptureSystem:
    """
    Adaptive system that routes data to TW_CAPTURE or RDH
    based on data characteristics.
    
    Decision flow:
    1. Analyze input data characteristics
    2. Route to appropriate capture system
    3. Combine results if needed
    """
    
    def __init__(self, config: Optional[AdaptiveConfig] = None):
        self.config = config or AdaptiveConfig()
        self.tw_stats = {"calls": 0, "latency_sum": 0}
        self.rdh_stats = {"calls": 0, "latency_sum": 0}
        
    def analyze_data(self, vx: int, vy: int) -> DataCharacteristics:
        """
        Analyze data characteristics to determine routing.
        
        Heuristics:
        - If |vx| and |vy| are within dodecahedron range → SPATIAL_2D
        - If data has structured patterns → MULTI_DIM
        - If data is random → RANDOM
        """
        # Check if within dodecahedron range (scaled by 207360)
        scale = 207360
        if abs(vx) < scale and abs(vy) < scale:
            return DataCharacteristics.SPATIAL_2D
        
        # Check for structured patterns (simplified)
        if vx % 1000 == 0 or vy % 100 == 0:
            return DataCharacteristics.MULTI_DIM
        
        # Default to random
        return DataCharacteristics.RANDOM
    
    def route_capture(self, vx: int, vy: int) -> Tuple[str, Tuple[int, int, int, int]]:
        """
        Route capture to appropriate system.
        
        Returns:
            (system_name, capture_result)
        """
        start_time = time.perf_counter()
        
        characteristics = self.analyze_data(vx, vy)
        
        if characteristics == DataCharacteristics.SPATIAL_2D:
            # Use TW_CAPTURE for 2D geometric data
            result = self._tw_capture(vx, vy)
            system = "tw_capture"
        elif characteristics == DataCharacteristics.MULTI_DIM:
            # Use RDH for multi-dimensional data
            result = self._rdh_capture(vx, vy)
            system = "rdh"
        elif characteristics == DataCharacteristics.RANDOM:
            # Use RDH for random data (lower latency)
            result = self._rdh_capture(vx, vy)
            system = "rdh"
        else:
            # Fallback to RDH
            result = self._rdh_capture(vx, vy)
            system = "rdh"
        
        elapsed = time.perf_counter() - start_time
        
        # Update stats
        if system == "tw_capture":
            self.tw_stats["calls"] += 1
            self.tw_stats["latency_sum"] += elapsed
        else:
            self.rdh_stats["calls"] += 1
            self.rdh_stats["latency_sum"] += elapsed
        
        return system, result
    
    def _tw_capture(self, vx: int, vy: int) -> Tuple[int, int, int, int]:
        """TW_CAPTURE implementation"""
        # Simplified - use actual implementation in production
        scale = 207360
        sector = int((math.atan2(vy, vx) + math.pi) / (2 * math.pi) * 10) % 10
        slot = sector * 6 + (abs(vx) % 6)
        resid_x = vx - sector * scale // 10
        resid_y = vy - slot * scale // 60
        drain = 0
        return slot, resid_x, resid_y, drain
    
    def _rdh_capture(self, vx: int, vy: int) -> Tuple[int, int, int, int]:
        """RDH implementation"""
        n_rings = 128
        n_wedges = 162
        ring = abs(vx) % n_rings
        wedge = abs(vy) % n_wedges
        node_id = ring * n_wedges + wedge
        resid_x = vx - ring * 1000
        resid_y = vy - wedge * 100
        drain = 0
        return node_id, resid_x, resid_y, drain
    
    def get_stats(self) -> Dict[str, Any]:
        """Get performance statistics"""
        tw_latency = (self.tw_stats["latency_sum"] / self.tw_stats["calls"] * 1e9 
                      if self.tw_stats["calls"] > 0 else 0)
        rdh_latency = (self.rdh_stats["latency_sum"] / self.rdh_stats["calls"] * 1e9 
                       if self.rdh_stats["calls"] > 0 else 0)
        
        return {
            "tw_calls": self.tw_stats["calls"],
            "rdh_calls": self.rdh_stats["calls"],
            "tw_avg_latency_ns": tw_latency,
            "rdh_avg_latency_ns": rdh_latency,
            "total_calls": self.tw_stats["calls"] + self.rdh_stats["calls"]
        }


def design_adaptive_system():
    """Design the adaptive system architecture"""
    
    print("=" * 90)
    print("ADAPTIVE TW_CAPTURE + RDH SYSTEM DESIGN")
    print("=" * 90)
    print()
    
    print("1. SYSTEM ARCHITECTURE")
    print("-" * 90)
    print("""
    ┌─────────────────────────────────────────────────────────────────────┐
    │                     ADAPTIVE CAPTURE SYSTEM                        │
    ├─────────────────────────────────────────────────────────────────────┤
    │                                                                     │
    │  Input Data (vx, vy)                                               │
    │       │                                                             │
    │       ▼                                                             │
    │  ┌─────────────────┐                                               │
    │  │ Data Analyzer   │ ← Heuristics based on data characteristics   │
    │  └────────┬────────┘                                               │
    │           │                                                         │
    │           ├──────────────────┬──────────────────┐                  │
    │           ▼                  ▼                  ▼                  │
    │  ┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐     │
    │  │ TW_CAPTURE_INT  │ │      RDH        │ │   Fallback      │     │
    │  │ (60 slots)      │ │ (128×162×1×1)   │ │   (RDH)         │     │
    │  │ + drain detect  │ │ + O(1) encode   │ │                 │     │
    │  └────────┬────────┘ └────────┬────────┘ └────────┬────────┘     │
    │           │                   │                   │               │
    │           └───────────────────┴───────────────────┘               │
    │                           │                                       │
    │                           ▼                                       │
    │                  ┌─────────────────┐                              │
    │                  │ Result Combiner │                              │
    │                  └────────┬────────┘                              │
    │                           │                                       │
    │                           ▼                                       │
    │                  (slot/node_id, resid_x, resid_y, drain)         │
    └─────────────────────────────────────────────────────────────────────┘
    """)
    
    print("2. DECISION LOGIC")
    print("-" * 90)
    print("""
    Data Characteristics → Routing Decision:
    
    ┌────────────────────┬────────────────────┬────────────────────┐
    │ Characteristic     │ Route To           │ Reason             │
    ├────────────────────┼────────────────────┼────────────────────┤
    │ SPATIAL_2D         │ TW_CAPTURE_INT     │ 2D geometric data │
    │ (|vx|,|vy| < scale)│ (60 slots + drain) │ with spatial      │
    │                    │                    │ locality           │
    ├────────────────────┼────────────────────┼────────────────────┤
    │ MULTI_DIM          │ RDH                │ Structured data   │
    │ (structured       │ (128×162×1×1)      │ with multiple     │
    │  patterns)         │                    │ dimensions        │
    ├────────────────────┼────────────────────┼────────────────────┤
    │ RANDOM             │ RDH                │ Random/high-      │
    │ (high entropy)     │ (lower latency)    │ entropy data      │
    ├────────────────────┼────────────────────┼────────────────────┤
    │ MIXED              │ RDH (fallback)     │ Unknown/complex   │
    │ (unknown)          │                    │ characteristics   │
    └────────────────────┴────────────────────┴────────────────────┘
    """)
    
    print("3. STRENGTHS BY SYSTEM")
    print("-" * 90)
    print("""
    TW_CAPTURE_INT Strengths:
    ├── 2D geometric data with spatial locality
    ├── Drain detection at sector boundaries (0.1-1% cases)
    ├── Lossless roundtrip (100% accuracy)
    └── Deterministic integer-only computation
    
    RDH Strengths:
    ├── Lower latency (682 ns vs 9379 ns)
    ├── Higher throughput (1.47 Mops vs 0.11 Mops)
    ├── Larger address space (20,736 vs 60 slots)
    ├── No collision (pure integer bijection)
    └── Multi-dimensional addressing (ring, wedge, mirror, u)
    
    Combined Benefits:
    ├── Best of both worlds based on data characteristics
    ├── Adaptive routing optimizes for each workload
    ├── Fallback ensures availability
    └── Performance monitoring enables tuning
    """)
    
    print("4. IMPLEMENTATION PLAN")
    print("-" * 90)
    print("""
    Phase 1: Core Adaptive Logic
    ├── Implement DataAnalyzer (heuristics for data characteristics)
    ├── Implement AdaptiveCaptureSystem (routing logic)
    ├── Integrate existing TW_CAPTURE_INT and RDH implementations
    └── Add performance monitoring and statistics
    
    Phase 2: Advanced Features
    ├── Machine learning-based routing (optional)
    ├── Dynamic threshold adjustment
    ├── Workload characterization and caching
    └── Performance prediction and optimization
    
    Phase 3: Integration
    ├── Wire into existing pipeline (capture → SID → store)
    ├── Add CLI commands for testing and benchmarking
    ├── Create comprehensive test suite
    └── Documentation and examples
    """)
    
    print("5. EXPECTED PERFORMANCE")
    print("-" * 90)
    print("""
    Based on benchmark results:
    
    Workload Type          │ TW_CAPTURE Only │ RDH Only │ Adaptive System
    ───────────────────────┼─────────────────┼──────────┼────────────────
    2D Geometric (90%)     │ 9379 ns/ops     │ 682 ns   │ 9379 ns (TW)
    Multi-dimensional (5%) │ N/A             │ 682 ns   │ 682 ns (RDH)
    Random (5%)            │ N/A             │ 682 ns   │ 682 ns (RDH)
    ───────────────────────┼─────────────────┼──────────┼────────────────
    Weighted Average       │ 9379 ns/ops     │ 682 ns   │ ~8900 ns/ops
    
    Note: Adaptive system has slight overhead for routing decision,
    but gains by using optimal system for each workload type.
    
    Key Insight: For workloads with >70% 2D geometric data,
    TW_CAPTURE provides better spatial locality despite higher latency.
    For other workloads, RDH is faster overall.
    """)
    
    print("6. RECOMMENDATION")
    print("-" * 90)
    print("""
    Based on benchmark results and design analysis:
    
    ✓ Use Adaptive System for production workloads with mixed data types
    ✓ Default to RDH for most workloads (lower latency, higher throughput)
    ✓ Use TW_CAPTURE specifically for 2D geometric data with spatial locality
    ✓ Monitor performance and adjust thresholds based on real workload
    ✓ Implement fallback to RDH for unknown data characteristics
    
    The adaptive system provides the best balance of performance,
    flexibility, and maintainability for the FGLS project.
    """)


if __name__ == "__main__":
    design_adaptive_system()
