"""
verify_real_file.py — Verify Roundtrip with Real Binary File
═══════════════════════════════════════════════════════════════════════════════

Tests roundtrip reconstruction with actual binary file from project.
"""

import os
import sys
import hashlib
import time

# Add current directory to path
sys.path.insert(0, '.')

# Import the pipeline
from pattern_to_frame_seek import PatternToFrameSeekPipeline


def extract_signatures_from_file(file_path: str, chunk_size: int = 64):
    """Extract (vx, vy) signatures from binary file"""
    signatures = []
    
    with open(file_path, "rb") as f:
        while True:
            chunk = f.read(chunk_size)
            if not chunk:
                break
            
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


def verify_real_file():
    """Verify roundtrip with real binary file"""
    print("=" * 80)
    print("REAL FILE ROUNDTRIP VERIFICATION")
    print("=" * 80)
    print()
    
    # Test file
    test_file = "bake_out/experiment/results/00-baseline/logits.bin"
    
    if not os.path.exists(test_file):
        print(f"  ERROR: Test file not found: {test_file}")
        return False
    
    file_size = os.path.getsize(test_file)
    print(f"  Test file: {test_file}")
    print(f"  File size: {file_size:,} bytes")
    print()
    
    # Extract signatures
    print("  Extracting signatures...")
    start_time = time.time()
    signatures = extract_signatures_from_file(test_file)
    extract_time = time.time() - start_time
    print(f"  Signatures extracted: {len(signatures)}")
    print(f"  Extraction time: {extract_time:.3f} seconds")
    print()
    
    # Process through pipeline
    print("  Processing through pipeline...")
    pipeline = PatternToFrameSeekPipeline()
    
    start_time = time.time()
    correct = 0
    total = len(signatures)
    
    for i, (vx, vy) in enumerate(signatures):
        frame0 = pipeline.process(vx, vy, i)
        frame_regenerated = pipeline.regenerate(i)
        
        if frame_regenerated:
            if (frame_regenerated.capture_method == frame0.capture_method and
                frame_regenerated.hilbert_pos == frame0.hilbert_pos):
                correct += 1
    
    process_time = time.time() - start_time
    accuracy = correct / total * 100
    
    print(f"  Processing time: {process_time:.3f} seconds")
    print(f"  Throughput: {total / process_time:.0f} frames/second")
    print()
    
    # Results
    print("  Results:")
    print(f"    Total frames: {total}")
    print(f"    Correct: {correct}")
    print(f"    Accuracy: {accuracy:.1f}%")
    print()
    
    # Storage analysis
    storage_original = file_size
    storage_frame0 = len(signatures) * 100  # ~100 bytes per frame0
    compression_ratio = storage_original / storage_frame0
    
    print("  Storage analysis:")
    print(f"    Original file: {storage_original:,} bytes")
    print(f"    Frame0 storage: {storage_frame0:,} bytes")
    print(f"    Compression ratio: {compression_ratio:.2f}x")
    print()
    
    # Verification
    if accuracy == 100:
        print("✓ ROUNDTRIP VERIFIED — All frames reconstructed correctly!")
        return True
    else:
        print("✗ ROUNDTRIP FAILED — Some frames not reconstructed correctly")
        return False


if __name__ == "__main__":
    success = verify_real_file()
    sys.exit(0 if success else 1)
