#!/usr/bin/env python3
"""
GeoPixel Encoder v2 — compact SVG encoding.

Usage:
    python geopixel_encode_v2.py <input_file> [output.svg]
    python geopixel_encode_v2.py --folder <input_folder> [output.svg]
"""

import sys, os, base64, hashlib, json
from pathlib import Path

# ── Config ──
CELL_SIZE = 4
CHUNK_SIZE = 8  # bits per byte

def data_to_svg(data: bytes, metadata: dict) -> str:
    """Encode data as compact SVG."""
    # Convert to binary string
    bits = ''.join(format(b, '08b') for b in data)
    n_bits = len(bits)
    
    # Calculate grid
    cols = 64
    rows = (n_bits + cols - 1) // cols
    width = cols * CELL_SIZE + 20
    height = rows * CELL_SIZE + 40
    
    # Start SVG
    svg = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}">',
        f'<rect width="{width}" height="{height}" fill="#0a0a14"/>',
        f'<text x="10" y="15" fill="#E74C3C" font-family="monospace" font-size="8">',
        f'GeoPixel v2 | {metadata.get("filename", "data")} | {len(data)}B | {n_bits} bits',
        f'</text>',
    ]
    
    # Draw bits as rectangles
    for i, bit in enumerate(bits):
        if bit == '1':
            row = i // cols
            col = i % cols
            x = col * CELL_SIZE + 10
            y = row * CELL_SIZE + 25
            
            # Color based on position
            color_idx = (row * 3 + col * 7) % 8
            colors = ['#E74C3C', '#E67E22', '#F1C40F', '#2ECC71', '#3498DB', '#9B59B6', '#1ABC9C', '#E91E63']
            color = colors[color_idx]
            
            svg.append(f'<rect x="{x}" y="{y}" width="{CELL_SIZE-1}" height="{CELL_SIZE-1}" fill="{color}" opacity="0.8"/>')
    
    # Add metadata as base64 in comment
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
    print(f"  Bits: {len(data) * 8}")
    print(f"  SVG size: {len(svg)} bytes")
    print(f"  Ratio: {len(svg)/len(data):.1f}x")

def encode_folder(folderpath: str, output: str = None):
    """Encode an entire folder to SVG."""
    folder = Path(folderpath)
    if not folder.exists():
        print(f"Error: {folderpath} not found")
        return
    
    # Collect all files
    files = {}
    for f in folder.rglob('*'):
        if f.is_file():
            rel = f.relative_to(folder)
            files[str(rel)] = f.read_bytes()
    
    # Combine into single data
    combined = json.dumps(files).encode()
    metadata = {
        'filename': folder.name,
        'size': len(combined),
        'type': 'folder',
        'file_count': len(files),
        'checksum': hashlib.sha256(combined).hexdigest()[:16]
    }
    
    svg = data_to_svg(combined, metadata)
    
    if output is None:
        output = f"{folder.name}.svg"
    
    outpath = Path(output)
    outpath.write_text(svg, encoding='utf-8')
    
    print(f"✓ Encoded folder: {folder.name}")
    print(f"  Files: {len(files)}")
    print(f"  Total size: {len(combined)} bytes")
    print(f"  SVG size: {len(svg)} bytes")
    print(f"  Ratio: {len(svg)/len(combined):.1f}x")

def main():
    if len(sys.argv) < 2:
        print("Usage:")
        print("  python geopixel_encode_v2.py <file> [output.svg]")
        print("  python geopixel_encode_v2.py --folder <folder> [output.svg]")
        return
    
    if sys.argv[1] == '--folder':
        folder = sys.argv[2]
        output = sys.argv[3] if len(sys.argv) > 3 else None
        encode_folder(folder, output)
    else:
        filepath = sys.argv[1]
        output = sys.argv[2] if len(sys.argv) > 2 else None
        encode_file(filepath, output)

if __name__ == '__main__':
    main()
