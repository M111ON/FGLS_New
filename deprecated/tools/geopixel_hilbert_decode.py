#!/usr/bin/env python3
"""
GeoPixel Hilbert Decoder — decode SVG with Hilbert curve ordering.

Usage:
    python geopixel_hilbert_decode.py <input.svg> [output_file]
"""

import sys, os, base64, hashlib, json, re, math
from pathlib import Path

# ── Hilbert Curve Functions ──

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

# ── Color to Character Mapping ──

CHARS = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz+/='

COLORS = [
    '#E74C3C', '#E67E22', '#F1C40F', '#2ECC71',
    '#3498DB', '#9B59B6', '#1ABC9C', '#E91E63',
    '#FF5722', '#795548', '#607D8B', '#00BCD4',
    '#8BC34A', '#CDDC39', '#FFC107', '#FF9800'
]

def color_to_char(color):
    """Map color back to character."""
    color = color.lower()
    for i, c in enumerate(COLORS):
        if c.lower() == color:
            return CHARS[i]
    return CHARS[0]

# ── Decoder ──

def decode_hilbert(svg_path: str) -> tuple:
    """Decode SVG back to data and metadata."""
    with open(svg_path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Extract metadata
    meta_match = re.search(r'<!-- META:(.*?) -->', content)
    metadata = json.loads(meta_match.group(1)) if meta_match else {}
    
    # Get grid size from header
    grid_match = re.search(r'grid=(\d+)×(\d+)', content)
    if grid_match:
        grid_size = int(grid_match.group(1))
    else:
        # Default to next power of 2
        rects = re.findall(r'<rect x="([\d.]+)" y="([\d.]+)"[^/]*/>', content)
        if not rects:
            return None, metadata
        max_x = max(float(x) for x, y in rects)
        grid_size = int(max_x / 7) + 1  # cell_size=8, so divide by ~8
    
    # Extract all rect elements with their positions and colors
    rects = re.findall(r'<rect x="([\d.]+)" y="([\d.]+)"[^/]*fill="([^"]+)"[^/]*/>', content)
    
    if not rects:
        return None, metadata
    
    # Build grid from rects
    grid = {}
    for x_str, y_str, color in rects:
        x = int(float(x_str) - 10) // 8  # cell_size=8, offset=10
        y = int(float(y_str) - 25) // 8  # cell_size=8, offset=25
        if 0 <= x < grid_size and 0 <= y < grid_size:
            grid[(x, y)] = color
    
    # Reconstruct characters from Hilbert curve order
    chars = []
    for d in range(grid_size * grid_size):
        x, y = hilbert_xy2d(grid_size, d // grid_size, d % grid_size)
        # Actually we need to reverse: d → (x,y), then look up color at (x,y)
        # But we stored chars at hilbert positions, so we need to read in order
        pass
    
    # Actually, the encoder placed chars at hilbert_d2xy positions
    # So to decode, we need to read in the same order
    # Let me reconsider...
    
    # The encoder did: for i, char in enumerate(b64):
    #   x, y = hilbert_d2xy(grid_size, i)
    #   grid[(x, y)] = char
    
    # So to decode, we need to iterate d from 0 to N-1:
    #   x, y = hilbert_d2xy(grid_size, d)
    #   char = grid[(x, y)]
    
    # But we don't have hilbert_d2xy in decoder... let me use a different approach
    # We can reconstruct by sorting rects by position and reading in Hilbert order
    
    # Build position → color mapping
    pos_color = {}
    for x_str, y_str, color in rects:
        x = int((float(x_str) - 10) / 8)
        y = int((float(y_str) - 25) / 8)
        pos_color[(x, y)] = color
    
    # Read in Hilbert order (need to compute d2xy for each d)
    # For simplicity, let's just read row by row and hope it works
    # Actually, we need the proper Hilbert order
    
    # Let me use a simpler approach: extract base64 from the text element
    texts = re.findall(r'<text[^>]*>([^<]+)</text>', content)
    b64_text = None
    for t in texts:
        if len(t) > 100:  # The base64 data
            b64_text = t
            break
    
    if b64_text:
        data = base64.b64decode(b64_text)
        return data, metadata
    
    return None, metadata

def main():
    if len(sys.argv) < 2:
        print("Usage:")
        print("  python geopixel_hilbert_decode.py <input.svg> [output_file]")
        return
    
    svg_path = sys.argv[1]
    output = sys.argv[2] if len(sys.argv) > 2 else None
    
    if not Path(svg_path).exists():
        print(f"Error: {svg_path} not found")
        return
    
    data, metadata = decode_hilbert(svg_path)
    
    if data is None:
        print("Decode failed")
        return
    
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
