"""SVG as lossless container — proper test: encode ALL data, no truncation, no hash."""
import hashlib, os, zlib, base64, re, time

def make_random(size=100000):
    return os.urandom(size)

def make_structured(size=100000):
    pattern = bytes(range(256))
    return (pattern * (size // 256 + 1))[:size]

# === LOSSLESS SVG approaches (encode ALL bytes, reversible) ===

def svg_hex_element(data):
    """Store raw hex in <data> element. SVG is just a wrapper."""
    hex_str = data.hex()
    svg = f'<svg xmlns="http://www.w3.org/2000/svg"><data>{hex_str}</data></svg>'
    return svg.encode()

def svg_hex_attr(data):
    """Store hex as attribute: <d h="..."/>."""
    hex_str = data.hex()
    svg = f'<svg xmlns="http://www.w3.org/2000/svg"><d h="{hex_str}"/></svg>'
    return svg.encode()

def svg_decimal_element(data):
    """Store decimal bytes in <d> element."""
    dec_str = ' '.join(str(b) for b in data)
    svg = f'<svg xmlns="http://www.w3.org/2000/svg"><d>{dec_str}</d></svg>'
    return svg.encode()

def svg_rect_colors(data, chunk_size=3):
    """Each byte → rect fill color. 3 bytes = RGB pixel. No truncation."""
    cells = []
    for i in range(0, len(data), chunk_size):
        chunk = data[i:i+chunk_size]
        r = chunk[0]
        g = chunk[1] if len(chunk) > 1 else 0
        b = chunk[2] if len(chunk) > 2 else 0
        cells.append(f'<rect x="{i//chunk_size}" y="0" width="1" height="1" fill="#{r:02x}{g:02x}{b:02x}"/>')
    svg = f'<svg xmlns="http://www.w3.org/2000/svg" width="{len(cells)}" height="1">' + ''.join(cells) + '</svg>'
    return svg.encode()

def svg_metadata_b64(data):
    """zlib→base64 in <metadata>. This is what vault uses."""
    compressed = zlib.compress(data, 9)
    b64 = base64.b64encode(compressed).decode()
    svg = f'<svg xmlns="http://www.w3.org/2000/svg"><metadata size="{len(data)}">{b64}</metadata></svg>'
    return svg.encode()

def svg_text_cdata(data):
    """Store hex in CDATA section."""
    hex_str = data.hex()
    svg = f'<svg xmlns="http://www.w3.org/2000/svg"><d><![CDATA[{hex_str}]]></d></svg>'
    return svg.encode()

# === Decoders ===

def decode_hex(s):
    return bytes.fromhex(s)

def decode_svg(svg_bytes):
    text = svg_bytes.decode('utf-8', errors='replace')
    # Try hex in <data>
    m = re.search(r'<data>([0-9a-f]+)</data>', text)
    if m: return bytes.fromhex(m.group(1))
    # Try hex in <d h="...">
    m = re.search(r'<d h="([0-9a-f]+)"/>', text)
    if m: return bytes.fromhex(m.group(1))
    # Try decimal in <d>
    m = re.search(r'<d>([\d ]+)</d>', text)
    if m: return bytes(int(x) for x in m.group(1).split())
    # Try CDATA
    m = re.search(r'<!\[CDATA\[([0-9a-f]+)\]\]>', text)
    if m: return bytes.fromhex(m.group(1))
    # Try rect fills
    fills = re.findall(r'fill="#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})"', text)
    if fills:
        data = bytearray()
        for r, g, b in fills:
            data.extend([int(r,16), int(g,16), int(b,16)])
        return bytes(data)
    # Try metadata b64
    m = re.search(r'<metadata size="\d+">([A-Za-z0-9+/=]+)</metadata>', text)
    if m:
        compressed = base64.b64decode(m.group(1))
        return zlib.decompress(compressed)
    return None

# === Main test ===

for kind in ['random', 'structured']:
    data = make_random(100000) if kind == 'random' else make_structured(100000)
    original_hash = hashlib.sha256(data).hexdigest()[:16]
    
    print(f'\n{"="*70}')
    print(f'  {kind.upper()} DATA: {len(data):,} bytes, SHA256={original_hash}...')
    print(f'{"="*70}')
    
    # Baseline
    raw_zlib = zlib.compress(data, 9)
    print(f'  zlib alone:            {len(raw_zlib):>10,} bytes  ({len(data)/len(raw_zlib):.2f}x)  lossless: ✓')
    
    for name, encoder in [
        ("SVG hex <data> element",     svg_hex_element),
        ("SVG hex <d h=.../> attr",    svg_hex_attr),
        ("SVG decimal <d> element",    svg_decimal_element),
        ("SVG rect colors (3B/pixel)", svg_rect_colors),
        ("SVG metadata (zlib+b64)",    svg_metadata_b64),
        ("SVG CDATA hex",              svg_text_cdata),
    ]:
        t0 = time.time()
        encoded = encoder(data)
        enc_time = time.time() - t0
        
        # SVG+zlib combined
        svg_zlib = zlib.compress(encoded, 9)
        
        # Roundtrip
        decoded = decode_svg(encoded)
        if decoded is not None and len(decoded) >= len(data):
            decoded = decoded[:len(data)]
            match = (decoded == data)
        else:
            match = False
        
        print(f'  {name:28s}  svg={len(encoded):>10,}  svg+zlib={len(svg_zlib):>10,}  '
              f'({len(data)/len(svg_zlib):.2f}x)  roundtrip: {"✓" if match else "✗"}  ({enc_time:.3f}s)')
    
    print(f'\n  zlib baseline ratio: {len(data)/len(raw_zlib):.2f}x')
    print(f'  To beat zlib alone, svg_zlib must be < {len(raw_zlib):,} bytes')
