#!/usr/bin/env python3
"""
Multimap Ghost Rate Experiment
==============================
Compare ghost rates for different map configurations on 10x10x10 binary cubes.

Configs:
  A: 6 maps   — standard 6 diagonal projections (baseline)
  B: 12 maps  — 6 standard + 6 using (2a±b) coefficients
  C: 24 maps  — 12 from B + 12 more: (a±2b) + corner combos
  D: 9 maps   — 3-axis + 6-diagonal (existing best)
  E: 24 maps  — 24-cell vertex projections (all corner diagonals × 3 free axes)

For each config, test at densities 5%, 10%, 15%, 20%, 30% with 100 random cubes.
"""

import numpy as np
import time

DIM = 10
N_TRIALS = 100
DENSITIES = [5, 10, 15, 20, 30]  # percent

# Pre-compute coordinate grids
xs, ys, zs = np.meshgrid(np.arange(DIM), np.arange(DIM), np.arange(DIM), indexing='ij')


def rand_cube(n_points, rng):
    """Generate a random binary cube with exactly n_points set to 1."""
    cube = np.zeros((DIM, DIM, DIM), dtype=np.uint8)
    coords = set()
    while len(coords) < n_points:
        c = tuple(rng.integers(0, DIM, 3))
        coords.add(c)
    for c in coords:
        cube[c] = 1
    return cube


# ============================================================================
# Projection builders
# ============================================================================

def build_projection(key_arr, other_arr, cube):
    """Build a 2D binary map: for each (key_val, other_val), mark 1 if any voxel exists there."""
    m = np.zeros((DIM, DIM), dtype=np.uint8)
    fk, fo, fc = key_arr.ravel(), other_arr.ravel(), cube.ravel().astype(bool)
    m[fk[fc], fo[fc]] = 1
    return m


def build_axis_projections(cube):
    """3 standard axis projections: any along x, y, z."""
    mx = np.any(cube, axis=0).astype(np.uint8)  # shape [y,z]
    my = np.any(cube, axis=1).astype(np.uint8)  # shape [x,z]
    mz = np.any(cube, axis=2).astype(np.uint8)  # shape [x,y]
    return mx, my, mz


def build_config_a(cube):
    """Config A: 6 standard diagonal maps (a±b)%10 projected along free axis."""
    maps = []
    # (x+y)%10 vs z, (x-y)%10 vs z
    maps.append(build_projection((xs + ys) % DIM, zs, cube))
    maps.append(build_projection((xs - ys) % DIM, zs, cube))
    # (y+z)%10 vs x, (y-z)%10 vs x
    maps.append(build_projection((ys + zs) % DIM, xs, cube))
    maps.append(build_projection((ys - zs) % DIM, xs, cube))
    # (x+z)%10 vs y, (x-z)%10 vs y
    maps.append(build_projection((xs + zs) % DIM, ys, cube))
    maps.append(build_projection((xs - zs) % DIM, ys, cube))
    return maps


def build_config_b(cube):
    """Config B: 12 maps — 6 standard + 6 using (2a±b) coefficients."""
    maps = build_config_a(cube)
    # (2x+y)%10 vs z, (2x-y)%10 vs z
    maps.append(build_projection((2 * xs + ys) % DIM, zs, cube))
    maps.append(build_projection((2 * xs - ys) % DIM, zs, cube))
    # (2y+z)%10 vs x, (2y-z)%10 vs x
    maps.append(build_projection((2 * ys + zs) % DIM, xs, cube))
    maps.append(build_projection((2 * ys - zs) % DIM, xs, cube))
    # (2x+z)%10 vs y, (2x-z)%10 vs y
    maps.append(build_projection((2 * xs + zs) % DIM, ys, cube))
    maps.append(build_projection((2 * xs - zs) % DIM, ys, cube))
    return maps


def build_config_c(cube):
    """Config C: 24 maps — 12 from B + 6 (a±2b) + 6 corner combos."""
    maps = build_config_b(cube)
    # 6 more: (a±2b) coefficients
    maps.append(build_projection((xs + 2 * ys) % DIM, zs, cube))
    maps.append(build_projection((xs - 2 * ys) % DIM, zs, cube))
    maps.append(build_projection((ys + 2 * zs) % DIM, xs, cube))
    maps.append(build_projection((ys - 2 * zs) % DIM, xs, cube))
    maps.append(build_projection((xs + 2 * zs) % DIM, ys, cube))
    maps.append(build_projection((xs - 2 * zs) % DIM, ys, cube))
    # 6 corner combos: (x+y+z)%10 and (x+y-z)%10 projected along each axis
    maps.append(build_projection((xs + ys + zs) % DIM, xs, cube))
    maps.append(build_projection((xs + ys + zs) % DIM, ys, cube))
    maps.append(build_projection((xs + ys + zs) % DIM, zs, cube))
    maps.append(build_projection((xs + ys - zs) % DIM, xs, cube))
    maps.append(build_projection((xs + ys - zs) % DIM, ys, cube))
    maps.append(build_projection((xs + ys - zs) % DIM, zs, cube))
    return maps


