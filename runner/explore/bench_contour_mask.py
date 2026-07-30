"""
bench_contour_mask.py — Performance benchmark for Contour Mask displacement model
Tests: encode, decode, XOR read, accuracy, storage
"""

import time
import random

def bench_contour_mask():
    print("=" * 70)
    print("Contour Mask Performance Benchmark")
    print("=" * 70)
    
    # === Structure ===
    CUBES = 10
    FACES = 6
    GRID = 10
    UNITS = CUBES * FACES * GRID * GRID  # 6000
    MAX_WEIGHT = 127
    MIN_WEIGHT = -128
    
    print(f"\n[Structure]")
    print(f"  Units: {UNITS}")
    print(f"  Weight range: [{MIN_WEIGHT}, {MAX_WEIGHT}]")
    
    # === Test Data ===
    N = 100000  # samples
    random.seed(42)
    weights = [random.randint(MIN_WEIGHT, MAX_WEIGHT) for _ in range(N)]
    
    # === Benchmark 1: Encode (weight → displacement) ===
    print(f"\n[Benchmark 1: Encode]")
    print(f"  Operation: weight → displacement from 0")
    
    positions = [0] * N
    start = time.perf_counter()
    for i in range(N):
        positions[i] = weights[i]  # displacement = weight
    encode_time = time.perf_counter() - start
    
    print(f"  Time: {encode_time*1e6/N:.2f} µs/sample")
    print(f"  Throughput: {N/encode_time/1e6:.2f} M samples/sec")
    
    # === Benchmark 2: Decode (displacement → weight via XOR 0) ===
    print(f"\n[Benchmark 2: Decode]")
    print(f"  Operation: XOR(displacement, 0) → weight")
    
    decoded = [0] * N
    start = time.perf_counter()
    for i in range(N):
        decoded[i] = positions[i] ^ 0  # XOR with 0
    decode_time = time.perf_counter() - start
    
    print(f"  Time: {decode_time*1e6/N:.2f} µs/sample")
    print(f"  Throughput: {N/decode_time/1e6:.2f} M samples/sec")
    
    # === Benchmark 3: XOR Read (diff between two units) ===
    print(f"\n[Benchmark 3: XOR Read]")
    print(f"  Operation: XOR(pos1, pos2) → diff")
    
    # Generate second set of weights
    weights2 = [random.randint(MIN_WEIGHT, MAX_WEIGHT) for _ in range(N)]
    positions2 = weights2.copy()
    
    xor_results = [0] * N
    start = time.perf_counter()
    for i in range(N):
        xor_results[i] = positions[i] ^ positions2[i]
    xor_time = time.perf_counter() - start
    
    print(f"  Time: {xor_time*1e6/N:.2f} µs/sample")
    print(f"  Throughput: {N/xor_time/1e6:.2f} M samples/sec")
    
    # === Benchmark 4: Recovery from XOR + known position ===
    print(f"\n[Benchmark 4: Recovery]")
    print(f"  Operation: XOR(pos1, xor_result) → pos2")
    
    recovered = [0] * N
    start = time.perf_counter()
    for i in range(N):
        recovered[i] = positions[i] ^ xor_results[i]
    recovery_time = time.perf_counter() - start
    
    print(f"  Time: {recovery_time*1e6/N:.2f} µs/sample")
    print(f"  Throughput: {N/recovery_time/1e6:.2f} M samples/sec")
    
    # === Benchmark 5: 15 Pairs Parallel ===
    print(f"\n[Benchmark 5: 15 Pairs Parallel]")
    print(f"  Operation: 15 XOR operations per sample")
    
    # Generate 6 face weights
    face_weights = {}
    for f in range(6):
        face_weights[f] = [random.randint(MIN_WEIGHT, MAX_WEIGHT) for _ in range(N)]
    
    pairs = []
    for i in range(6):
        for j in range(i+1, 6):
            pairs.append((i, j))
    
    start = time.perf_counter()
    for a, b in pairs:
        for i in range(N):
            _ = face_weights[a][i] ^ face_weights[b][i]
    parallel_time = time.perf_counter() - start
    
    print(f"  Pairs: {len(pairs)}")
    print(f"  Total XOR: {len(pairs) * N}")
    print(f"  Time: {parallel_time*1e6/(len(pairs)*N):.2f} µs/XOR")
    print(f"  Throughput: {len(pairs)*N/parallel_time/1e6:.2f} M XOR/sec")
    
    # === Benchmark 6: Accuracy ===
    print(f"\n[Benchmark 6: Accuracy]")
    
    exact_match = sum(1 for i in range(N) if decoded[i] == weights[i])
    recovery_match = sum(1 for i in range(N) if recovered[i] == weights2[i])
    
    print(f"  Decode accuracy: {exact_match}/{N} ({exact_match/N*100:.2f}%)")
    print(f"  Recovery accuracy: {recovery_match}/{N} ({recovery_match/N*100:.2f}%)")
    
    # === Storage ===
    print(f"\n[Storage]")
    storage_bytes = UNITS * 1  # int8 per unit
    storage_bits_per_weight = 8
    
    print(f"  Units: {UNITS}")
    print(f"  Per unit: 1 byte (int8)")
    print(f"  Total: {storage_bytes} bytes ({storage_bytes/1024:.1f} KB)")
    print(f"  Bits per weight: {storage_bits_per_weight}")
    
    # === Comparison with Identity Model ===
    print(f"\n[Comparison with Identity Model]")
    print(f"  Identity: weight → store(weight) = O(1)")
    print(f"  Displacement: weight → position = O(1)")
    print(f"  XOR read: XOR(pos1, pos2) = O(1)")
    print()
    print(f"  Identity decode: memory[position] = O(1)")
    print(f"  Displacement decode: XOR(position, 0) = O(1)")
    print()
    print(f"  Both are O(1) — same speed")
    
    # === Summary ===
    print(f"\n{'='*70}")
    print(f"SUMMARY")
    print(f"{'='*70}")
    print(f"  Encode:    {encode_time*1e6/N:.2f} µs/sample")
    print(f"  Decode:    {decode_time*1e6/N:.2f} µs/sample")
    print(f"  XOR read:  {xor_time*1e6/N:.2f} µs/sample")
    print(f"  Recovery:  {recovery_time*1e6/N:.2f} µs/sample")
    print(f"  15-pair:   {parallel_time*1e6/(len(pairs)*N):.2f} µs/XOR")
    print()
    print(f"  Accuracy:  100% (lossless)")
    print(f"  Storage:   {storage_bytes/1024:.1f} KB ({UNITS} units)")
    print(f"  Bits/weight: {storage_bits_per_weight}")
    print()
    print(f"  Key insight: displacement model is O(1) for all operations")
    print(f"  = same speed as identity model")
    print(f"  = but with physical meaning (displacement from 0)")

if __name__ == "__main__":
    bench_contour_mask()
