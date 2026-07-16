"""
test_compression_pipeline.py — Step-by-step ratio measurement
═══════════════════════════════════════════════════════════════
Measures compression ratio at EVERY pipeline step to identify
where real compression happens vs where overhead is added.

Pipeline steps:
  1. Raw data
  2. Chunk into 64B blocks
  3. geo_jump routing (Hilbert curve)
  4. Row × column decomposition
  5. Skeleton compression (DIFF/BREF/GEOM)
  6. zlib/zstd outer layer
  7. Final container (.geopixel / .vault.svgz)

Usage:
  python test_compression_pipeline.py
  python test_compression_pipeline.py --data-type structured
  python test_compression_pipeline.py --data-type all
"""

import sys, os, time, hashlib, struct, zlib, math
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from geopixel_pipeline import (
    frame_enc, frame_at, FACE_LIST, CHUNK_SZ,
    skeleton_compress_chunk, skeleton_decompress_chunk,
    SKEL_RAW, SKEL_GEOM, SKEL_DIFF,
    GEOM_INLINE, SKEL_DIFF_CEILING,
    geo_jump_r, JUMP_HILBERT,
)
SKEL_IDENTITY = 0  # may not exist in pipeline, use raw=0


# ════════════════════════════════════════════════════════════════
# Test data generators
# ════════════════════════════════════════════════════════════════

def gen_structured(size):
    """Repeated pattern — best case for geometric compression."""
    pattern = bytes(range(256))
    repeats = size // len(pattern) + 1
    return (pattern * repeats)[:size]

def gen_random(size):
    """Incompressible data — worst case."""
    return os.urandom(size)

