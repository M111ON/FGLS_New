"""Test: convert data to SVG sequence patterns, measure encoding result."""
import hashlib, math, os, struct, zlib, base64

CHUNK_SZ = 64

def make_data(kind='structured', size=100000):
    if kind == 'structured':
        pattern = bytes(range(256))
        return (pattern * 400)[:size]
    elif kind == 'random':
        return os.urandom(size)
    elif kind == 'source':
        return open(__file__, 'rb').read() * (size // len(open(__file__, 'rb').read()) + 1)
    elif kind == 'tensor':
        import random
        random.seed(42)
        data = bytearray()
        cluster = bytes(random.randint(0, 15) for _ in range(64))
        while len(data) < size:
            noisy = bytes((b + random.randint(-2, 2)) % 256 for b in cluster)
            data.extend(noisy)
            if len(data) % 256 == 0:
                cluster = bytes(random.randint(0, 15) for _ in range(64))
        return bytes(data[:size])
    return os.urandom(size)

def chunk_data(data):
    chunks = []
    for i in range(0, len(data), CHUNK_SZ):
        chunk = data[i:i+CHUNK_SZ]
        if len(chunk) < CHUNK_SZ:
            chunk = chunk + b'\x00' * (CHUNK_SZ - len(chunk))
        chunks.append(chunk)
    return chunks

def approach_svg_sequence(chunks, max_frames=64):
    """Each chunk = one SVG frame with colored cells."""
    frames = []
    for ci, chunk in enumerate(chunks[:max_frames]):
        cells = []
        for b in range(0, min(CHUNK_SZ, len(chunk)), 2):
            r, g = chunk[b], chunk[b+1] if b+1 < len(chunk) else 0
            cells.append(f'<rect x="{b//2*8}" y="0" width="8" height="8" fill="#{r:02x}{g:02x}00"/>')
        frame = '<svg xmlns="http://www.w3.org/2000/svg" width="256" height="8">' + ''.join(cells) + '</svg>'
        frames.append(frame)
    
    h = len(frames) * 12
    svg = f'<svg xmlns="http://www.w3.org/2000/svg" width="256" height="{h}">'
    for i, frame in enumerate(frames):
        inner = frame[frame.index('>')+1:frame.rindex('</svg>')]
        svg += f'<g transform="translate(0,{i*12})">{inner}</g>'
    svg += '</svg>'
    return svg.encode()

def approach_svg_grid(chunks, side=20):
    """All chunks in one SVG as a color grid."""
    cells = []
    for ci, chunk in enumerate(chunks[:side*side]):
        r, c = ci // side, ci % side
        h = hashlib.md5(chunk).digest()
        color = f'#{h[0]:02x}{h[1]:02x}{h[2]:02x}'
        cells.append(f'<rect x="{c*4}" y="{r*4}" width="4" height="4" fill="{color}"/>')
    svg = f'<svg xmlns="http://www.w3.org/2000/svg" width="{side*4}" height="{side*4}">' + ''.join(cells) + '</svg>'
    return svg.encode()

def approach_wallet_sequence(chunks, max_frames=16):
    """Each chunk = 8x8 grayscale wallet vector grid."""
    frames = []
    for ci, chunk in enumerate(chunks[:max_frames]):
        cells = []
        for r in range(8):
            for c in range(8):
                idx = r * 8 + c
                if idx < len(chunk):
                    v = chunk[idx]
                    color = f'#{v:02x}{v:02x}{v:02x}'
                else:
                    color = '#000000'
                cells.append(f'<rect x="{c*10}" y="{r*10}" width="10" height="10" fill="{color}"/>')
        frame = '<svg xmlns="http://www.w3.org/2000/svg" width="80" height="80">' + ''.join(cells) + '</svg>'
        frames.append(frame)
    
    h = len(frames) * 84
    svg = f'<svg xmlns="http://www.w3.org/2000/svg" width="80" height="{h}">'
    for i, frame in enumerate(frames):
        inner = frame[frame.index('>')+1:frame.rindex('</svg>')]
        svg += f'<g transform="translate(0,{i*84})">{inner}</g>'
    svg += '</svg>'
    return svg.encode()

def approach_vault_svg(data):
    """Data → zlib → base64 → SVG metadata."""
    compressed = zlib.compress(data, 9)
    b64 = base64.b64encode(compressed).decode()
    svg = f'<svg xmlns="http://www.w3.org/2000/svg"><metadata><vault:file size="{len(data)}">{b64}</vault:file></metadata></svg>'
    return svg.encode()

def approach_svgz_grid(chunks, side=20):
    """SVG grid → gzip → .svgz"""
    cells = []
    for ci, chunk in enumerate(chunks[:side*side]):
        r, c = ci // side, ci % side
        h = hashlib.md5(chunk).digest()
        color = f'#{h[0]:02x}{h[1]:02x}{h[2]:02x}'
        cells.append(f'<rect x="{c*4}" y="{r*4}" width="4" height="4" fill="{color}"/>')
    svg = f'<svg xmlns="http://www.w3.org/2000/svg" width="{side*4}" height="{side*4}">' + ''.join(cells) + '</svg>'
    return gzip.compress(svg.encode())

import gzip

for kind in ['structured', 'random', 'source', 'tensor']:
    data = make_data(kind, 100000)
    chunks = chunk_data(data)
    n = len(chunks)
    
    print(f'\n{"="*60}')
    print(f'  DATA: {kind.upper()} ({len(data):,} bytes, {n} chunks)')
    print(f'{"="*60}')
    
    # Raw zlib baseline
    raw_zlib = zlib.compress(data, 9)
    print(f'  [baseline] zlib alone:           {len(raw_zlib):>8,} bytes  ({len(data)/len(raw_zlib):.2f}x)')
    
    # SVG sequence (16 frames)
    svg_seq = approach_svg_sequence(chunks, 16)
    svg_seq_comp = zlib.compress(svg_seq, 9)
    print(f'  SVG sequence (16 frames):        {len(svg_seq):>8,} bytes  svg')
    print(f'  SVG sequence + zlib:             {len(svg_seq_comp):>8,} bytes  ({len(data)/len(svg_seq_comp):.2f}x)')
    
    # SVG grid (20x20)
    svg_grid = approach_svg_grid(chunks, 20)
    svg_grid_comp = zlib.compress(svg_grid, 9)
    print(f'  SVG grid (20x20):                {len(svg_grid):>8,} bytes  svg')
    print(f'  SVG grid + zlib:                 {len(svg_grid_comp):>8,} bytes  ({len(data)/len(svg_grid_comp):.2f}x)')
    
    # Wallet vector sequence (16 frames)
    wallet = approach_wallet_sequence(chunks, 16)
    wallet_comp = zlib.compress(wallet, 9)
    print(f'  Wallet vector (16 frames):       {len(wallet):>8,} bytes  svg')
    print(f'  Wallet vector + zlib:            {len(wallet_comp):>8,} bytes  ({len(data)/len(wallet_comp):.2f}x)')
    
    # Vault SVG (zlib+base64 in metadata)
    vault = approach_vault_svg(data)
    print(f'  Vault SVG (zlib in metadata):    {len(vault):>8,} bytes  ({len(data)/len(vault):.2f}x)')
    
    # SVG grid → gzip (.svgz)
    svgz = approach_svgz_grid(chunks, 20)
    print(f'  .svgz (gzip SVG grid):           {len(svgz):>8,} bytes  ({len(data)/len(svgz):.2f}x)')
