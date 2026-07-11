#!/usr/bin/env python3
"""
GeoPixel Minimal — encode data as SVG with base64 text.

This is the simplest approach: base64 data as SVG text.
Works well with LLMs - they can read the base64 and decode.

Usage:
    python geopixel_minimal.py <input_file> [output.svg]
"""

import sys, os, base64, hashlib, json
from pathlib import Path

def encode_file(filepath: str, output: str = None):
    """Encode file as minimal SVG with base64 data."""
    path = Path(filepath)
    if not path.exists():
        print(f"Error: {filepath} not found")
        return
    
    data = path.read_bytes()
    b64 = base64.b64encode(data).decode()
    metadata = {
        'filename': path.name,
        'size': len(data),
        'checksum': hashlib.sha256(data).hexdigest()[:16]
    }
    
    # Create minimal SVG with base64 as text
    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="400" height="300">
<rect width="400" height="300" fill="#0a0a14"/>
<text x="10" y="20" fill="#E74C3C" font-family="monospace" font-size="10">GeoPixel | {path.name} | {len(data)}B</text>
<text x="10" y="40" fill="#2ECC71" font-family="monospace" font-size="8">{b64}</text>
<!-- META:{json.dumps(metadata)} -->
</svg>'''
    
    if output is None:
        output = path.with_suffix('.svg').name
    
    outpath = Path(output)
    outpath.write_text(svg, encoding='utf-8')
    
    print(f"✓ Encoded: {path.name} ({len(data)} bytes) → {output}")
    print(f"  Base64: {len(b64)} chars")
    print(f"  SVG: {len(svg)} bytes")
    print(f"  Ratio: {len(svg)/len(data):.1f}x")

def main():
    if len(sys.argv) < 2:
        print("Usage: python geopixel_minimal.py <file> [output.svg]")
        return
    
    filepath = sys.argv[1]
    output = sys.argv[2] if len(sys.argv) > 2 else None
    encode_file(filepath, output)

if __name__ == '__main__':
    main()