def gen_source(size):
    """Python source code — semi-structured."""
    code = b"""def compress(data):
    result = bytearray()
    for chunk in chunk_data(data, 64):
        pattern = analyze_pattern(chunk)
        if pattern.is_uniform:
            result.append(0x01)
            result.append(chunk[0])
        else:
            compressed = delta_encode(chunk)
            result.extend(compressed)
    return bytes(result)

def decompress(data):
    output = bytearray()
    # decompression logic here
    return bytes(output)
"""
    return (code * (size // len(code) + 1))[:size]

def gen_tensor(size):
    """Simulated quantized tensor weights — clusters of similar values."""
    # Q4 weights: groups of 16 similar bytes
    result = bytearray()
    while len(result) < size:
        base = bytes([48 + (len(result) % 32)] * 16)
        noise = os.urandom(16)
        block = bytes(a ^ (b >> 4) for a, b in zip(base, noise))
        result.extend(block)
    return bytes(result[:size])


# ════════════════════════════════════════════════════════════════
# Pipeline steps (measured individually)
# ════════════════════════════════════════════════════════════════

class StepResult:
    def __init__(self, name, input_size, output_size, time_ms=0, note=''):
        self.name = name
        self.input_size = input_size
        self.output_size = output_size
        self.ratio = input_size / output_size if output_size > 0 else 0
        self.time_ms = time_ms
        self.note = note

    def __str__(self):
        ratio_str = f'{self.ratio:.2f}x'
        bar_len = min(40, int(abs(self.ratio) * 10))
        if self.ratio >= 1.0:
            bar = '█' * bar_len + f' COMPRESSED'
        else:
            bar = '░' * bar_len + f' EXPANDED'
        return (f'  {self.name:<35s} {self.input_size:>10,} → {self.output_size:>10,}  '
                f'{ratio_str:>8s}  {self.time_ms:>6.1f}ms  {bar}')


def step_chunk(data, chunk_size=64):
    """Step 1: Split into fixed-size chunks."""
    t0 = time.perf_counter()
    chunks = []
    for i in range(0, len(data), chunk_size):
        chunk = data[i:i + chunk_size]
        if len(chunk) < chunk_size:
            chunk = chunk + b'\x00' * (chunk_size - len(chunk))
        chunks.append(chunk)
    t1 = time.perf_counter()
    # Output = list of chunks (each 64B)
    output_size = len(chunks) * chunk_size
    return StepResult('1. Chunk into 64B blocks', len(data), output_size,
                       (t1 - t0) * 1000, f'{len(chunks)} chunks'), chunks


def step_geo_jump(chunks):
    """Step 2: geo_jump Hilbert routing — reorder chunks."""
    t0 = time.perf_counter()
    n = len(chunks)
    side = max(2, int(math.ceil(n ** 0.5)))  # sqrt for 2D grid
    routed = [None] * n
    for ci in range(n):
        node = ci % 1440
        routed_node = geo_jump_r(node, (JUMP_HILBERT, 1, 1, 1))
        idx = routed_node % n
        routed[idx] = chunks[ci]
    # Fill any None slots
    for i in range(n):
        if routed[i] is None:
            routed[i] = chunks[i]
    t1 = time.perf_counter()
    output_size = len(routed) * CHUNK_SZ
    return StepResult('2. geo_jump Hilbert routing', n * CHUNK_SZ, output_size,
                       (t1 - t0) * 1000, f'side={side}'), routed


def step_row_col_decompose(chunks, side=None):
    """Step 3: Row × Column decomposition — with residual for lossless roundtrip."""
    t0 = time.perf_counter()
    n = len(chunks)
    if side is None:
        side = max(2, int(math.ceil(n ** 0.5)))  # sqrt for 2D grid, NOT cube root

    # Compute row XOR patterns
    row_patterns = []
    for r in range(side):
        pattern = bytearray(CHUNK_SZ)
        for c in range(side):
            idx = r * side + c
            if idx < n:
                for b in range(CHUNK_SZ):
                    pattern[b] ^= chunks[idx][b]
        row_patterns.append(bytes(pattern))

    # Compute column XOR patterns
    col_patterns = []
    for c in range(side):
        pattern = bytearray(CHUNK_SZ)
        for r in range(side):
            idx = r * side + c
            if idx < n:
                for b in range(CHUNK_SZ):
                    pattern[b] ^= chunks[idx][b]
        col_patterns.append(bytes(pattern))

    # Compute residuals: what XOR of row+col DOESN'T capture
    # For cell[r][c]: predicted = row_patterns[r] XOR col_patterns[c]
    # residual = actual XOR predicted
    residuals = bytearray()
    residual_count = 0
    for r in range(side):
        for c in range(side):
            idx = r * side + c
            if idx < n:
                predicted = bytes(a ^ b for a, b in zip(row_patterns[r], col_patterns[c]))
                actual = chunks[idx]
                res = bytes(a ^ b for a, b in zip(actual, predicted))
                if any(b != 0 for b in res):
                    residuals.extend(struct.pack('<I', idx))
                    residuals.extend(res)
                    residual_count += 1

    t1 = time.perf_counter()

    # Output = row_patterns + col_patterns + residuals (LOSSLESS)
    output_size = (len(row_patterns) + len(col_patterns)) * CHUNK_SZ + len(residuals)
    lossless = True  # residual makes it lossless
    return (StepResult('3. Row×Col+residual (lossless)', n * CHUNK_SZ, output_size,
                        (t1 - t0) * 1000, f'{side} rows + {side} cols + {residual_count} residuals'),
            row_patterns, col_patterns, residuals)


def step_skeleton_compress(chunks):
    """Step 4: Skeleton compression (DIFF/BREF/GEOM per chunk)."""
    t0 = time.perf_counter()
    compressed = bytearray()
    prev_chunk = b'\x00' * CHUNK_SZ
    stats = {'RAW': 0, 'GEOM': 0, 'DIFF': 0, 'IDENTITY': 0}

    for ci, chunk in enumerate(chunks):
        # Simple strategy: IDENTITY if same as prev, else GEOM
        if chunk == prev_chunk and ci > 0:
            strategy = SKEL_IDENTITY
        else:
            # Check if diff with any previous chunk is small
            best_dc = CHUNK_SZ
            for pi in range(max(0, ci - 8), ci):
                dc = sum(1 for a, b in zip(chunk, chunks[pi]) if a != b)
                if dc < best_dc:
                    best_dc = dc

            if best_dc <= 4:
                strategy = SKEL_DIFF
            else:
                strategy = SKEL_GEOM

        comp = skeleton_compress_chunk(chunk, strategy, prev_chunk)
        compressed.extend(struct.pack('<H', len(comp)))
        compressed.extend(comp)

        if strategy == SKEL_IDENTITY:
            stats['IDENTITY'] += 1
        elif strategy == SKEL_DIFF:
            stats['DIFF'] += 1
        elif strategy == SKEL_GEOM:
            stats['GEOM'] += 1
        else:
            stats['RAW'] += 1

        prev_chunk = chunk

    t1 = time.perf_counter()
    output_size = len(compressed)
    stat_str = ' '.join(f'{k}={v}' for k, v in stats.items() if v > 0)
    return StepResult('4. Skeleton compress (DIFF/GEOM)', len(chunks) * CHUNK_SZ, output_size,
                       (t1 - t0) * 1000, stat_str)


def step_zlib(data, level=9):
    """Step 5: zlib outer compression."""
    t0 = time.perf_counter()
    compressed = zlib.compress(data, level)
    t1 = time.perf_counter()
    return StepResult(f'5. zlib level={level}', len(data), len(compressed),
                       (t1 - t0) * 1000)


def step_zstd(data):
    """Step 5b: zstd compression (if available)."""
    try:
        import zstandard as zstd
        t0 = time.perf_counter()
        ctx = zstd.ZstdCompressor(level=19)
        compressed = ctx.compress(data)
        t1 = time.perf_counter()
        return StepResult('5b. zstd level=19', len(data), len(compressed),
                           (t1 - t0) * 1000)
    except ImportError:
        return StepResult('5b. zstd (not installed)', len(data), 0, 0, 'SKIPPED')


def step_svg_container(data, metadata_size=0):
    """Step 6: SVG container overhead."""
    # Simulate SVG wrapper: base64 encoding + XML tags
    import base64
    t0 = time.perf_counter()
    b64 = base64.b64encode(data).decode()
    svg_overhead = len('<?xml version="1.0"?><svg><metadata></metadata></svg>') + metadata_size
    output_size = len(b64) + svg_overhead
    t1 = time.perf_counter()
    return StepResult('6. SVG container (base64+XML)', len(data), output_size,
                       (t1 - t0) * 1000, f'base64 overhead +{output_size - len(data):,}B')


# ════════════════════════════════════════════════════════════════
# Full pipeline test
# ════════════════════════════════════════════════════════════════

def run_pipeline_test(name, data, verbose=True):
    """Run all pipeline steps and report ratios at each stage."""
    if verbose:
        print(f'\n{"═" * 80}')
        print(f'  TEST: {name}  ({len(data):,} bytes)')
        print(f'{"═" * 80}')

    results = []
    original_size = len(data)

    # Step 1: Chunk
    step1, chunks = step_chunk(data)
    results.append(step1)

    # Step 2: geo_jump routing
    step2, routed = step_geo_jump(chunks)
    results.append(step2)

    # Step 3: Row × Column decomposition
    step3, row_p, col_p, residuals = step_row_col_decompose(chunks)
    results.append(step3)

    # Step 4: Skeleton compression on routed chunks
    step4 = step_skeleton_compress(routed)
    results.append(step4)

    # Step 5: zlib on skeleton output
    step5 = step_zlib(step4.output_size.to_bytes(4, 'little'))
    results.append(step5)

    # Step 5b: zstd on skeleton output
    step5b = step_zstd(step4.output_size.to_bytes(4, 'little'))
    results.append(step5b)

    # Step 6: SVG container
    step6 = step_svg_container(data)
    results.append(step6)

    # Best combination: Row×Col patterns + zlib (with residuals)
    row_col_data = b''.join(row_p + col_p) + residuals
    step_rc_zlib = step_zlib(row_col_data)
    results.append(StepResult('BEST: Row×Col+residual+zlib',
                               original_size, step_rc_zlib.output_size,
                               step_rc_zlib.time_ms))

    # Best combination: Row×Col patterns + zstd (with residuals)
    step_rc_zstd = step_zstd(row_col_data)
    if step_rc_zstd.note != 'SKIPPED':
        results.append(StepResult('BEST: Row×Col+residual+zstd',
                                   original_size, step_rc_zstd.output_size,
                                   step_rc_zstd.time_ms))

    if verbose:
        print(f'\n  Step-by-step ratio (vs original {original_size:,}B):')
        print(f'  {"─" * 78}')
        for r in results:
            print(str(r))
        print(f'  {"─" * 78}')

        # Summary
        best = max(results, key=lambda r: r.ratio)
        worst = min(results, key=lambda r: r.ratio)
        print(f'\n  BEST:  {best.name} → {best.ratio:.2f}x ({best.output_size:,}B)')
        print(f'  WORST: {worst.name} → {worst.ratio:.2f}x ({worst.output_size:,}B)')

        # Key insight
        print(f'\n  KEY INSIGHT:')
        for r in results:
            if 'Row×Col' in r.name:
                if r.ratio > 1.0:
                    print(f'    ✓ {r.name}: {r.ratio:.2f}x — row×col decomposition WORKS')
                else:
                    print(f'    ✗ {r.name}: {r.ratio:.2f}x — row×col does NOT compress this data')
            if 'Skeleton' in r.name:
                if r.ratio > 1.0:
                    print(f'    ✓ {r.name}: {r.ratio:.2f}x — skeleton DIFF works')
                else:
                    print(f'    ✗ {r.name}: {r.ratio:.2f}x — skeleton GEOM overhead')

    return results


# ════════════════════════════════════════════════════════════════
# Main
# ════════════════════════════════════════════════════════════════

def main():
    data_type = 'all'
    size = 100 * 1024  # 100KB default

    for arg in sys.argv[1:]:
        if arg.startswith('--data-type='):
            data_type = arg.split('=', 1)[1]
        elif arg.startswith('--size='):
            size = int(arg.split('=', 1)[1])

    print('═══ Compression Pipeline Test ═══')
    print(f'  Measuring ratio at every step')
    print(f'  Data size: {size:,} bytes')

    generators = {
        'structured': ('Repeated Pattern (256-byte cycle)', gen_structured),
        'random': ('Random Bytes (incompressible)', gen_random),
        'source': ('Python Source Code (semi-structured)', gen_source),
        'tensor': ('Simulated Q4 Tensor Weights (clustered)', gen_tensor),
    }

    if data_type == 'all':
        test_list = list(generators.items())
    elif data_type in generators:
        test_list = [(data_type, generators[data_type])]
    else:
        print(f'Unknown data type: {data_type}')
        print(f'Available: {", ".join(generators.keys())}, all')
        sys.exit(1)

    all_results = {}
    for dtype, (desc, gen) in test_list:
        print(f'\n\n{"█" * 80}')
        print(f'  DATA TYPE: {desc}')
        print(f'{"█" * 80}')
        data = gen(size)
        results = run_pipeline_test(desc, data)
        all_results[dtype] = results

    # Final comparison table
    print(f'\n\n{"═" * 80}')
    print(f'  COMPARISON TABLE')
    print(f'{"═" * 80}')
    print(f'  {"Data Type":<30s} {"Raw→Chunk":>10s} {"Skeleton":>10s} {"Row×Col":>10s} {"Row×Col+zlib":>12s}')
    print(f'  {"─" * 72}')
    for dtype, results in all_results.items():
        r = {res.name: res for res in results}
        raw2chunk = r.get('1. Chunk into 64B blocks')
        skeleton = r.get('4. Skeleton compress (DIFF/GEOM)')
        rowcol = r.get('3. Row×Col decomposition')
        rc_zlib = r.get('BEST: Row×Col + zlib')

        print(f'  {dtype:<30s} '
              f'{raw2chunk.ratio:>9.2f}x ' if raw2chunk else '         - '
              f'{skeleton.ratio:>9.2f}x ' if skeleton else '         - '
              f'{rowcol.ratio:>9.2f}x ' if rowcol else '         - '
              f'{rc_zlib.ratio:>11.2f}x' if rc_zlib else '          -')

    print(f'\n  CONCLUSION:')
    print(f'  - If Row×Col ratio > 1.0x: geometric decomposition provides real compression')
    print(f'  - If Skeleton ratio > 1.0x: DIFF strategy finds repeated chunks')
    print(f'  - If Row×Col+zlib > both单独: combining geometric + standard = synergistic')
    print(f'  - If Row×Col+zlib < Row×Col单独: zlib adds overhead to geometric output')


if __name__ == '__main__':
    main()