def build_config_d(cube):
    """Config D: 9 maps — 3-axis + 6-diagonal (existing best)."""
    mx, my, mz = build_axis_projections(cube)
    diag_maps = build_config_a(cube)  # same 6 diagonal maps
    return [mx, my, mz] + diag_maps


def build_config_e(cube):
    """Config E: 24-cell vertex projections.
    
    Use all 8 corner diagonal directions (±1,±1,±1) × 3 free axes = 24 projections.
    For direction (a,b,c) with a=1, b∈{-1,1}, c∈{-1,1}:
      key = (a*x + b*y + c*z) % 10
      Project along x, y, and z (3 maps per direction).
    4 directions × 3 projections = 12 unique maps.
    Plus 12 edge-diagonal projections using (2,1,0) combos:
      (2x+y)%10 vs z, (2x-y)%10 vs z, (x+2y)%10 vs z, (x-2y)%10 vs z
      (2y+z)%10 vs x, (2y-z)%10 vs x, (y+2z)%10 vs x, (y-2z)%10 vs x
      (2x+z)%10 vs y, (2x-z)%10 vs y, (x+2z)%10 vs y, (x-2z)%10 vs y
    Total = 12 + 12 = 24 maps.
    """
    maps = []
    # Corner diagonals: (1,±1,±1) projected along x, y, z
    combos = [(1, 1, 1), (1, 1, -1), (1, -1, 1), (1, -1, -1)]
    for (ca, cb, cc) in combos:
        key = (ca * xs + cb * ys + cc * zs) % DIM
        maps.append(build_projection(key, xs, cube))  # along x
        maps.append(build_projection(key, ys, cube))  # along y
        maps.append(build_projection(key, zs, cube))  # along z
    # Edge diagonals with (2,1,0) combos
    maps.append(build_projection((2 * xs + ys) % DIM, zs, cube))
    maps.append(build_projection((2 * xs - ys) % DIM, zs, cube))
    maps.append(build_projection((xs + 2 * ys) % DIM, zs, cube))
    maps.append(build_projection((xs - 2 * ys) % DIM, zs, cube))
    maps.append(build_projection((2 * ys + zs) % DIM, xs, cube))
    maps.append(build_projection((2 * ys - zs) % DIM, xs, cube))
    maps.append(build_projection((ys + 2 * zs) % DIM, xs, cube))
    maps.append(build_projection((ys - 2 * zs) % DIM, xs, cube))
    maps.append(build_projection((2 * xs + zs) % DIM, ys, cube))
    maps.append(build_projection((2 * xs - zs) % DIM, ys, cube))
    maps.append(build_projection((xs + 2 * zs) % DIM, ys, cube))
    maps.append(build_projection((xs - 2 * zs) % DIM, ys, cube))
    return maps


# ============================================================================
# Reconstruction from maps
# ============================================================================

def reconstruct_from_maps(maps, map_specs):
    """Reconstruct cube from 2D maps using AND constraints.
    
    maps: list of 2D numpy arrays (DIM × DIM)
    map_specs: list of (key_arr, other_arr) tuples describing how each map was built
    """
    recon = np.ones((DIM, DIM, DIM), dtype=np.uint8)
    for m, (key, other) in zip(maps, map_specs):
        recon &= m[key, other]
    return recon


def get_map_specs_a():
    """Map specs for Config A."""
    return [
        ((xs + ys) % DIM, zs),
        ((xs - ys) % DIM, zs),
        ((ys + zs) % DIM, xs),
        ((ys - zs) % DIM, xs),
        ((xs + zs) % DIM, ys),
        ((xs - zs) % DIM, ys),
    ]


def get_map_specs_b():
    """Map specs for Config B."""
    specs = get_map_specs_a()
    specs += [
        ((2 * xs + ys) % DIM, zs),
        ((2 * xs - ys) % DIM, zs),
        ((2 * ys + zs) % DIM, xs),
        ((2 * ys - zs) % DIM, xs),
        ((2 * xs + zs) % DIM, ys),
        ((2 * xs - zs) % DIM, ys),
    ]
    return specs


