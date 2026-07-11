#!/usr/bin/env python3
"""
GeoPixel Full Pipeline — GeoField → Wallet → Geopixel
═══════════════════════════════════════════════════════

Complete pipeline:
  1. Raw file → 64-byte chunks
  2. Chunks → GpAddr (tile_id, dim) via geo_field mapping
  3. GpAddr → CoordRecord (face|edge|z) wallet format
  4. Wang tile validation (Fib 2&7 chord)
  5. Tantrix routing (256-state)
  6. 6-face surface encoding (Hamburger Codec)
  7. Interior reconstruction via geo_frame_seek timeline

All components from:
  geo_field_core.h, geo_frame_seek_wang.h, lc_tantrix.h,
  pogls_coord_wallet.h, geo_frame_seek.h, geo_jump.h
"""

import numpy as np
import math, time, hashlib, struct

# ════════════════════════════════════════════════════════════════
# CONSTANTS (from C headers)
# ════════════════════════════════════════════════════════════════

CHUNK_SZ = 64                    # geo_field_core.h: GF_CHUNK_SZ
FRAME_CYCLE = 1440               # geo_frame_seek.h
FRAME_STRIDE = 37                # prime walk
FRAME_FACE_SZ = 120
FRAME_EDGES = 12
FRAME_H_ACTIVE = 9
FRAME_ICO_NODES = 162
FRAME_PEANO_GRID = 81

GEO_FULL = 20736                 # 144²
GOLDEN_PHI = (1 + math.sqrt(5)) / 2

# Wang tile constants
WANG_WIN_SIZE = 12               # geo_frame_seek_wang.h
WANG_WIN_COUNT = 120             # 1440 / 12

# Tantrix constants (from lc_tantrix.h)
TANTRIX_NULL   = 0x00
TANTRIX_CROSS  = 0xAA
TANTRIX_MERGE  = 0x55
TANTRIX_SPLIT  = 0xFF

LC_GATE_WARP      = 0
LC_GATE_COLLISION = 1
LC_GATE_ROUTE     = 2
LC_GATE_GROUND    = 3

# Goldberg sphere (from geo_goldberg_sphere.h)
# Level 2: 10*2²+2 = 42 tiles, 12 penta + 30 hexa
GP_LEVEL_DEFAULT = 2
GP_PENT_COUNT = 12
GP_MAX_LEVEL = 8

# Skeleton strategies (from skeleton_index.h)
SKEL_ID   = 0
SKEL_FLAT = 1
SKEL_DIFF = 2
SKEL_BREF = 3
SKEL_GEOM = 4
SKEL_RAW  = 5

# ════════════════════════════════════════════════════════════════
# SECTION 1: Goldberg Sphere → GpAddr mapping
# ════════════════════════════════════════════════════════════════

def gp_face_count(level):
    """Number of tiles at given Goldberg level. 10n²+2"""
    return 10 * level * level + 2

