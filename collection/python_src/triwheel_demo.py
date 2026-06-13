"""
triwheel_demo.py — Triangle Wheel Activation Test

Tensor data → direction vector → nearest triangle centroid → zone + coordinates
No pre-computed encoder weights. Pure geometric selection.
"""

import math
import ctypes
import subprocess
import os

# ── Test tensors (from test_goldberg_vs_p5h_y6.c) ──
TENSORS = [
    ("text_model.layers.0.attn.q_proj.weight",   [0.01,0.02,0.015,0.5,0.3,0.2]),
    ("text_model.layers.0.attn.k_proj.weight",   [0.01,0.01,0.012,0.4,0.4,0.3]),
    ("text_model.layers.1.ffn.gate_proj.weight", [0.05,0.06,0.04, 1.2,0.8,0.6]),
    ("text_model.layers.1.ffn.down_proj.weight", [0.08,0.07,0.09, 2.1,1.5,1.2]),
    ("vision_model.encoder.layers.0.attn.q",     [0.2, 0.3, 0.25, 5.0,4.0,3.5]),
    ("vision_model.encoder.layers.4.mlp.fc2",    [0.5, 0.8, 0.6,  9.0,7.0,6.0]),
    ("model.layers.0.attn.q_proj.weight",        [0.03,0.04,0.03, 0.8,0.6,0.5]),
    ("model.layers.20.ffn.down_proj.weight",     [0.1, 0.12,0.09, 3.0,2.5,2.0]),
    ("voice.encoder.layers.0.weight",            [0.02,0.02,0.02, 0.3,0.3,0.3]),
    ("voice.decoder.layers.5.weight",            [0.04,0.05,0.04, 0.6,0.5,0.5]),
]

TW_CORE = 10
TW_HEX = 6
TW_N_HEX = 10
TW_RING = TW_HEX * TW_N_HEX
TW_TOT = TW_CORE + TW_RING

DEG36 = math.radians(36)
DEG60 = math.radians(60)

def build_triwheel(base_angle=0.0, radius=1.0, core_radius=0.5):
    """Generate triangle wheel geometry."""
    centroids = []
    vertices = []
    hex_group = []
    zones = []

    # ── Core: 10 Δ around origin @ 36° ──
    for i in range(TW_CORE):
        a0 = i * DEG36 + base_angle
        a1 = (i + 1) * DEG36 + base_angle
        v0 = (0.0, 0.0, 0.0)
        v1 = (core_radius * math.cos(a0), core_radius * math.sin(a0), 0.0)
        v2 = (core_radius * math.cos(a1), core_radius * math.sin(a1), 0.0)
        cx = (v0[0] + v1[0] + v2[0]) / 3.0
        cy = (v0[1] + v1[1] + v2[1]) / 3.0
        centroids.append((cx, cy, 0.0))
        vertices.append((v0, v1, v2))
        hex_group.append(-1)
        zones.append(f"core_{i}")

    # ── Ring: 10 hex × 6 equilateral Δ ──
    dx = core_radius * 1.732  # sqrt(3) = edge to center
    for h in range(TW_N_HEX):
        ha = (h + 0.5) * DEG36 + base_angle
        hcx = core_radius * 1.4 * math.cos(ha)
        hcy = core_radius * 1.4 * math.sin(ha)
        for t in range(TW_HEX):
            ta = ha + (t - 0.5) * DEG60
            tri_len = core_radius * 0.6
            v0 = (hcx, hcy, 0.0)
            v1 = (hcx + tri_len * math.cos(ta - DEG60/2),
                  hcy + tri_len * math.sin(ta - DEG60/2), 0.0)
            v2 = (hcx + tri_len * math.cos(ta + DEG60/2),
                  hcy + tri_len * math.sin(ta + DEG60/2), 0.0)
            cx = (v0[0] + v1[0] + v2[0]) / 3.0
            cy = (v0[1] + v1[1] + v2[1]) / 3.0
            centroids.append((cx, cy, 0.0))
            vertices.append((v0, v1, v2))
            hex_group.append(h)
            zones.append(f"hex{h}_tri{t}")

    return centroids, vertices, hex_group, zones

def vec_angle(v):
    """Angle of 3D vector in xy-plane."""
    return math.atan2(v[1], v[0])

def angle_diff(a, b):
    """Minimal angular difference."""
    d = abs(a - b)
    if d > math.pi:
        d = 2 * math.pi - d
    return d

def activate(centroids, direction_vec, max_n=3):
    """Find nearest triangle centroids by angle."""
    target = vec_angle(direction_vec)
    scored = []
    for i, c in enumerate(centroids):
        ca = vec_angle(c)
        diff = angle_diff(target, ca)
        conf = 1.0 / (1.0 + diff / 0.1)
        scored.append((diff, conf, i, c))
    scored.sort(key=lambda x: x[0])
    return scored[:max_n]

def main():
    print("=== Triangle Wheel Activation Test ===\n")

    centroids, verts, hg, zones = build_triwheel()

    # Print wheel geometry summary
    print(f"Core: {TW_CORE} triangles @ 36° intervals")
    print(f"Ring: {TW_N_HEX} hex × {TW_HEX} tri = {TW_RING} triangles")
    print(f"Total: {TW_TOT} triangles\n")

    sum_shift = 0.0
    sum_conf = 0.0
    n_act = 0

    for name, f in TENSORS:
        print(f"{name}")
        # Core activation: use (f0, f1, f2) 
        core_dir = (f[0], f[1], f[2])
        # Ring activation: use (f3, f4, f5)
        ring_dir = (f[3], f[4], f[5])

        core_hits = activate(centroids[:TW_CORE], core_dir, max_n=2)
        ring_hits = activate(centroids[TW_CORE:], ring_dir, max_n=2)

        # Best activation: closest overall
        all_hits = activate(centroids, (f[0]+f[3], f[1]+f[4], f[2]+f[5]), max_n=3)

        # Compute shift-like metric: best activated centroid norm
        best = all_hits[0]
        idx = best[2]
        conf_best = best[1]
        shift = math.sqrt(centroids[idx][0]**2 + centroids[idx][1]**2 + centroids[idx][2]**2)

        # Zone = triangle ID (0-69) + description
        zone_str = zones[idx]
        hex_id = hg[idx]

        print(f"  core→ {core_hits[0][2]}: core_{core_hits[0][2]} "
              f"(conf={core_hits[0][1]:.3f})")
        if len(core_hits) > 1 and core_hits[1][1] > 0.5:
            print(f"       {core_hits[1][2]}: core_{core_hits[1][2]} "
                  f"(conf={core_hits[1][1]:.3f})")

        print(f"  ring→ {ring_hits[0][2]}: {zones[TW_CORE + ring_hits[0][2]]} "
              f"(conf={ring_hits[0][1]:.3f})")

        print(f"  best→ Δ{idx} @ {zone_str} "
              f"shift={shift:.4f} conf={conf_best:.3f}")
        if hex_id >= 0:
            print(f"       hex_group={hex_id}")
        else:
            print(f"       hex_group=core")
        print()

        sum_shift += shift
        sum_conf += conf_best
        n_act += 1

    avg_shift = sum_shift / n_act if n_act else 0
    avg_conf = sum_conf / n_act if n_act else 0
    print(f"=== Summary ===")
    print(f"Avg shift: {avg_shift:.4f}")
    print(f"Avg conf:  {avg_conf:.4f}")

if __name__ == "__main__":
    main()
