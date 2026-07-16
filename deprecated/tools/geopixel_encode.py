#!/usr/bin/env python3
"""
GeoPixel Encoder — encode files as SVG geometric patterns.

Usage:
    python geopixel_encode.py <input_file> [output.svg]
    python geopixel_encode.py --folder <input_folder> [output.svg]
"""

import sys, os, base64, hashlib, json
from pathlib import Path

# ── Config ──
CELL_SIZE = 12
GRID_COLS = 32
PATTERN_SIZE = 64  # bits per chunk

# ── Colors based on data value ──
COLORS = [
    '#E74C3C', '#E67E22', '#F1C40F', '#2ECC71',
    '#3498DB', '#9B59B6', '#1ABC9C', '#E91E63'
]

def file_to_chunks(data: bytes, chunk_size: int = PATTERN_SIZE):
    """Convert bytes to chunks of bits."""
    chunks = []
    for i in range(0, len(data), chunk_size):
        chunk = data[i:i+chunk_size]
        bits = []
        for byte in chunk:
            for bit in range(7, -1, -1):
                bits.append((byte >> bit) & 1)
        # pad to chunk_size
        while len(bits) < chunk_size:
            bits.append(0)
        chunks.append(bits[:chunk_size])
    return chunks

def bits_to_color(bits):
    """Determine color based on bit pattern."""
    val = sum(b << i for i, b in enumerate(bits[:8]))
    return COLORS[val % len(COLORS)]

def encode_svg(data: bytes, metadata: dict) -> str:
    """Encode data as SVG with geometric patterns."""
    chunks = file_to_chunks(data)
    n_chunks = len(chunks)
    
    # Calculate grid dimensions
    grid_rows = (n_chunks + GRID_COLS - 1) // GRID_COLS
    width = GRID_COLS * (CELL_SIZE + 2) + 40
    height = grid_rows * (CELL_SIZE + 2) + 80
    
    # Start SVG
    svg_parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        f'<rect width="{width}" height="{height}" fill="#0D0D0D"/>',
        f'<rect x="2" y="2" width="{width-4}" height="{height-4}" rx="6" fill="none" stroke="#E74C3C" stroke-width="2" opacity="0.6"/>',
        f'<text x="20" y="25" fill="#E74C3C" font-family="monospace" font-size="10">GeoPixel v1 | {metadata.get("filename", "data")} | {len(data)} bytes | {n_chunks} chunks</text>',
    ]
    
    # Draw patterns
    for idx, bits in enumerate(chunks):
        row = idx // GRID_COLS
        col = idx % GRID_COLS
        x = col * (CELL_SIZE + 2) + 20
        y = row * (CELL_SIZE + 2) + 40
        
        color = bits_to_color(bits)
        
        # Draw 8x8 grid for each chunk
        for bit_idx, bit in enumerate(bits):
            br = bit_idx // 8
            bc = bit_idx % 8
            cx = x + bc * (CELL_SIZE // 2)
            cy = y + br * (CELL_SIZE // 2)
            
            if bit:
                svg_parts.append(
                    f'<rect x="{cx}" y="{cy}" width="{CELL_SIZE//2 - 1}" height="{CELL_SIZE//2 - 1}" fill="{color}" opacity="0.8"/>'
                )
    
    # Add metadata as comment
    meta_json = json.dumps(metadata, separators=(',', ':'))
    svg_parts.append(f'<!-- METADATA:{meta_json} -->')
    
    svg_parts.append('</svg>')
    return '\n'.join(svg_parts)

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
    
    svg = encode_svg(data, metadata)
    
    if output is None:
        output = path.with_suffix('.svg').name
    
    outpath = Path(output)
    outpath.write_text(svg, encoding='utf-8')
    
    print(f"✓ Encoded: {path.name} ({len(data)} bytes) → {output}")
    print(f"  Chunks: {len(file_to_chunks(data))}")
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
    
    svg = encode_svg(combined, metadata)
    
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
        print("  python geopixel_encode.py <file> [output.svg]")
        print("  python geopixel_encode.py --folder <folder> [output.svg]")
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
