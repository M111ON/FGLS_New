#!/usr/bin/env python3
"""
GeoPixel Minimal Decoder — decode SVG with base64 data.

Usage:
    python geopixel_decode_minimal.py <input.svg> [output_file]
"""

import sys, os, base64, hashlib, json, re
from pathlib import Path

def decode_svg(svg_path: str) -> tuple:
    """Decode SVG back to data and metadata."""
    with open(svg_path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Extract metadata
    meta_match = re.search(r'<!-- META:(.*?) -->', content)
    metadata = json.loads(meta_match.group(1)) if meta_match else {}
    
    # Extract base64 data (the longest text element)
    texts = re.findall(r'<text[^>]*>([^<]+)</text>', content)
    b64 = max(texts, key=len) if texts else ''
    
    # Decode base64
    data = base64.b64decode(b64)
    
    return data, metadata

def main():
    if len(sys.argv) < 2:
        print("Usage: python geopixel_decode_minimal.py <input.svg> [output_file]")
        return
    
    svg_path = sys.argv[1]
    output = sys.argv[2] if len(sys.argv) > 2 else None
    
    if not Path(svg_path).exists():
        print(f"Error: {svg_path} not found")
        return
    
    data, metadata = decode_svg(svg_path)
    
    if output is None:
        output = metadata.get('filename', 'decoded')
    
    # Verify checksum
    if 'checksum' in metadata:
        actual = hashlib.sha256(data).hexdigest()[:16]
        if actual != metadata['checksum']:
            print(f"⚠ Checksum mismatch: {actual} != {metadata['checksum']}")
        else:
            print(f"✓ Checksum verified")
    
    # Save
    Path(output).write_bytes(data)
    print(f"✓ Decoded: {output} ({len(data)} bytes)")

if __name__ == '__main__':
    main()
