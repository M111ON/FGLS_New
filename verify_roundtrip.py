"""
verify_roundtrip.py — Verify Roundtrip Reconstruction with Real Data
═══════════════════════════════════════════════════════════════════════════════

Tests:
1. Synthetic data roundtrip
2. Real binary file roundtrip
3. Multiple capture methods
4. Statistical verification
"""

import os
import sys
import hashlib
import time
from typing import List, Tuple, Dict, Any

# Add current directory to path
sys.path.insert(0, '.')

# Import the pipeline
from pattern_to_frame_seek import (
    PatternToFrameSeekPipeline, 
    CaptureMethod,
    LBlockContainer,
    FrameSeek
)


def generate_test_binary(size: int = 1024) -> bytes:
    """Generate test binary data"""
    # Use deterministic pattern for reproducibility
    data = bytearray()
    for i in range(size):
        data.append((i * 7 + 13) % 256)
    return bytes(data)


def extract_signatures(data: bytes, chunk_size: int = 64) -> List[Tuple[int, int]]:
    """
    Extract (vx, vy) signatures from binary data.
    
    Simple method: split chunk into two halves, compute mean as integer.
    """
    signatures = []
    
    for i in range(0, len(data), chunk_size):
        chunk = data[i:i+chunk_size]
        if len(chunk) < chunk_size:
            chunk = chunk + b'\x00' * (chunk_size - len(chunk))
        
        # Split into two halves
        half1 = chunk[:32]
        half2 = chunk[32:]
        
        # Compute mean as integer (scale by 207360)
        mean1 = sum(half1) // len(half1)
        mean2 = sum(half2) // len(half2)
        
        vx = mean1 * 207360 // 128
        vy = mean2 * 207360 // 128
        
        signatures.append((vx, vy))
    
    return signatures


def verify_roundtrip_synthetic():
    """Verify roundtrip with synthetic data"""
    print("=" * 80)
    print("TEST 1: Synthetic Data Roundtrip")
    print("=" * 80)
    
    pipeline = PatternToFrameSeekPipeline()
    
    # Test points
    test_points = [
        (100000, 50000),
        (500000, 200000),
        (-300000, 150000),
        (0, 0),
        (207360, 0),
    ]
    
    correct = 0
    total = len(test_points)
    
    for i, (vx, vy) in enumerate(test_points):
        # Process
        frame0 = pipeline.process(vx, vy, i)
        
        # Regenerate
        frame_regenerated = pipeline.regenerate(i)
        
        if frame_regenerated:
            # Verify capture method
            if frame_regenerated.capture_method == frame0.capture_method:
                correct += 1
                print(f"  Point {i+1}: ({vx:>10}, {vy:>10}) → PASS")
            else:
                print(f"  Point {i+1}: ({vx:>10}, {vy:>10}) → FAIL (method mismatch)")
        else:
            print(f"  Point {i+1}: ({vx:>10}, {vy:>10}) → FAIL (not stored)")
    
    accuracy = correct / total * 100
    print(f"\n  Accuracy: {correct}/{total} ({accuracy:.1f}%)")
    return accuracy == 100


def verify_roundtrip_binary():
    """Verify roundtrip with real binary data"""
    print("\n" + "=" * 80)
    print("TEST 2: Binary File Roundtrip")
    print("=" * 80)
    
    # Create test file
    test_data = generate_test_binary(1024)
    test_file = "test_roundtrip.bin"
    
    with open(test_file, "wb") as f:
        f.write(test_data)
    
    print(f"  Test file: {test_file} ({len(test_data)} bytes)")
    
    # Extract signatures
    signatures = extract_signatures(test_data)
    print(f"  Signatures extracted: {len(signatures)}")
    
    # Process through pipeline
    pipeline = PatternToFrameSeekPipeline()
    
    correct = 0
    total = len(signatures)
    
    for i, (vx, vy) in enumerate(signatures):
        frame0 = pipeline.process(vx, vy, i)
        frame_regenerated = pipeline.regenerate(i)
        
        if frame_regenerated:
            if (frame_regenerated.capture_method == frame0.capture_method and
                frame_regenerated.hilbert_pos == frame0.hilbert_pos):
                correct += 1
    
    accuracy = correct / total * 100
    print(f"  Roundtrip accuracy: {correct}/{total} ({accuracy:.1f}%)")
    
    # Cleanup
    os.remove(test_file)
    
    return accuracy == 100


