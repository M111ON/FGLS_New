"""
3-Path Architecture Test
========================
Based on the original architecture diagram:
1. Primary Pipeline: Data → 64B → Bond → Shell → GeoPixel → Hamburger → GPX5
2. Capture Net: Content → FNV-1a → Node ID → RDH → DRamTile
3. GeoField: File → Variable Chunks → GpAddr → FrustumBlock

All3 paths converge into DRamTile Core Engine.
"""
import hashlib, math, os, struct, zlib, time, gzip

CHUNK_SZ = 64

# ═══════════════════════════════════════════════════════════════
# PATH 1: Primary Pipeline
# ═══════════════════════════════════════════════════════════════

def fnv1a_hash(data):
    """FNV-1a hash (used in Capture Net for fingerprinting)."""
    h = 0x811c9dc5
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h

def bond_chain(chunks):
    """Bond Chain: link chunks sequentially with XOR chain."""
    t0 = time.perf_counter()
    chain = []
    prev = b'\x00' * CHUNK_SZ
    for chunk in chunks:
        bonded = bytes(a ^ b for a, b in zip(chunk, prev))
        chain.append(bonded)
        prev = chunk
    t1 = time.perf_counter()
    return chain, (t1 - t0) * 1000

def bond_chain_decode(chain):
    """Decode bond chain back to original chunks."""
    chunks = []
    prev = b'\x00' * CHUNK_SZ
    for bonded in chain:
        original = bytes(a ^ b for a, b in zip(bonded, prev))
        chunks.append(original)
        prev = original
    return chunks

def shell_container_encode(chunks):
    """Shell Container: classify and compress chunks."""
    t0 = time.perf_counter()
    encoded = []
    for chunk in chunks:
        # Classify: FLAT (all same byte), DIFF (XOR with prev), or RAW
        if len(set(chunk)) == 1:
            encoded.append(('FLAT', chunk[0:1]))
        else:
            encoded.append(('RAW', chunk))
    t1 = time.perf_counter()
    return encoded, (t1 - t0) * 1000

def shell_container_decode(encoded):
    """Decode shell container."""
    chunks = []
    for tag, data in encoded:
        if tag == 'FLAT':
            chunks.append(bytes([data[0]] * CHUNK_SZ))
        else:
            chunks.append(data)
    return chunks

def geopixel_rgb_encode(chunks, side=None):
    """GeoPixel RGB: convert chunks to RGB pixel data."""
    t0 = time.perf_counter()
    n = len(chunks)
    if side is None:
        side = max(2, int(math.ceil(n ** 0.5)))
    
    # Create pixel array: each chunk = 8x8 block of pixels
    pixels = []
    for ci, chunk in enumerate(chunks[:side*side]):
        r, c = ci // side, ci % side
        # Sample chunk to create pixel
        if len(chunk) >= 3:
            pr, pg, pb = chunk[0], chunk[1], chunk[2]
        else:
            pr, pg, pb = 0, 0, 0
        pixels.append((pr, pg, pb))
    
    t1 = time.perf_counter()
    return pixels, (t1 - t0) * 1000