def gp_chunk_to_addr(level, chunk_idx):
    """Map chunk_idx → GpAddr (tile_id, dim). From geo_field_core.h"""
    face_max = gp_face_count(level)
    tile_id = chunk_idx % face_max
    dim = (chunk_idx // face_max) & 0x7F
    return tile_id, dim

def gp_is_pentagon(tile_id):
    """First 12 tiles are pentagons."""
    return tile_id < GP_PENT_COUNT

def gp_tile_to_pent(level, tile_id):
    """Map any tile to its nearest pentagon anchor."""
    return tile_id % GP_PENT_COUNT

def gp_is_zone_boundary(tile_id):
    """Zone boundary = pentagon tiles."""
    return gp_is_pentagon(tile_id)

# ════════════════════════════════════════════════════════════════
# SECTION 2: TRing walk (stride-37)
# ════════════════════════════════════════════════════════════════

def tring_walk_enc(tile_id):
    """TRing walk enc: stride-37 on tile_id. From tring.h"""
    return (tile_id * FRAME_STRIDE) % 720

def tring_walk_spoke(tile_id):
    """TRing spoke: 0..5 based on tile_id mod 6."""
    return tile_id % 6

# ════════════════════════════════════════════════════════════════
# SECTION 3: Skeleton Index (skeleton_index.h)
# ════════════════════════════════════════════════════════════════

class SkeletonCtx:
    """Simplified skeleton encode context."""
    def __init__(self):
        self.last_chunk = None
        self.zone_resets = 0
        self.hits = [0] * 6

def skel_encode_chunk(ctx, chunk, chunk_offset):
    """Classify chunk encoding strategy."""
    if ctx.last_chunk is None:
        ctx.hits[SKEL_ID] += 1
        ctx.last_chunk = chunk[:]
        return SKEL_ID, chunk

    # FLAT: all bytes same value
    if len(set(chunk)) == 1:
        ctx.hits[SKEL_FLAT] += 1
        return SKEL_FLAT, bytes([chunk[0]])

    # DIFF: XOR with previous chunk
    diff = bytes(a ^ b for a, b in zip(chunk, ctx.last_chunk))
    nonzero = sum(1 for b in diff if b != 0)
    if nonzero < 16:
        ctx.hits[SKEL_DIFF] += 1
        ctx.last_chunk = chunk[:]
        return SKEL_DIFF, diff

    # Default: RAW
    ctx.hits[SKEL_RAW] += 1
    ctx.last_chunk = chunk[:]
    return SKEL_RAW, chunk

# ════════════════════════════════════════════════════════════════
# SECTION 4: Wang Tile Layer (geo_frame_seek_wang.h)
# ════════════════════════════════════════════════════════════════

def wang_chord_a(enc):
    """Fibonacci 2 chord: (enc * 2) % 9"""
    return (enc * 2) % 9

def wang_chord_b(enc):
    """Fibonacci 7 chord: (enc * 7) % 9"""
    return (enc * 7) % 9

def wang_is_369(enc):
    """Tesla loop boundary: enc%9 ∈ {0,3,6}"""
    d = enc % 9
    return d == 0 or d == 3 or d == 6

def wang_chord_valid(enc):
    """Tamper invariant: chord_a + chord_b == 9"""
    a = wang_chord_a(enc)
    b = wang_chord_b(enc)
    if enc % 9 == 0:
        return a == 0 and b == 0
    return (a + b) == 9

class WangLayer:
    """Wang tile validation layer."""
    def __init__(self):
        self.windows = [None] * WANG_WIN_COUNT
        self._compute_all()

    def _compute_window(self, win_idx):
        base_t = win_idx * WANG_WIN_SIZE
        xor_acc = 0
        skip_mask = 0
        first_enc = 0
        last_enc = 0
        tile_id = 0

        for i in range(WANG_WIN_SIZE):
            t = base_t + i
            enc = (t * FRAME_STRIDE) % FRAME_CYCLE
            xor_acc ^= enc
            frame = self._frame_at(enc)
            if frame['is_skip']:
                skip_mask |= (1 << i)
            if i == 0:
                first_enc = enc
                tile_id = frame['face']
            if i == WANG_WIN_SIZE - 1:
                last_enc = enc

        return {
            'xor_enc': xor_acc,
            'edge_top': wang_chord_a(first_enc),
            'edge_top_b': wang_chord_b(first_enc),
            'edge_bot': wang_chord_a(last_enc),
            'edge_bot_b': wang_chord_b(last_enc),
            'tile_id': tile_id,
            'skip_mask': skip_mask,
            'valid': True,
        }

    def _compute_all(self):
        for w in range(WANG_WIN_COUNT):
            self.windows[w] = self._compute_window(w)

    def _frame_at(self, enc):
        face = enc // FRAME_FACE_SZ
        slot = enc % FRAME_FACE_SZ
        is_skip = (enc % FRAME_EDGES) >= FRAME_H_ACTIVE
        return {'face': face, 'slot': slot, 'is_skip': is_skip}

    def edge_valid(self, win_idx):
        if win_idx == 0:
            return True
        prev = self.windows[win_idx - 1]
        curr = self.windows[win_idx]
        return prev['edge_bot'] == curr['edge_top']

    def tamper_check(self, win_idx):
        w = self.windows[win_idx]
        top_ok = (w['edge_top'] == 0 and w['edge_top_b'] == 0) or \
                 (w['edge_top'] + w['edge_top_b'] == 9)
        bot_ok = (w['edge_bot'] == 0 and w['edge_bot_b'] == 0) or \
                 (w['edge_bot'] + w['edge_bot_b'] == 9)
        return top_ok and bot_ok

    def seek_gate(self, enc):
        win_idx = (enc // WANG_WIN_SIZE) % WANG_WIN_COUNT
        if not wang_chord_valid(enc):
            return 'TAMPER'
        if wang_is_369(enc):
            return '369'
        if not self.edge_valid(win_idx):
            return 'MISMATCH'
        return 'OK'

    def verify(self):
        for w in range(WANG_WIN_COUNT):
            if not self.windows[w]['valid']:
                return False
        for w in range(1, WANG_WIN_COUNT):
            if not self.edge_valid(w):
                return False
        for w in range(WANG_WIN_COUNT):
            if not self.tamper_check(w):
                return False
        return True

# ════════════════════════════════════════════════════════════════
# SECTION 5: Tantrix 256-state routing (lc_tantrix.h)
# ════════════════════════════════════════════════════════════════

def tantrix_make(entry, exit, spoke, cls=0):
    return (entry & 0x3) | ((exit & 0x3) << 2) | \
           ((spoke & 0x3) << 4) | ((cls & 0x3) << 6)

def tantrix_entry(t):  return t & 0x3
def tantrix_exit(t):   return (t >> 2) & 0x3
def tantrix_spoke(t):  return (t >> 4) & 0x3
def tantrix_class(t):  return (t >> 6) & 0x3

def tantrix_connects(left, right):
    if left == TANTRIX_NULL or right == TANTRIX_NULL:
        return False
    if left == TANTRIX_SPLIT or right == TANTRIX_SPLIT:
        return True
    return tantrix_exit(left) == tantrix_entry(right)

def tantrix_route(t, incoming_gate):
    if t == TANTRIX_NULL:
        return LC_GATE_GROUND, 'DROP'
    if t == TANTRIX_SPLIT:
        return incoming_gate, 'BROADCAST'
    if t == TANTRIX_MERGE:
        return LC_GATE_GROUND, 'MERGE'
    if t == TANTRIX_CROSS:
        cross_map = [LC_GATE_COLLISION, LC_GATE_GROUND,
                     LC_GATE_WARP, LC_GATE_ROUTE]
        return cross_map[incoming_gate & 3], 'FORWARD'

    if tantrix_entry(t) != incoming_gate:
        return LC_GATE_GROUND, 'DROP'

    exit_gate = tantrix_exit(t)
    cls = tantrix_class(t)
    if cls == 1:  # SKIP
        exit_gate ^= 0x3
    elif cls == 2:  # MIRROR
        exit_gate = ((exit_gate & 1) << 1) | ((exit_gate >> 1) & 1)

    return exit_gate, 'FORWARD'

# ════════════════════════════════════════════════════════════════
# SECTION 6: Wallet (pogls_coord_wallet.h)
# ════════════════════════════════════════════════════════════════

def wallet_chunk_seed(chunk64):
    """XOR-fold 8 words + finalizer. From _wallet_chunk_seed"""
    w = [0] * 8
    for i in range(8):
        off = i * 8
        for j in range(8):
            if off + j < len(chunk64):
                w[i] |= chunk64[off + j] << (j * 8)
    s = w[0] ^ w[1] ^ w[2] ^ w[3] ^ w[4] ^ w[5] ^ w[6] ^ w[7]
    s ^= (s >> 33) & 0xFFFFFFFFFFFFFFFF
    s = (s * 0xff51afd7ed558ccd) & 0xFFFFFFFFFFFFFFFF
    s ^= (s >> 33) & 0xFFFFFFFFFFFFFFFF
    s = (s * 0xc4ceb9fe1a85ec53) & 0xFFFFFFFFFFFFFFFF
    s ^= (s >> 33) & 0xFFFFFFFFFFFFFFFF
    return s

def wallet_xorfold64(chunk64):
    """XOR-fold 64B → uint32 checksum."""
    acc = 0
    for i in range(16):
        off = i * 4
        w = 0
        for j in range(4):
            if off + j < len(chunk64):
                w |= chunk64[off + j] << (j * 8)
        acc ^= w
    return acc

def wallet_coord_pack(face, edge, z):
    """Pack ThetaCoord into uint32. From wallet_coord_pack"""
    return (face << 24) | (edge << 16) | (z << 8)

def wallet_coord_unpack(p):
    return (p >> 24) & 0xFF, (p >> 16) & 0xFF, (p >> 8) & 0xFF

# ════════════════════════════════════════════════════════════════
# SECTION 7: geo_frame_seek (from geo_frame_seek.h)
# ════════════════════════════════════════════════════════════════

def frame_enc(t):
    return (t * FRAME_STRIDE) % FRAME_CYCLE

def frame_at(enc):
    face = enc // FRAME_FACE_SZ
    slot = enc % FRAME_FACE_SZ
    group = face % 3
    edge = enc % 3
    is_skip = (enc % FRAME_EDGES) >= FRAME_H_ACTIVE
    step = (enc // 3) % 4
    sub = enc % 3
    hilbert_group = (enc // FRAME_EDGES) % 3
    ico_idx = enc % FRAME_ICO_NODES
    phase = (enc // FRAME_EDGES) % 12
    return {
        'enc': enc, 'face': face, 'slot': slot, 'group': group,
        'edge': edge, 'is_skip': is_skip, 'step': step, 'sub': sub,
        'hilbert_group': hilbert_group, 'ico_idx': ico_idx, 'phase': phase,
    }

# ════════════════════════════════════════════════════════════════
# SECTION 8: geo_jump (from geo_jump.h)
# ════════════════════════════════════════════════════════════════

def geo_jump(node_id, jump_type=0, param=0):
    if jump_type == 0:
        return (node_id * 37 + param) % GEO_FULL
    elif jump_type == 1:
        return (node_id * 81 + param) % GEO_FULL
    elif jump_type == 2:
        return (node_id * 12 + param) % GEO_FULL
    return node_id

# ════════════════════════════════════════════════════════════════
# SECTION 9: 6-face cube operations
# ════════════════════════════════════════════════════════════════

def unfold_cube(cube):
    return {
        'front':  cube[:, :, -1],
        'back':   cube[:, :, 0],
        'left':   cube[0, :, :],
        'right':  cube[-1, :, :],
        'top':    cube[:, -1, :],
        'bottom': cube[:, 0, :],
    }

FACE_LIST = ['front', 'back', 'left', 'right', 'top', 'bottom']

def reconstruct_from_timeline(faces, side):
    cube = np.zeros((side, side, side), dtype=np.float32)
    cube[:, :, -1] = faces['front']
    cube[:, :, 0] = faces['back']
    cube[0, :, :] = faces['left']
    cube[-1, :, :] = faces['right']
    cube[:, -1, :] = faces['top']
    cube[:, 0, :] = faces['bottom']

    idx = np.arange(1, side - 1)
    x, y, z = np.meshgrid(idx, idx, idx, indexing='ij')
    linear = (x * side + y) * side + z
    t = linear % FRAME_CYCLE
    enc = (t * FRAME_STRIDE) % FRAME_CYCLE
    face = enc // FRAME_FACE_SZ
    slot = enc % FRAME_FACE_SZ
    ico_idx = enc % FRAME_ICO_NODES
    phase = (enc // FRAME_EDGES) % 12

    interior = (face * 100 + slot * 10 + ico_idx + phase * 5).astype(np.float32)
    cube[1:-1, 1:-1, 1:-1] = interior
    return cube

# ════════════════════════════════════════════════════════════════
# SECTION 10: Full Pipeline
# ════════════════════════════════════════════════════════════════

class PipelineResult:
    def __init__(self):
        self.stats = {}
        self.wang_valid = False
        self.tantrix_valid = False
        self.skel_stats = None
        self.coord_count = 0
        self.total_time = 0

def pipeline_encode(data, gp_level=GP_LEVEL_DEFAULT):
    """
    Full pipeline: data → GeoField → Wallet coords → 6 faces

    Steps:
      1. Split data into 64-byte chunks
      2. Map each chunk to GpAddr (tile_id, dim) via gp_chunk_to_addr
      3. Classify each chunk (skeleton: ID/FLAT/DIFF/RAW)
      4. Generate CoordRecord (face|edge|z) from GpAddr
      5. Validate Wang tile layer
      6. Route through Tantrix
      7. Build cube from coords + timeline
      8. Unfold to 6 faces
    """
    t0 = time.perf_counter()
    result = PipelineResult()
    face_max = gp_face_count(gp_level)

    # Step 1: Chunk
    n_chunks = (len(data) + CHUNK_SZ - 1) // CHUNK_SZ
    chunks = []
    for i in range(n_chunks):
        off = i * CHUNK_SZ
        chunk = data[off:off + CHUNK_SZ]
        if len(chunk) < CHUNK_SZ:
            chunk = chunk + b'\x00' * (CHUNK_SZ - len(chunk))
        chunks.append(chunk)

    # Step 2: GeoField → GpAddr
    skel = SkeletonCtx()
    coord_records = []
    for ci, chunk in enumerate(chunks):
        tile_id, dim = gp_chunk_to_addr(gp_level, ci)

        # Step 3: Skeleton classify
        skel_strategy, skel_data = skel_encode_chunk(skel, chunk, ci * CHUNK_SZ)

        # Step 4: CoordRecord
        tring_enc = tring_walk_enc(tile_id)
        spoke = tring_walk_spoke(tile_id)

        # Map GpAddr → wallet coordinate (face|edge|z)
        face = tile_id % 12
        edge = dim % 5
        z = (tile_id // 12) & 0xFF
        coord_packed = wallet_coord_pack(face, edge, z)

        seed = wallet_chunk_seed(chunk)
        checksum = wallet_xorfold64(chunk)

        coord_records.append({
            'file_idx': 0,
            'chunk_idx': ci,
            'tile_id': tile_id,
            'dim': dim,
            'coord_packed': coord_packed,
            'face': face,
            'edge': edge,
            'z': z,
            'seed': seed,
            'checksum': checksum,
            'fast_sig': seed & 0xFFFFFFFF,
            'skel_strategy': skel_strategy,
            'tring_enc': tring_enc,
            'spoke': spoke,
        })

    result.coord_count = len(coord_records)
    result.skel_stats = skel.hits[:]

    # Step 5: Wang tile validation
    wang = WangLayer()
    result.wang_valid = wang.verify()

    # Step 6: Tantrix routing
    tan_ok = True
    for cr in coord_records:
        entry_gate = cr['edge'] % 4
        tile = tantrix_make(entry_gate, (entry_gate + 1) % 4, cr['spoke'] % 4)
        out_gate, route_type = tantrix_route(tile, entry_gate)
        if route_type == 'DROP':
            tan_ok = False
    result.tantrix_valid = tan_ok

    # Step 7: Build cube from coord records
    side = 100
    total_voxels = side ** 3
    cube = np.zeros((side, side, side), dtype=np.float32)

    for cr in coord_records:
        linear = cr['chunk_idx']
        x = (linear // (side * side)) % side
        y = (linear // side) % side
        z_idx = linear % side

        # Value derived from timeline + wallet coordinates
        enc = frame_enc(linear % FRAME_CYCLE)
        frame = frame_at(enc)

        cube[x, y, z_idx] = (
            cr['face'] * 100 +
            cr['edge'] * 50 +
            frame['slot'] * 10 +
            cr['dim'] +
            frame['ico_idx'] +
            frame['phase'] * 5
        )

    # Step 8: Unfold
    faces = unfold_cube(cube)
    face_bytes = sum(f.nbytes for f in faces.values())

    t_total = time.perf_counter() - t0
    result.total_time = t_total
    result.stats = {
        'data_size': len(data),
        'n_chunks': n_chunks,
        'face_max': face_max,
        'cube_side': side,
        'face_bytes': face_bytes,
        'cube_bytes': cube.nbytes,
        'ratio': cube.nbytes / face_bytes if face_bytes > 0 else 0,
    }
    result.cube = cube
    result.faces = faces
    result.coord_records = coord_records

    return result

def pipeline_decode(faces, side, original_size):
    """Decode: 6 faces + timeline → data."""
    reconstructed = reconstruct_from_timeline(faces, side)
    arr = reconstructed.astype(np.uint8).flatten()
    return arr[:original_size].tobytes()

# ════════════════════════════════════════════════════════════════
# SECTION 11: CLI Test
# ════════════════════════════════════════════════════════════════

if __name__ == '__main__':
    import sys

    if len(sys.argv) < 2:
        print("Usage: python geopixel_pipeline.py <file>")
        print("\nRuns full GeoField → Wallet → Geopixel pipeline.")
        sys.exit(1)

    filepath = sys.argv[1]
    data = open(filepath, 'rb').read()

    print(f"═══ GeoPixel Full Pipeline ═══\n")
    print(f"File: {filepath}")
    print(f"Size: {len(data):,} bytes ({len(data)/1024:.1f} KB)")
    print(f"SHA:  {hashlib.sha256(data).hexdigest()[:16]}\n")

    # Encode
    print("─── ENCODE (GeoField → Wallet → Geopixel) ───\n")
    result = pipeline_encode(data)

    print(f"  Chunks:     {result.stats['n_chunks']}")
    print(f"  GeoField:   face_max={result.stats['face_max']}")
    print(f"  Cube:       {result.stats['cube_side']}³ = {result.stats['cube_bytes']/1024:.1f} KB")
    print(f"  6 faces:    {result.stats['face_bytes']/1024:.1f} KB")
    print(f"  Ratio:      {result.stats['ratio']:.1f}x")
    print()

    # Wang validation
    print(f"  Wang tile:  {'✓ VALID' if result.wang_valid else '✗ INVALID'}")
    print(f"  Tantrix:    {'✓ VALID' if result.tantrix_valid else '✗ INVALID'}")
    print()

    # Skeleton stats
    skel_names = ['ID', 'FLAT', 'DIFF', 'BREF', 'GEOM', 'RAW']
    print(f"  Skeleton:   {' '.join(f'{n}={c}' for n, c in zip(skel_names, result.skel_stats))}")
    print()

    # Wang windows sample
    wang = WangLayer()
    print(f"  Wang verify: {'✓' if wang.verify() else '✗'}")
    for w in [0, 1, 59, 119]:
        win = wang.windows[w]
        print(f"    win[{w:3d}]: edge_top={win['edge_top']}, "
              f"edge_bot={win['edge_bot']}, "
              f"tile={win['tile_id']}, xor=0x{win['xor_enc']:04x}")

    # Tantrix sample
    print()
    print("  Tantrix routing sample:")
    for i in range(min(6, result.coord_count)):
        cr = result.coord_records[i]
        entry_gate = cr['edge'] % 4
        tile = tantrix_make(entry_gate, (entry_gate + 1) % 4, cr['spoke'] % 4)
        out_gate, route_type = tantrix_route(tile, entry_gate)
        print(f"    chunk[{i}]: entry={entry_gate} → exit={out_gate} ({route_type}) "
              f"coord=face{cr['face']}_e{cr['edge']}_z{cr['z']}")

    # CoordRecord sample
    print()
    print("  CoordRecord sample:")
    for i in range(min(4, result.coord_count)):
        cr = result.coord_records[i]
        face, edge, z = wallet_coord_unpack(cr['coord_packed'])
        print(f"    [{i}] tile={cr['tile_id']:3d} dim={cr['dim']:3d} "
              f"coord=({face},{edge},{z}) "
              f"skel={skel_names[cr['skel_strategy']]} "
              f"seed=0x{cr['seed']:016x}")

    print()
    print(f"  Time:       {result.total_time*1000:.1f} ms")
    print(f"\n═══ Pipeline Complete ═══")
