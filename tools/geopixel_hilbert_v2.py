#!/usr/bin/env python3
"""
GeoPixel Hilbert v2 — compact Hilbert encoding.

Simple approach: base64 text + Hilbert metadata for visualization.
Can be decoded by LLM or by our decoder.

Usage:
    python geopixel_hilbert_v2.py <input_file> [output.svg]
"""

import sys, os, base64, hashlib, json, math
from pathlib import Path

def next_pow2(x):
    return 1 << (x - 1).bit_length()

def encode_file(filepath: str, output: str = None):
    """Encode file as compact SVG with Hilbert metadata."""
    path = Path(filepath)
    if not path.exists():
        print(f"Error: {filepath} not found")
        return
    
    data = path.read_bytes()
    b64 = base64.b64encode(data).decode()
    n_chars = len(b64)
    
    # Calculate Hilbert grid
    grid_size = next_pow2(math.ceil(math.sqrt(n_chars)))
    
    metadata = {
        'filename': path.name,
        'size': len(data),
        'checksum': hashlib.sha256(data).hexdigest()[:16],
        'encoding': 'hilbert_v2',
        'grid_size': grid_size,
        'b64_length': n_chars
    }
    
    # Create minimal SVG with base64 as text
    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="400" height="300">
<rect width="400" height="300" fill="#0a0a14"/>
<text x="10" y="20" fill="#E74C3C" font-family="monospace" font-size="10">GeoPixel Hilbert v2 | {path.name} | {len(data)}B | grid={grid_size}×{grid_size}</text>
<text x="10" y="40" fill="#2ECC71" font-family="monospace" font-size="8">{b64}</text>
<!-- META:{json.dumps(metadata)} -->
</svg>'''
    
    if output is None:
        output = path.with_suffix('.svg').name
    
    outpath = Path(output)
    outpath.write_text(svg, encoding='utf-8')
    
    print(f"✓ Encoded: {path.name} ({len(data)} bytes) → {output}")
    print(f"  Base64: {n_chars} chars")
    print(f"  Grid: {grid_size}×{grid_size}")
    print(f"  SVG: {len(svg)} bytes")
    print(f"  Ratio: {len(svg)/len(data):.1f}x")

def main():
    if len(sys.argv) < 2:
        print("Usage:")
        print("  python geopixel_hilbert_v2.py <file> [output.svg]")
        return
    
    filepath = sys.argv[1]
    output = sys.argv[2] if len(sys.argv) > 2 else None
    encode_file(filepath, output)

if __name__ == '__main__':
    main()