def get_map_specs_c():
    """Map specs for Config C."""
    specs = get_map_specs_b()
    specs += [
        ((xs + 2 * ys) % DIM, zs),
        ((xs - 2 * ys) % DIM, zs),
        ((ys + 2 * zs) % DIM, xs),
        ((ys - 2 * zs) % DIM, xs),
        ((xs + 2 * zs) % DIM, ys),
        ((xs - 2 * zs) % DIM, ys),
        # Corner combos
        ((xs + ys + zs) % DIM, xs),
        ((xs + ys + zs) % DIM, ys),
        ((xs + ys + zs) % DIM, zs),
        ((xs + ys - zs) % DIM, xs),
        ((xs + ys - zs) % DIM, ys),
        ((xs + ys - zs) % DIM, zs),
    ]
    return specs


def get_map_specs_d():
    """Map specs for Config D (axis + diagonal)."""
    # Axis projections are handled separately (not key/other pair)
    # We need a special reconstruction path for D
    return None  # handled specially


def get_map_specs_e():
    """Map specs for Config E."""
    specs = []
    combos = [(1, 1, 1), (1, 1, -1), (1, -1, 1), (1, -1, -1)]
    for (ca, cb, cc) in combos:
        key = (ca * xs + cb * ys + cc * zs) % DIM
        specs.append((key, xs))
        specs.append((key, ys))
        specs.append((key, zs))
    specs += [
        ((2 * xs + ys) % DIM, zs),
        ((2 * xs - ys) % DIM, zs),
        ((xs + 2 * ys) % DIM, zs),
        ((xs - 2 * ys) % DIM, zs),
        ((2 * ys + zs) % DIM, xs),
        ((2 * ys - zs) % DIM, xs),
        ((ys + 2 * zs) % DIM, xs),
        ((ys - 2 * zs) % DIM, xs),
        ((2 * xs + zs) % DIM, ys),
        ((2 * xs - zs) % DIM, ys),
        ((xs + 2 * zs) % DIM, ys),
        ((xs - 2 * zs) % DIM, ys),
    ]
    return specs


def reconstruct_config_d(cube_shape, maps):
    """Special reconstruction for Config D: 3-axis AND 6-diagonal."""
    mx, my, mz, diag1, diag2, diag3, diag4, diag5, diag6 = maps
    # Start with axis reconstruction
    recon = (mx[None, :, :] & my[:, None, :] & mz[:, :, None]).astype(np.uint8)
    # Apply diagonal constraints
    diag_specs = get_map_specs_a()
    diag_maps = [diag1, diag2, diag3, diag4, diag5, diag6]
    for m, (key, other) in zip(diag_maps, diag_specs):
        recon &= m[key, other]
    return recon


# ============================================================================
# Main experiment
# ============================================================================