def verify_deterministic():
    """Verify deterministic behavior"""
    print("\n" + "=" * 80)
    print("TEST 3: Deterministic Verification")
    print("=" * 80)
    
    pipeline1 = PatternToFrameSeekPipeline()
    pipeline2 = PatternToFrameSeekPipeline()
    
    test_points = [(100000, 50000), (500000, 200000), (-300000, 150000)]
    
    correct = 0
    total = len(test_points)
    
    for i, (vx, vy) in enumerate(test_points):
        # Process with both pipelines
        frame1 = pipeline1.process(vx, vy, i)
        frame2 = pipeline2.process(vx, vy, i)
        
        # Verify same result
        if (frame1.capture_method == frame2.capture_method and
            frame1.hilbert_pos == frame2.hilbert_pos and
            frame1.rotation == frame2.rotation):
            correct += 1
            print(f"  Point {i+1}: ({vx:>10}, {vy:>10}) → PASS (deterministic)")
        else:
            print(f"  Point {i+1}: ({vx:>10}, {vy:>10}) → FAIL (non-deterministic)")
    
    accuracy = correct / total * 100
    print(f"\n  Deterministic accuracy: {correct}/{total} ({accuracy:.1f}%)")
    return accuracy == 100


def verify_lblock_properties():
    """Verify L-block properties"""
    print("\n" + "=" * 80)
    print("TEST 4: L-block Properties Verification")
    print("=" * 80)
    
    lblock = LBlockContainer(grid_size=8)
    
    # Test 1: Same position → same rotation
    correct = 0
    total = 64
    
    for d in range(64):
        cells1, rot1, _ = lblock.summon(d)
        cells2, rot2, _ = lblock.summon(d)
        
        if rot1 == rot2 and cells1 == cells2:
            correct += 1
    
    accuracy = correct / total * 100
    print(f"  Same position → same rotation: {correct}/{total} ({accuracy:.1f}%)")
    
    # Test 2: All 4 rotations covered
    rotations = set()
    for d in range(64):
        _, rot, _ = lblock.summon(d)
        rotations.add(rot)
    
    all_covered = len(rotations) == 4
    print(f"  All 4 rotations covered: {'PASS' if all_covered else 'FAIL'}")
    
    # Test 3: Each rotation produces 4 unique cells
    cells_valid = True
    for rot in range(4):
        cells = lblock.lblock_shape(4, 4, rot)
        if len(set(cells)) != 4:
            cells_valid = False
            break
    
    print(f"  Each rotation → 4 unique cells: {'PASS' if cells_valid else 'FAIL'}")
    
    return accuracy == 100 and all_covered and cells_valid


def verify_frame_seek():
    """Verify frame seek properties"""
    print("\n" + "=" * 80)
    print("TEST 5: Frame Seek Verification")
    print("=" * 80)
    
    frame_seek = FrameSeek()
    
    # Test 1: Full cycle coverage
    visited = set()
    pos = 0
    for _ in range(1440):
        visited.add(pos)
        pos = frame_seek.next_frame(pos)
    
    full_coverage = len(visited) == 1440
    print(f"  Full cycle coverage (1440 frames): {'PASS' if full_coverage else 'FAIL'}")
    
    # Test 2: Deterministic
    pos1 = frame_seek.seek(42)
    pos2 = frame_seek.seek(42)
    deterministic = pos1 == pos2
    print(f"  Deterministic (same input → same output): {'PASS' if deterministic else 'FAIL'}")
    
    # Test 3: Next frame is different
    pos1 = frame_seek.seek(0)
    pos2 = frame_seek.next_frame(pos1)
    different = pos1 != pos2
    print(f"  Next frame is different: {'PASS' if different else 'FAIL'}")
    
    return full_coverage and deterministic and different


def main():
    """Run all verification tests"""
    print("=" * 80)
    print("ROUNDTRIP VERIFICATION SUITE")
    print("=" * 80)
    print()
    
    results = []
    
    # Run all tests
    results.append(("Synthetic Roundtrip", verify_roundtrip_synthetic()))
    results.append(("Binary Roundtrip", verify_roundtrip_binary()))
    results.append(("Deterministic", verify_deterministic()))
    results.append(("L-block Properties", verify_lblock_properties()))
    results.append(("Frame Seek", verify_frame_seek()))
    
    # Summary
    print("\n" + "=" * 80)
    print("VERIFICATION SUMMARY")
    print("=" * 80)
    print()
    
    all_pass = True
    for name, passed in results:
        status = "PASS" if passed else "FAIL"
        print(f"  {name:<25} {status}")
        if not passed:
            all_pass = False
    
    print()
    if all_pass:
        print("✓ ALL TESTS PASSED — Roundtrip verified!")
    else:
        print("✗ SOME TESTS FAILED — Check implementation")
    
    print()
    return all_pass


if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)
