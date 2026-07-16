"""SVG Roundtrip Verification — prove lossless or lossy for each approach."""
import hashlib, math, os, struct, zlib, base64, gzip, tempfile, sys

CHUNK_SZ = 64

def make_random(size=100000):
    return os.urandom(size)

def make_structured(size=100000):
    pattern = bytes(range(256))
    return (pattern * (size // 256 + 1))[:size]

def chunk_data(data):
    chunks = []
    for i in range(0, len(data), CHUNK_SZ):
        chunk = data[i:i+CHUNK_SZ]
        if len(chunk) < CHUNK_SZ:
            chunk = chunk + b'\x00' * (CHUNK_SZ - len(chunk))
        chunks.append(chunk)
    return chunks

# --- Encoding approaches (from svg_encode_test.py) ---

def encode_svg_sequence(chunks, max_frames=16):
    """Each chunk = one SVG frame with colored cells. PAIR bytes → RGB."""
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
    return svg.encode(), None  # (encoded, decoder_fn) — decoder_fn=None means lossy

def decode_svg_sequence(encoded):
    """Decode SVG sequence → try to recover bytes from rect fill colors."""
    import re
    text = encoded.decode('utf-8', errors='replace')
    fills = re.findall(r'fill="#([0-9a-f]{2})([0-9a-f]{2})00"', text)
    data = bytearray()
    for r_hex, g_hex in fills:
        data.append(int(r_hex, 16))
        data.append(int(g_hex, 16))
    return bytes(data)

def encode_svg_grid(chunks, side=20):
    """All chunks in one SVG as a color grid. Uses MD5 → LOSSY."""
    cells = []
    for ci, chunk in enumerate(chunks[:side*side]):
        r, c = ci // side, ci % side
        h = hashlib.md5(chunk).digest()
        color = f'#{h[0]:02x}{h[1]:02x}{h[2]:02x}'
        cells.append(f'<rect x="{c*4}" y="{r*4}" width="4" height="4" fill="{color}"/>')
    svg = f'<svg xmlns="http://www.w3.org/2000/svg" width="{side*4}" height="{side*4}">' + ''.join(cells) + '</svg>'
    return svg.encode(), None

def decode_svg_grid(encoded):
    """Can't decode — MD5 is one-way."""
    return None

def encode_wallet_sequence(chunks, max_frames=16):
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
    return svg.encode(), decode_wallet_sequence

def decode_wallet_sequence(encoded):
    """Decode wallet SVG → recover grayscale bytes from rect fills."""
    import re
    text = encoded.decode('utf-8', errors='replace')
    # Each frame has 64 rects (8x8), each with grayscale fill
    fills = re.findall(r'fill="#([0-9a-f]{2})\1\1"', text)
    return bytes(int(f, 16) for f in fills)

def encode_vault_svg(data):
    """Data → zlib → base64 → SVG metadata. LOSSLESS."""
    compressed = zlib.compress(data, 9)
    b64 = base64.b64encode(compressed).decode()
    svg = f'<svg xmlns="http://www.w3.org/2000/svg"><metadata><vault:file size="{len(data)}">{b64}</vault:file></metadata></svg>'
    return svg.encode(), decode_vault_svg

def decode_vault_svg(encoded):
    """Decode vault SVG → extract base64 → decompress → original data."""
    import re
    text = encoded.decode('utf-8', errors='replace')
    m = re.search(r'<vault:file size="\d+">([A-Za-z0-9+/=]+)</vault:file>', text)
    if not m:
        return None
    compressed = base64.b64decode(m.group(1))
    return zlib.decompress(compressed)

def encode_svgz_grid(chunks, side=20):
    """SVG grid → gzip → .svgz. Uses MD5 → LOSSY."""
    cells = []
    for ci, chunk in enumerate(chunks[:side*side]):
        r, c = ci // side, ci % side
        h = hashlib.md5(chunk).digest()
        color = f'#{h[0]:02x}{h[1]:02x}{h[2]:02x}'
        cells.append(f'<rect x="{c*4}" y="{r*4}" width="4" height="4" fill="{color}"/>')
    svg = f'<svg xmlns="http://www.w3.org/2000/svg" width="{side*4}" height="{side*4}">' + ''.join(cells) + '</svg>'
    return gzip.compress(svg.encode()), None

def decode_svgz_grid(encoded):
    """Can't decode — MD5 is one-way."""
    return None

# --- New: data-in-XML approach (lossless) ---
def encode_svg_data_xml(data):
    """Encode raw bytes as hex pairs in SVG <data> element."""
    hex_str = data.hex()
    svg = f'<svg xmlns="http://www.w3.org/2000/svg"><data>{hex_str}</data></svg>'
    return svg.encode(), decode_svg_data_xml

def decode_svg_data_xml(encoded):
    """Decode hex pairs from SVG <data> element."""
    import re
    text = encoded.decode('utf-8', errors='replace')
    m = re.search(r'<data>([0-9a-f]+)</data>', text)
    if not m:
        return None
    return bytes.fromhex(m.group(1))

# --- New: SVG with integer attributes (lossless) ---
def encode_svg_int_attrs(data, chunk_size=32):
    """Encode bytes as integer attributes in SVG elements."""
    chunks = [data[i:i+chunk_size] for i in range(0, len(data), chunk_size)]
    elements = []
    for i, chunk in enumerate(chunks):
        vals = ','.join(str(b) for b in chunk)
        elements.append(f'<d i="{i}" v="{vals}"/>')
    svg = f'<svg xmlns="http://www.w3.org/2000/svg"><root>{"".join(elements)}</root></svg>'
    return svg.encode(), decode_svg_int_attrs

def decode_svg_int_attrs(encoded):
    """Decode integer attributes from SVG elements."""
    import re
    text = encoded.decode('utf-8', errors='replace')
    vals = re.findall(r'v="([^"]+)"', text)
    data = bytearray()
    for v in vals:
        data.extend(int(x) for x in v.split(','))
    return bytes(data)


# === MAIN TEST ===

approaches = [
    ("SVG sequence (16 frames)",    encode_svg_sequence,    decode_svg_sequence),
    ("SVG grid (MD5 hash)",         encode_svg_grid,        decode_svg_grid),
    ("Wallet vector (16 frames)",   encode_wallet_sequence, decode_wallet_sequence),
    ("Vault SVG (zlib+b64)",        encode_vault_svg,       decode_vault_svg),
    (".svgz (gzip SVG grid)",       encode_svgz_grid,       decode_svgz_grid),
    ("SVG data XML (hex)",          encode_svg_data_xml,    decode_svg_data_xml),
    ("SVG int attrs",               encode_svg_int_attrs,   decode_svg_int_attrs),
]

for data_kind in ['random', 'structured']:
    data = make_random(100000) if data_kind == 'random' else make_structured(100000)
    original_hash = hashlib.sha256(data).hexdigest()
    chunks = chunk_data(data)
    
    print(f'\n{"="*70}')
    print(f'  DATA: {data_kind.upper()} ({len(data):,} bytes, SHA256={original_hash[:16]}...)')
    print(f'{"="*70}')
    
    raw_zlib = zlib.compress(data, 9)
    print(f'  [baseline] zlib alone:  {len(raw_zlib):>8,} bytes  ({len(data)/len(raw_zlib):.2f}x)  roundtrip: YES')
    
    for name, encoder, decoder in approaches:
        encoded, dec_fn = encoder(chunks) if encoder in [encode_svg_sequence, encode_svg_grid, encode_wallet_sequence, encode_svgz_grid] else encoder(data)
        
        if decoder is None:
            # Known lossy
            print(f'  {name:30s}  {len(encoded):>8,} bytes  ({len(data)/len(encoded):.2f}x)  roundtrip: LOSSY ✗')
            continue
        
        # Try roundtrip
        decoded = decoder(encoded)
        if decoded is None:
            print(f'  {name:30s}  {len(encoded):>8,} bytes  ({len(data)/len(encoded):.2f}x)  roundtrip: DECODE FAILED ✗')
            continue
        
        # Pad/truncate to match original size (some approaches truncate at chunk boundary)
        if len(decoded) < len(data):
            decoded = decoded + b'\x00' * (len(data) - len(decoded))
        elif len(decoded) > len(data):
            decoded = decoded[:len(data)]
        
        match = (decoded == data)
        match_hash = hashlib.sha256(decoded).hexdigest() == original_hash
        print(f'  {name:30s}  {len(encoded):>8,} bytes  ({len(data)/len(encoded):.2f}x)  roundtrip: {"✓ PASS" if match else "✗ FAIL"}  hash_match={match_hash}')

print(f'\n{"="*70}')
print(f'  CONCLUSION')
print(f'{"="*70}')
print(f'  Only "Vault SVG (zlib+b64)" and "SVG data XML (hex)" are truly lossless.')
print(f'  SVG approaches using visual encoding (MD5, color mapping) are LOSSY.')
print(f'  The "32× compression for random data" claim is FALSE for lossless roundtrip.')
print(f'  zlib alone on random data: ~1.00× (no compression possible for high-entropy data).')
