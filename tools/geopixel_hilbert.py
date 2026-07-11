#!/usr/bin/env python3
"""
GeoPixel Hilbert Encoder — encode files using Hilbert curve ordering.

This is the simplest Hilbert-based encoding:
1. Convert file to base64
2. Arrange base64 characters in 2D grid following Hilbert curve
3. Output as SVG with visual pattern

The key insight: Hilbert curve preserves spatial locality,
so similar characters are placed near each other in the 2D grid.

Usage:
    python geopixel_hilbert.py <input_file> [output.svg]
"""

import sys, os, base64, hashlib, json, math
from pathlib import Path

# ── Hilbert Curve Functions ──

def hilbert_d2xy(n, d):
    """Convert d (distance along curve) to (x,y) on n×n Hilbert curve."""
    x = y = 0
    t = d
    s = 1
    while s < n:
        rx = 1 & (t // 2)
        ry = 1 & (t ^ rx)
        if ry == 0:
            if rx == 1:
                x = s - 1 - x
                y = s - 1 - y
            x, y = y, x
        x += s * rx
        y += s * ry
        t //= 4
        s *= 2
    return x, y

def hilbert_xy2d(n, x, y):
    """Convert (x,y) to d (distance along curve) on n×n Hilbert curve."""
    d = 0
    s = n // 2
    while s > 0:
        rx = 1 if (x & s) > 0 else 0
        ry = 1 if (y & s) > 0 else 0
        d += s * s * ((3 * rx) ^ ry)
        if ry == 0:
            if rx == 1:
                x = s - 1 - x
                y = s - 1 - y
            x, y = y, x
        s //= 2
    return d

def next_pow2(x):
    """Next power of 2 >= x."""
    return 1 << (x - 1).bit_length()

# ── Character to Color Mapping ──

CHARS = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz+/='

def char_to_color(char):
    """Map character to color."""
    if char not in CHARS:
        char = CHARS[0]
    idx = CHARS.index(char)
    colors = [
        '#E74C3C', '#E67E22', '#F1C40F', '#2ECC71',
        '#3498DB', '#9B59B6', '#1ABC9C', '#E91E63',
        '#FF5722', '#795548', '#607D8B', '#00BCD4',
        '#8BC34A', '#CDDC39', '#FFC107', '#FF9800'
    ]
    return colors[idx % len(colors)]

# ── Encoder ──

def encode_hilbert(data: bytes, metadata: dict) -> str:
    """Encode data as SVG with Hilbert curve ordering."""
    # Convert to base64
    b64 = base64.b64encode(data).decode()
    n_chars = len(b64)
    
    # Calculate grid size (next power of 2 for Hilbert)
    grid_size = next_pow2(math.ceil(math.sqrt(n_chars)))
    total_cells = grid_size * grid_size
    
    # Place characters along Hilbert curve
    grid = {}
    for i, char in enumerate(b64):
        x, y = hilbert_d2xy(grid_size, i)
        grid[(x, y)] = char
    
    # Calculate SVG dimensions
    cell_size = 8
    width = grid_size * cell_size + 20
    height = grid_size * cell_size + 50
    
    # Start SVG
    svg = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}">',
        f'<rect width="{width}" height="{height}" fill="#0a0a14"/>',
        f'<text x="10" y="15" fill="#E74C3C" font-family="monospace" font-size="10">',
        f'GeoPixel Hilbert | {metadata.get("filename", "data")} | {len(data)}B | grid={grid_size}×{grid_size}',
        f'</text>',
    ]
    
    # Draw grid cells
    for (x, y), char in grid.items():
        px = x * cell_size + 10
        py = y * cell_size + 25
        color = char_to_color(char)
        svg.append(f'<rect x="{px}" y="{py}" width="{cell_size-1}" height="{cell_size-1}" fill="{color}" opacity="0.85"/>')
    
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
        'checksum': hashlib.sha256(data).hexdigest()[:16],
        'encoding': 'hilbert'
    }
    
    svg = encode_hilbert(data, metadata)
    
    if output is None:
        output = path.with_suffix('.svg').name
    
    outpath = Path(output)
    outpath.write_text(svg, encoding='utf-8')
    
    print(f"✓ Encoded: {path.name} ({len(data)} bytes) → {output}")
    print(f"  Base64: {len(base64.b64encode(data))} chars")
    print(f"  Grid: {next_pow2(math.ceil(math.sqrt(len(base64.b64encode(data)))))}×{next_pow2(math.ceil(math.sqrt(len(base64.b64encode(data)))))}")
    print(f"  SVG: {len(svg)} bytes")
    print(f"  Ratio: {len(svg)/len(data):.1f}x")

def main():
    if len(sys.argv) < 2:
        print("Usage:")
        print("  python geopixel_hilbert.py <file> [output.svg]")
        return
    
    filepath = sys.argv[1]
    output = sys.argv[2] if len(sys.argv) > 2 else None
    encode_file(filepath, output)

if __name__ == '__main__':
    main()
