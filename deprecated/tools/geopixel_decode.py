#!/usr/bin/env python3
"""
GeoPixel Decoder — decode SVG geometric patterns back to files.

Usage:
    python geopixel_decode.py <input.svg> [output_file]
"""

import sys, os, base64, hashlib, json, re
from pathlib import Path

def decode_svg(svg_path: str) -> tuple:
    """Decode SVG back to data and metadata."""
    with open(svg_path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Extract metadata from comment
    meta_match = re.search(r'<!-- METADATA:(.*?) -->', content)
    if meta_match:
        metadata = json.loads(meta_match.group(1))
    else:
        metadata = {}
    
    # Extract all rect elements (the pattern cells)
    rects = re.findall(r'<rect x="([\d.]+)" y="([\d.]+)" width="([\d.]+)" height="([\d.]+)" fill="([^"]+)"[^/]*/>', content)
    
    if not rects:
        print("Error: No pattern data found in SVG")
        return None, metadata
    
    # Parse grid dimensions from header
    header_match = re.search(r'(\d+) chunks', content)
    n_chunks = int(header_match.group(1)) if header_match else len(rects)
    
    # Reconstruct bits from pattern
    # Group rects by position to determine grid
    cells = []
    for x, y, w, h, color in rects:
        cells.append((float(x), float(y), color))
    
    # Sort by position (y then x)
    cells.sort(key=lambda c: (c[1], c[0]))
    
    # Each chunk = 8 cells (8 bits)
    chunks = []
    for i in range(0, len(cells), 8):
        chunk_cells = cells[i:i+8]
        bits = []
        for _, _, color in chunk_cells:
            # Determine bit from color
            color_map = {
                '#E74C3C': [1,0,0,0,0,0,0,0],
                '#E67E22': [0,1,0,0,0,0,0,0],
                '#F1C40F': [0,0,1,0,0,0,0,0],
                '#2ECC71': [0,0,0,1,0,0,0,0],
                '#3498DB': [0,0,0,0,1,0,0,0],
                '#9B59B6': [0,0,0,0,0,1,0,0],
                '#1ABC9C': [0,0,0,0,0,0,1,0],
                '#E91E63': [0,0,0,0,0,0,0,1],
            }
            bits.extend(color_map.get(color, [0]*8))
        chunks.append(bits[:8])  # Take first 8 bits
    
    # Reconstruct bytes from bits
    data = bytearray()
    for chunk_bits in chunks:
        for i in range(0, len(chunk_bits), 8):
            byte_bits = chunk_bits[i:i+8]
            if len(byte_bits) == 8:
                byte = sum(b << (7-j) for j, b in enumerate(byte_bits))
                data.append(byte)
    
    return bytes(data), metadata

def main():
    if len(sys.argv) < 2:
        print("Usage:")
        print("  python geopixel_decode.py <input.svg> [output_file]")
        return
    
    svg_path = sys.argv[1]
    output = sys.argv[2] if len(sys.argv) > 2 else None
    
    if not Path(svg_path).exists():
        print(f"Error: {svg_path} not found")
        return
    
    data, metadata = decode_svg(svg_path)
    
    if data is None:
        print("Decode failed")
        return
    
    # Determine output path
    if output is None:
        if metadata.get('type') == 'folder':
            output = metadata.get('filename', 'decoded') + '.json'
        else:
            output = metadata.get('filename', 'decoded')
    
    # Verify checksum
    if 'checksum' in metadata:
        actual = hashlib.sha256(data).hexdigest()[:16]
        if actual != metadata['checksum']:
            print(f"⚠ Checksum mismatch: {actual} != {metadata['checksum']}")
        else:
            print(f"✓ Checksum verified: {metadata['checksum']}")
    
    # Save output
    outpath = Path(output)
    
    if metadata.get('type') == 'folder':
        # Decode folder
        files = json.loads(data)
        folder = outpath.with_suffix('')
        folder.mkdir(exist_ok=True)
        for filename, content in files.items():
            filepath = folder / filename
            filepath.parent.mkdir(parents=True, exist_ok=True)
            filepath.write_bytes(content.encode('latin-1') if isinstance(content, str) else content)
            print(f"  ✓ {filename}")
        print(f"✓ Decoded folder: {folder}")
    else:
        outpath.write_bytes(data)
        print(f"✓ Decoded: {output} ({len(data)} bytes)")

if __name__ == '__main__':
    main()