def hamburger_codec_encode(pixels):
    """Hamburger Codec: same pattern, different colors."""
    t0 = time.perf_counter()
    # Group pixels by pattern (R,G,B values that share structure)
    patterns = {}
    for i, (r, g, b) in enumerate(pixels):
        # Pattern = quantized color (reduce to 16 levels)
        pattern = (r // 16, g // 16, b // 16)
        if pattern not in patterns:
            patterns[pattern] = []
        patterns[pattern].append(i)
    
    # Store: pattern dict + per-pixel color offsets
    encoded = {
        'patterns': {k: len(v) for k, v in patterns.items()},
        'count': len(pixels),
        'unique_patterns': len(patterns)
    }
    t1 = time.perf_counter()
    return encoded, (t1 - t0) * 1000

def gpx5_container_encode(primary_output, capture_output, geofield_output):
    """GPX5 Container: combine all3 paths into final container."""
    t0 = time.perf_counter()
    container = {
        'primary': primary_output,
        'capture': capture_output,
        'geofield': geofield_output,
        'magic': 'GPX5',
        'version': 1
    }
    # Serialize to bytes
    data = zlib.compress(str(container).encode(), 9)
    t1 = time.perf_counter()
    return data, (t1 - t0) * 1000

# ═══════════════════════════════════════════════════════════════
# PATH 2: Capture Net
# ═══════════════════════════════════════════════════════════════

def tw_capture(content):
    """TW Capture: temporal window capture."""
    t0 = time.perf_counter()
    # Create temporal fingerprint
    fingerprint = fnv1a_hash(content)
    timestamp = int(time.time() * 1000) % 0xFFFFFFFF
    capture = {
        'fingerprint': fingerprint,
        'timestamp': timestamp,
        'size': len(content)
    }
    t1 = time.perf_counter()
    return capture, (t1 - t0) * 1000

def node_id_generate(capture):
    """Node ID: generate unique identifier from capture."""
    t0 = time.perf_counter()
    node_id = (capture['fingerprint'] ^ capture['timestamp']) & 0xFFFFFFFF
    t1 = time.perf_counter()
    return node_id, (t1 - t0) * 1000

def rdh_address_encode(node_id, size):
    """RDH Address: relative data heap address."""
    t0 = time.perf_counter()
    # RDH = relative offset + size + checksum
    offset = node_id % 1000000
    checksum = fnv1a_hash(struct.pack('<II', offset, size))
    rdh = {
        'offset': offset,
        'size': size,
        'checksum': checksum
    }
    t1 = time.perf_counter()
    return rdh, (t1 - t0) * 1000

# ═══════════════════════════════════════════════════════════════
# PATH 3: GeoField
# ═══════════════════════════════════════════════════════════════

def variable_length_chunks(data, min_sz=32, max_sz=256):
    """Variable-length Chunks: adaptive chunking based on content."""
    t0 = time.perf_counter()
    chunks = []
    i = 0
    while i < len(data):
        # Find natural break point (content-aware)
        chunk_sz = min_sz
        for sz in range(min_sz, min(max_sz + 1, len(data) - i + 1)):
            # Use content hash to determine break
            h = fnv1a_hash(data[i:i+sz])
            if h % 7 == 0:  # 1/7 chance of break
                chunk_sz = sz
                break
        chunks.append(data[i:i+chunk_sz])
        i += chunk_sz
    t1 = time.perf_counter()
    return chunks, (t1 - t0) * 1000

def gp_addr_encode(chunks):
    """GpAddr: geometric address for each chunk."""
    t0 = time.perf_counter()
    addresses = []
    for i, chunk in enumerate(chunks):
        # Geometric address = hash-based positioning
        h = fnv1a_hash(chunk)
        addr = {
            'tile_id': h % 12,  # 12 pentagons
            'dim': (h >> 8) % 4,  # 4 dimensions
            'face': (h >> 16) % 12,  # 12 faces
            'edge': (h >> 24) % 5,  # 5 edges
        }
        addresses.append(addr)
    t1 = time.perf_counter()
    return addresses, (t1 - t0) * 1000

def frustum_block_encode(chunks, addresses):
    """FrustumBlock: multi-resolution zoom blocks."""
    t0 = time.perf_counter()
    # Group chunks by geometric address
    blocks = {}
    for chunk, addr in zip(chunks, addresses):
        key = (addr['tile_id'], addr['face'])
        if key not in blocks:
            blocks[key] = []
        blocks[key].append(chunk)
    
    # Create multi-resolution representation
    multi_res = {}
    for key, block_chunks in blocks.items():
        # Level 0: full resolution
        level_0 = b''.join(block_chunks)
        # Level 1: half resolution (sample every 2nd byte)
        level_1 = level_0[::2]
        # Level 2: quarter resolution
        level_2 = level_0[::4]
        multi_res[key] = {
            'level_0': len(level_0),
            'level_1': len(level_1),
            'level_2': len(level_2),
            'chunks': len(block_chunks)
        }
    
    t1 = time.perf_counter()
    return multi_res, (t1 - t0) * 1000

# ═══════════════════════════════════════════════════════════════
# TEST HARNESS
# ═══════════════════════════════════════════════════════════════

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

def test_primary_pipeline(data):
    """Test Path 1: Primary Pipeline."""
    print("\n═══ PATH 1: Primary Pipeline ═══")
    
    # Step 1: 64B Chunks
    chunks = [data[i:i+CHUNK_SZ] for i in range(0, len(data), CHUNK_SZ)]
    print(f"  1. 64B Chunks: {len(data):,} → {len(chunks)} chunks")
    
    # Step 2: Bond Chain
    chain, t_bond = bond_chain(chunks)
    chain_size = len(chain) * CHUNK_SZ
    print(f"  2. Bond Chain: {len(chunks)*CHUNK_SZ:,} → {chain_size:,} bytes ({t_bond:.1f}ms)")
    
    # Step 3: Shell Container
    shell, t_shell = shell_container_encode(chain)
    shell_size = sum(1 + len(d) for _, d in shell)
    print(f"  3. Shell Container: {chain_size:,} → {shell_size:,} bytes ({t_shell:.1f}ms)")
    
    # Step 4: GeoPixel RGB
    pixels, t_gp = geopixel_rgb_encode(chain)
    print(f"  4. GeoPixel RGB: {len(chain)} chunks → {len(pixels)} pixels ({t_gp:.1f}ms)")
    
    # Step 5: Hamburger Codec
    hamburger, t_hb = hamburger_codec_encode(pixels)
    print(f"  5. Hamburger Codec: {len(pixels)} pixels → {hamburger['unique_patterns']} patterns ({t_hb:.1f}ms)")
    
    # Step 6: GPX5 Container (just primary part)
    gpx5_primary = zlib.compress(str(shell).encode(), 9)
    print(f"  6. GPX5 Primary: {shell_size:,} → {len(gpx5_primary):,} bytes")
    
    return {
        'chunks': chunks,
        'chain': chain,
        'shell': shell,
        'pixels': pixels,
        'hamburger': hamburger,
        'gpx5_primary': gpx5_primary
    }

def test_capture_net(data):
    """Test Path 2: Capture Net."""
    print("\n═══ PATH 2: Capture Net ═══")
    
    # Step 1: FNV-1a Fingerprint
    fingerprint = fnv1a_hash(data)
    print(f"  1. FNV-1a Fingerprint: {fingerprint:#010x}")
    
    # Step 2: TW Capture
    capture, t_tw = tw_capture(data)
    print(f"  2. TW Capture: fp={capture['fingerprint']:#010x} ts={capture['timestamp']} ({t_tw:.1f}ms)")
    
    # Step 3: Node ID
    node_id, t_node = node_id_generate(capture)
    print(f"  3. Node ID: {node_id:#010x} ({t_node:.1f}ms)")
    
    # Step 4: RDH Address
    rdh, t_rdh = rdh_address_encode(node_id, len(data))
    print(f"  4. RDH Address: offset={rdh['offset']} size={rdh['size']} ({t_rdh:.1f}ms)")
    
    # Step 5: DRamTile mmap (simulate)
    dramtile_size = 64  # metadata only
    print(f"  5. DRamTile mmap: {dramtile_size} bytes metadata")
    
    return {
        'fingerprint': fingerprint,
        'capture': capture,
        'node_id': node_id,
        'rdh': rdh,
        'dramtile_size': dramtile_size
    }

def test_geofield(data):
    """Test Path 3: GeoField."""
    print("\n═══ PATH 3: GeoField ═══")
    
    # Step 1: Variable-length Chunks
    var_chunks, t_var = variable_length_chunks(data)
    var_size = sum(len(c) for c in var_chunks)
    print(f"  1. Variable Chunks: {len(data):,} → {len(var_chunks)} chunks ({var_size:,} bytes, {t_var:.1f}ms)")
    
    # Step 2: GpAddr
    addresses, t_addr = gp_addr_encode(var_chunks)
    addr_size = len(addresses) * 8  # 8 bytes per address
    print(f"  2. GpAddr: {len(var_chunks)} addresses ({addr_size:,} bytes, {t_addr:.1f}ms)")
    
    # Step 3: FrustumBlock
    frustum, t_frustum = frustum_block_encode(var_chunks, addresses)
    total_levels = sum(b['level_0'] + b['level_1'] + b['level_2'] for b in frustum.values())
    print(f"  3. FrustumBlock: {len(frustum)} blocks, {total_levels:,} bytes total ({t_frustum:.1f}ms)")
    
    return {
        'var_chunks': var_chunks,
        'addresses': addresses,
        'frustum': frustum,
        'total_levels': total_levels
    }

def test_gpx5_container(primary, capture, geofield):
    """Test GPX5 Container: combine all3 paths."""
    print("\n═══ GPX5 Container (All3 Paths) ═══")
    
    # Combine all outputs
    combined = {
        'primary_shell': str(primary['shell']),
        'capture_node': capture['node_id'],
        'geofield_blocks': len(geofield['frustum'])
    }
    
    # Serialize and compress
    data = zlib.compress(str(combined).encode(), 9)
    print(f"  Combined size: {len(data):,} bytes")
    
    # Also test gzip
    gz_data = gzip.compress(str(combined).encode(), 9)
    print(f"  Gzip size: {len(gz_data):,} bytes")
    
    return {
        'combined': data,
        'gzip': gz_data
    }

def run_full_test(kind='structured', size=100000):
    """Run complete 3-path architecture test."""
    print(f"\n{'='*70}")
    print(f"  3-PATH ARCHITECTURE TEST: {kind.upper()} ({size:,} bytes)")
    print(f"{'='*70}")
    
    data = make_data(kind, size)
    
    # Test all3 paths
    primary = test_primary_pipeline(data)
    capture = test_capture_net(data)
    geofield = test_geofield(data)
    
    # Test GPX5 container
    gpx5 = test_gpx5_container(primary, capture, geofield)
    
    # Summary
    print(f"\n═══ SUMMARY ═══")
    print(f"  Original data:      {len(data):>8,} bytes")
    print(f"  Primary (GPX5):     {len(primary['gpx5_primary']):>8,} bytes  ({len(data)/len(primary['gpx5_primary']):.2f}x)")
    print(f"  Capture metadata:   {capture['dramtile_size']:>8} bytes")
    print(f"  GeoField frustum:   {geofield['total_levels']:>8,} bytes")
    print(f"  GPX5 combined:      {len(gpx5['combined']):>8,} bytes  ({len(data)/len(gpx5['combined']):.2f}x)")
    print(f"  GPX5 gzip:          {len(gpx5['gzip']):>8,} bytes  ({len(data)/len(gpx5['gzip']):.2f}x)")
    
    # Verify roundtrip
    decoded_chain = bond_chain_decode(primary['chain'])
    decoded_chunks = shell_container_decode(primary['shell'])
    roundtrip_ok = decoded_chain == decoded_chunks
    
    # Check if first chunk matches
    first_match = decoded_chain[0] == data[:CHUNK_SZ]
    print(f"\n  Roundtrip: {'✓ PASS' if roundtrip_ok else '✗ FAIL'}")
    print(f"  First chunk match: {'✓' if first_match else '✗'}")
    
    return {
        'original': len(data),
        'primary': len(primary['gpx5_primary']),
        'capture': capture['dramtile_size'],
        'geofield': geofield['total_levels'],
        'gpx5': len(gpx5['combined']),
        'gpx5_gz': len(gpx5['gzip']),
        'roundtrip': roundtrip_ok
    }

if __name__ == '__main__':
    results = {}
    for kind in ['structured', 'random', 'source', 'tensor']:
        results[kind] = run_full_test(kind, 100000)
    
    print(f"\n{'='*70}")
    print(f"  COMPARISON TABLE")
    print(f"{'='*70}")
    print(f"  {'Type':<12} {'Original':>10} {'Primary':>10} {'GPX5':>10} {'GPX5+gz':>10} {'Ratio':>8}")
    print(f"  {'-'*12} {'-'*10} {'-'*10} {'-'*10} {'-'*10} {'-'*8}")
    for kind, r in results.items():
        ratio = r['original'] / r['gpx5_gz']
        print(f"  {kind:<12} {r['original']:>10,} {r['primary']:>10,} {r['gpx5']:>10,} {r['gpx5_gz']:>10,} {ratio:>7.2f}x")