def run_experiment():
    configs = {
        'A': ('6 maps (6dir×1)', 6, build_config_a, get_map_specs_a, 'diagonal'),
        'B': ('12 maps (6dir×2)', 12, build_config_b, get_map_specs_b, 'diagonal'),
        'C': ('24 maps (6dir×4)', 24, build_config_c, get_map_specs_c, 'diagonal'),
        'D': ('9 maps (3ax+6diag)', 9, build_config_d, None, 'special'),
        'E': ('24 maps (24cell)', 24, build_config_e, get_map_specs_e, 'diagonal'),
    }

    results = {}  # (config, density) -> {ghost_total, ghost_max, accuracy, trials}

    total_trials = len(configs) * len(DENSITIES) * N_TRIALS
    trial_count = 0
    t_start = time.time()

    for density in DENSITIES:
        n_points = int(DIM ** 3 * density / 100.0)
        rng = np.random.default_rng(42)  # fixed seed for reproducibility within density

        # Accumulators per config
        accum = {}
        for cfg_key in configs:
            accum[cfg_key] = {
                'ghost_sum': 0, 'ghost_max': 0, 'acc_sum': 0.0,
                'ghost_list': [], 'zero_ghost_count': 0
            }

        for trial in range(N_TRIALS):
            cube = rand_cube(n_points, rng)
            total_active = int(cube.sum())

            for cfg_key, (cfg_name, n_maps, build_fn, specs_fn, rtype) in configs.items():
                maps = build_fn(cube)

                if rtype == 'special':
                    recon = reconstruct_config_d(cube.shape, maps)
                else:
                    specs = specs_fn()
                    recon = reconstruct_from_maps(maps, specs)

                # Ghost = cells in reconstruction but not in original
                ghost_mask = (recon == 1) & (cube == 0)
                ghost_count = int(ghost_mask.sum())

                # Reconstruction accuracy: how many original cells are correctly recovered
                # (all original cells should be in reconstruction — false negatives)
                recovered = int((recon & cube).sum())
                accuracy = recovered / total_active * 100 if total_active > 0 else 100.0

                a = accum[cfg_key]
                a['ghost_sum'] += ghost_count
                a['ghost_max'] = max(a['ghost_max'], ghost_count)
                a['acc_sum'] += accuracy
                a['ghost_list'].append(ghost_count)
                if ghost_count == 0:
                    a['zero_ghost_count'] += 1

                trial_count += 1

        # Store results for this density
        for cfg_key in configs:
            a = accum[cfg_key]
            avg_ghost = a['ghost_sum'] / N_TRIALS
            avg_acc = a['acc_sum'] / N_TRIALS
            ghost_rate = (1.0 - a['zero_ghost_count'] / N_TRIALS) * 100
            results[(cfg_key, density)] = {
                'avg_ghost': avg_ghost,
                'max_ghost': a['ghost_max'],
                'ghost_rate': ghost_rate,
                'accuracy': avg_acc,
                'zero_pct': a['zero_ghost_count'] / N_TRIALS * 100,
            }

        elapsed = time.time() - t_start
        print(f"  Density {density:>2}% done ({elapsed:.1f}s elapsed)")

    # ============================================================================
    # Print results table
    # ============================================================================
    print("\n" + "=" * 100)
    print("MULTIMAP GHOST RATE EXPERIMENT RESULTS")
    print(f"Cube: {DIM}×{DIM}×{DIM}, Trials per density: {N_TRIALS}")
    print("=" * 100)

    # Table 1: Ghost Rate (%) — Config vs Density
    print("\n--- TABLE 1: Ghost Rate (%) — percentage of trials with ANY ghost ---")
    header = f"{'Config':<22}"
    for d in DENSITIES:
        header += f"  {d:>5}%"
    header += f"  {'AVG':>6}"
    print(header)
    print("-" * len(header))

    for cfg_key in ['A', 'B', 'C', 'D', 'E']:
        cfg_name = configs[cfg_key][0]
        row = f"{cfg_key}: {cfg_name:<17}"
        rates = []
        for d in DENSITIES:
            r = results[(cfg_key, d)]
            rates.append(r['ghost_rate'])
            row += f"  {r['ghost_rate']:>5.1f}%"
        avg_rate = np.mean(rates)
        row += f"  {avg_rate:>5.1f}%"
        print(row)

    # Table 2: Average Ghost Count
    print("\n--- TABLE 2: Average Ghost Count per trial ---")
    header = f"{'Config':<22}"
    for d in DENSITIES:
        header += f"  {d:>6}%"
    header += f"  {'AVG':>7}"
    print(header)
    print("-" * len(header))

    for cfg_key in ['A', 'B', 'C', 'D', 'E']:
        cfg_name = configs[cfg_key][0]
        row = f"{cfg_key}: {cfg_name:<17}"
        vals = []
        for d in DENSITIES:
            r = results[(cfg_key, d)]
            vals.append(r['avg_ghost'])
            row += f"  {r['avg_ghost']:>7.1f}"
        avg_val = np.mean(vals)
        row += f"  {avg_val:>7.1f}"
        print(row)

    # Table 3: Max Ghost Count
    print("\n--- TABLE 3: Max Ghost Count (worst case) ---")
    header = f"{'Config':<22}"
    for d in DENSITIES:
        header += f"  {d:>6}%"
    print(header)
    print("-" * len(header))

    for cfg_key in ['A', 'B', 'C', 'D', 'E']:
        cfg_name = configs[cfg_key][0]
        row = f"{cfg_key}: {cfg_name:<17}"
        for d in DENSITIES:
            r = results[(cfg_key, d)]
            row += f"  {r['max_ghost']:>7}"
        print(row)

    # Table 4: Reconstruction Accuracy (%)
    print("\n--- TABLE 4: Reconstruction Accuracy (% of original cells recovered) ---")
    header = f"{'Config':<22}"
    for d in DENSITIES:
        header += f"  {d:>5}%"
    header += f"  {'AVG':>6}"
    print(header)
    print("-" * len(header))

    for cfg_key in ['A', 'B', 'C', 'D', 'E']:
        cfg_name = configs[cfg_key][0]
        row = f"{cfg_key}: {cfg_name:<17}"
        accs = []
        for d in DENSITIES:
            r = results[(cfg_key, d)]
            accs.append(r['accuracy'])
            row += f"  {r['accuracy']:>5.1f}%"
        avg_acc = np.mean(accs)
        row += f"  {avg_acc:>5.1f}%"
        print(row)

    # Table 5: Zero-ghost percentage
    print("\n--- TABLE 5: Zero-Ghost Rate (%) — trials with ZERO ghosts ---")
    header = f"{'Config':<22}"
    for d in DENSITIES:
        header += f"  {d:>5}%"
    header += f"  {'AVG':>6}"
    print(header)
    print("-" * len(header))

    for cfg_key in ['A', 'B', 'C', 'D', 'E']:
        cfg_name = configs[cfg_key][0]
        row = f"{cfg_key}: {cfg_name:<17}"
        zero_pcts = []
        for d in DENSITIES:
            r = results[(cfg_key, d)]
            zero_pcts.append(r['zero_pct'])
            row += f"  {r['zero_pct']:>5.1f}%"
        avg_zp = np.mean(zero_pcts)
        row += f"  {avg_zp:>5.1f}%"
        print(row)

    # Table 6: Storage cost comparison
    print("\n--- TABLE 6: Storage Cost (bits) ---")
    map_bits_per_map = DIM * DIM  # 100 bits per 10×10 binary map
    exc_bits = 11  # 10-bit coord + 1 flag per ghost cell
    print(f"{'Config':<22} {'Maps':>5} {'MapBits':>8} {'ExcBits@5%':>11} {'ExcBits@10%':>12} {'ExcBits@20%':>12} {'Total@10%':>10}")
    print("-" * 90)

    for cfg_key in ['A', 'B', 'C', 'D', 'E']:
        cfg_name = configs[cfg_key][0]
        n_maps = configs[cfg_key][1]
        map_bits = n_maps * map_bits_per_map
        row = f"{cfg_key}: {cfg_name:<17} {n_maps:>5} {map_bits:>8}"
        for d in [5, 10, 20]:
            r = results[(cfg_key, d)]
            exc = r['avg_ghost'] * exc_bits
            row += f" {exc:>11.1f}"
        r10 = results[(cfg_key, 10)]
        total_10 = map_bits + r10['avg_ghost'] * exc_bits
        row += f" {total_10:>10.0f}"
        print(row)

    # Summary
    print("\n" + "=" * 100)
    print("SUMMARY")
    print("=" * 100)
    
    # Find best config at each density
    print("\nBest config at each density (lowest ghost rate):")
    for d in DENSITIES:
        best_cfg = min(configs.keys(), key=lambda k: results[(k, d)]['ghost_rate'])
        best_rate = results[(best_cfg, d)]['ghost_rate']
        print(f"  {d:>2}%: Config {best_cfg} ({configs[best_cfg][0]}) — ghost rate {best_rate:.1f}%")

    print("\nMarginal benefit analysis (ghost rate reduction per added map):")
    map_counts = {'A': 6, 'D': 9, 'B': 12, 'C': 24, 'E': 24}
    sorted_configs = sorted(configs.keys(), key=lambda k: map_counts[k])
    prev_key = None
    for cfg_key in sorted_configs:
        n = map_counts[cfg_key]
        avg_gr = np.mean([results[(cfg_key, d)]['ghost_rate'] for d in DENSITIES])
        if prev_key is not None:
            prev_n = map_counts[prev_key]
            prev_gr = np.mean([results[(prev_key, d)]['ghost_rate'] for d in DENSITIES])
            delta_maps = n - prev_n
            delta_rate = prev_gr - avg_gr
            marginal = delta_rate / delta_maps if delta_maps > 0 else 0
            print(f"  Config {cfg_key} ({n} maps): avg ghost rate = {avg_gr:.1f}%, "
                  f"delta from {prev_key}: -{delta_rate:.1f}% over +{delta_maps} maps "
                  f"(= {marginal:.2f}% per map)")
        else:
            print(f"  Config {cfg_key} ({n} maps): avg ghost rate = {avg_gr:.1f}%")
        prev_key = cfg_key

    total_time = time.time() - t_start
    print(f"\nTotal experiment time: {total_time:.1f}s")
    print(f"Total trials: {total_trials}")


if __name__ == '__main__':
    run_experiment()
