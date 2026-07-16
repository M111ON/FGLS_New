#!/usr/bin/env python3
"""
GeoPixel Encoder v3 — practical compact encoding.

Uses base64 text inside SVG for compact representation.
Each character maps to a geometric pattern.

Usage:
    python geopixel_encode_v3.py <input_file> [output.svg]
"""

import sys, os, base64, hashlib, json
from pathlib import Path

# ── Character to pattern mapping ──
CHARS = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz+/'

def char_to_pattern(char, x, y, size=6):
    """Convert a character to geometric pattern."""
    idx = CHARS.index(char) if char in CHARS else 0
    
    # Color based on character
    colors = ['#E74C3C', '#E67E22', '#F1C40F', '#2ECC71', '#3498DB', '#9B59B6', '#1ABC9C', '#E91E63']
    color = colors[idx % 8]
    
    # Pattern shape based on value
    patterns = []
    
    # Simple rectangular pattern
    patterns.append(f'<rect x="{x}" y="{y}" width="{size}" height="{size}" fill="{color}" opacity="0.8"/>')
    
    # Add detail for higher values
    if idx > 16:
        patterns.append(f'<rect x="{x+1}" y="{y+1}" width="{size-2}" height="{size-2}" fill="#0a0a14" opacity="0.5"/>')
    
    return '\n'.join(patterns)

def data_to_svg(data: bytes, metadata: dict) -> str:
    """Encode data as compact SVG with character patterns."""
    # Convert to base64
    b64 = base64.b64encode(data).decode()
    
    # Calculate grid
    cols = 32
    rows = (len(b64) + cols - 1) // cols
    cell_size = 8
    width = cols * cell_size + 20
    height = rows * cell_size + 50
    
    # Start SVG
    svg = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}">',
        f'<rect width="{width}" height="{height}" fill="#0a0a14"/>',
        f'<text x="10" y="15" fill="#E74C3C" font-family="monospace" font-size="10">',
        f'GeoPixel v3 | {metadata.get("filename", "data")} | {len(data)}B',
        f'</text>',
    ]
    
    # Draw character patterns
    for i, char in enumerate(b64):
        row = i // cols
        col = i % cols
        x = col * cell_size + 10
        y = row * cell_size + 25
        
        svg.append(char_to_pattern(char, x, y, cell_size))
    
    # Add metadata
    meta_b64 = base64.b64encode(json.dumps(metadata).encode()).decode()
    svg.append(f'<!-- META:{meta_b64} -->')
    
    svg.append('</svg>')
    return '\n'.join(svg)

def encode_file(filepath: str, output: str = None):
    """Encode a single file to SVG."""
    path = Path(filepath)
    if not path.exists():
        print(f"Error: {filepath} not found")
        return
    
    data = path.read_bytes()
    metadata = {
        'filename': path.name,
        'size': len(data),
        'type': 'file',
        'checksum': hashlib.sha256(data).hexdigest()[:16]
    }
    
    svg = data_to_svg(data, metadata)
    
    if output is None:
        output = path.with_suffix('.svg').name
    
    outpath = Path(output)
    outpath.write_text(svg, encoding='utf-8')
    
    print(f"✓ Encoded: {path.name} ({len(data)} bytes) → {output}")
    print(f"  Base64 length: {len(base64.b64encode(data))}")
    print(f"  SVG size: {len(svg)} bytes")
    print(f"  Ratio: {len(svg)/len(data):.1f}x")

def main():
    if len(sys.argv) < 2:
        print("Usage:")
        print("  python geopixel_encode_v3.py <file> [output.svg]")
        return
    
    filepath = sys.argv[1]
    output = sys.argv[2] if len(sys.argv) > 2 else None
    encode_file(filepath, output)

if __name__ == '__main__':
    main()
