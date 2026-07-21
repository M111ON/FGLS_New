"""
test_geo_seek_real.py — Test geo_frame_seek on REAL GGUF model weights
═══════════════════════════════════════════════════════════════════════
Reads raw bytes from GGUF Q8_0 tensors, runs tw_capture + frame_seek permutation,
measures delta_ratio on real model data.
"""
import struct, os, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from geo_frame_seek import (
    frame_seek, frame_enc, tw_capture_int_combined, tw_reconstruct_int_combined,
    geo_frame_seek_verify, FRAME_EDGES, FRAME_CYCLE, FRAME_FACE_SZ,
)


def read_gguf_raw(path):
    """Read raw tensor bytes from GGUF by skipping header entirely."""
    with open(path, 'rb') as f:
        data = f.read()

    # Find tensor data: scan for alignment patterns
    # GGUF v3: header ends at offset determined by metadata
    # Simplified: skip first 256KB as header, take raw bytes after
    # Real approach: scan for first non-header block

    # Actually, let's just use the raw bytes after the first 1% of file
    # (header is typically <1% for large models)
    header_end = max(256 * 1024, len(data) // 100)
    raw_data = data[header_end:]

    # Count total Q8_0 bytes available
    n_bytes = len(raw_data)
    print(f"  Raw data from offset {header_end:,}: {n_bytes:,} bytes")

    return raw_data


def test_real_gguf(path, max_bytes=76800):
    """Test geo_frame_seek on real GGUF tensor bytes."""
    print(f"\n{'='*60}")
    print(f"TEST: {os.path.basename(path)}")
    print(f"{'='*60}")

    raw_data = read_gguf_raw(path)
    test_bytes = raw_data[:max_bytes]
    n_pairs = len(test_bytes) // 2
    n_frames = n_pairs // FRAME_EDGES

    print(f"  Test bytes: {len(test_bytes):,} ({n_pairs} pairs, {n_frames} frames)")

    # Verify frame seek
    t0 = time.perf_counter()
    rc = geo_frame_seek_verify()
    t_verify = time.perf_counter() - t0
    print(f"  Verify: {'PASS' if rc == 0 else f'FAIL ({rc})'} ({t_verify*1000:.1f}ms)")

    # Run capture on all pairs
    t0 = time.perf_counter()
    scale = 207360 // 128
    captures = []
    for i in range(n_pairs):
        b0 = test_bytes[2*i]
        b1 = test_bytes[2*i + 1]
        vx = (b0 if b0 < 128 else b0 - 256) * scale
        vy = (b1 if b1 < 128 else b1 - 256) * scale
        cap = tw_capture_int_combined(vx, vy)
        captures.append(cap)
    t_capture = time.perf_counter() - t0

    # Permute by frame
    permuted = []
    for fi in range(n_frames):
        df = frame_seek(fi)
        base = fi * FRAME_EDGES
        for ci in range(FRAME_EDGES):
            src_idx = base + (ci + df.slot) % FRAME_EDGES
            permuted.append(captures[src_idx])

    # Delta encode
    t0 = time.perf_counter()
    same_count = 0
    total_deltas = 0
    zone_same = 0
    slot_same = 0
    resid_same = 0

    if n_frames > 1:
        seed = permuted[:FRAME_EDGES]
        for fi in range(1, n_frames):
            df = frame_seek(fi)
            frame_caps = permuted[fi*FRAME_EDGES : (fi+1)*FRAME_EDGES]
            for ci in range(FRAME_EDGES):
                seed_ci = (ci + df.slot) % FRAME_EDGES
                if seed_ci < len(seed) and ci < len(frame_caps):
                    s = seed[seed_ci]
                    c = frame_caps[ci]
                    total_deltas += 1
                    if c.zone == s.zone:
                        zone_same += 1
                    if c.slot == s.slot:
                        slot_same += 1
                    if c.resid_x == s.resid_x and c.resid_y == s.resid_y:
                        resid_same += 1
                    if (c.zone == s.zone and c.slot == s.slot and
                        c.resid_x == s.resid_x and c.resid_y == s.resid_y):
                        same_count += 1
    t_delta = time.perf_counter() - t0

    # Verify roundtrip on captures
    roundtrip_ok = 0
    for i in range(min(1000, len(captures))):
        vx_orig = (test_bytes[2*i] if test_bytes[2*i] < 128 else test_bytes[2*i] - 256) * scale
        vy_orig = (test_bytes[2*i+1] if test_bytes[2*i+1] < 128 else test_bytes[2*i+1] - 256) * scale
        vx2, vy2 = tw_reconstruct_int_combined(captures[i])
        if vx2 == vx_orig and vy2 == vy_orig:
            roundtrip_ok += 1

    # Results
    print(f"\n  Results:")
    print(f"    Capture:     {t_capture/n_pairs*1e6:.1f} μs/pair ({n_pairs} pairs)")
    print(f"    Delta encode: {t_delta*1000:.1f} ms ({n_frames-1} frames)")
    print(f"    Roundtrip:   {roundtrip_ok}/{min(1000,n_pairs)} LOSSLESS")
    print(f"\n  Delta stats ({total_deltas} deltas):")
    print(f"    zone same:   {zone_same}/{total_deltas} ({zone_same/max(1,total_deltas)*100:.1f}%)")
    print(f"    slot same:   {slot_same}/{total_deltas} ({slot_same/max(1,total_deltas)*100:.1f}%)")
    print(f"    resid same:  {resid_same}/{total_deltas} ({resid_same/max(1,total_deltas)*100:.1f}%)")
    print(f"    FULL same:   {same_count}/{total_deltas} ({same_count/max(1,total_deltas)*100:.1f}%)")

    return {
        'name': os.path.basename(path),
        'n_frames': n_frames,
        'same_ratio': same_count/max(1,total_deltas),
        'zone_ratio': zone_same/max(1,total_deltas),
        'slot_ratio': slot_same/max(1,total_deltas),
        'resid_ratio': resid_same/max(1,total_deltas),
        'roundtrip': roundtrip_ok == min(1000, n_pairs),
    }


if __name__ == "__main__":
    # Test on local GGUF models
    models = [
        (r"I:\model\SmolLM2-360M-Instruct.Q8_0.gguf", 76800),
        (r"I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf", 76800),
        (r"I:\model\smolVLM-256M-Instruct-text.Q8_0.gguf", 76800),
    ]

    results = []
    for path, max_bytes in models:
        if os.path.exists(path):
            r = test_real_gguf(path, max_bytes)
            if r:
                results.append(r)

    # Summary
    print(f"\n{'='*60}")
    print("SUMMARY")
    print(f"{'='*60}")
    print(f"{'Model':<40} {'Frames':>6} {'Zone%':>6} {'Slot%':>6} {'Resid%':>7} {'Full%':>6} {'RT':>4}")
    print("-" * 80)
    for r in results:
        print(f"{r['name']:<40} {r['n_frames']:>6} {r['zone_ratio']*100:>5.1f}% {r['slot_ratio']*100:>5.1f}% {r['resid_ratio']*100:>6.1f}% {r['same_ratio']*100:>5.1f}% {'✓' if r['roundtrip'] else '✗':>4}")
