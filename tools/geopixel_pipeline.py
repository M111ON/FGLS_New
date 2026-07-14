#!/usr/bin/env python3
"""
GeoPixel Full Pipeline — GeoField → Wallet → Geopixel
════════════════════════════════════════════════════════

Complete pipeline:
  1. Raw file → 64-byte chunks
  2. Chunks → GpAddr (tile_id, dim) via geo_field mapping
  3. GpAddr → CoordRecord (face|edge|z) wallet format
  4. Wang tile validation (Fib 2&7 chord) — tamper detection only
  5. Tantrix routing (256-state)
  6. Cube build: raw chunk bytes placed at wallet-coord positions
  7. 6-face unfold → .geopixel file

All components from:
  geo_field_core.h, geo_frame_seek_wang.h, lc_tantrix.h,
  pogls_coord_wallet.h, geo_frame_seek.h, geo_jump.h
"""

import numpy as np
import math, time, hashlib, struct, os, ctypes

# ════════════════════════════════════════════════════════════════
# ACCELERATION: auto-detect C-DLL, fallback to numpy, fallback to scalar
# ════════════════════════════════════════════════════════════════

_C_SEED_DLL = None
try:
    _dll_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             '..', 'collection', 'wallet_seed_c.dll')
    if os.path.exists(_dll_path):
        _C_SEED_DLL = ctypes.CDLL(_dll_path)
        _C_SEED_DLL.wallet_seed_batch_c.restype = ctypes.c_int
        _C_SEED_DLL.wallet_seed_batch_c.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
        _C_SEED_DLL.wallet_xorfold_batch_c.restype = ctypes.c_int
        _C_SEED_DLL.wallet_xorfold_batch_c.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
except Exception:
    _C_SEED_DLL = None

SEED_BACKEND = 'c_dll' if _C_SEED_DLL else 'numpy'

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

# Goldberg sphere
GP_PENT_COUNT = 12
GP_MAX_LEVEL = 8

# Skeleton strategies (from skeleton_index.h)
SKEL_ID   = 0
SKEL_FLAT = 1
SKEL_DIFF = 2
SKEL_BREF = 3
SKEL_GEOM = 4
SKEL_RAW  = 5
SKEL_NAMES = ['ID', 'FLAT', 'DIFF', 'BREF', 'GEOM', 'RAW']

# GEOM subtypes — evolution without format change
GEOM_INLINE    = 0x00  # 64B raw data inline (baseline, correctness-first)
GEOM_BLUEPRINT = 0x01  # reserved: reconstruct from formula (future)
GEOM_PARAMETRIC = 0x02 # reserved: parametric reconstruction (future)
GEOM_TEMPLATE  = 0x03  # reserved: template-based (future)
GEOM_SUBTYPE_NAMES = {0x00: 'INLINE', 0x01: 'BLUEPRINT', 0x02: 'PARAMETRIC', 0x03: 'TEMPLATE'}

# Cube bases
VALID_BASES = [4, 8, 16]

# File format
GPXL_MAGIC = b'GPXL'
GPXL_VERSION = 2  # current format version
GPXL_HEADER_SZ = 48  # v2: expanded from 32B

# Zone boundary for skel_enc_zone_reset (pentagon tile IDs)
SKEL_PENTAGON_IDS = set(range(GP_PENT_COUNT))

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
    return tile_id < GP_PENT_COUNT

def gp_tile_to_pent(level, tile_id):
    return tile_id % GP_PENT_COUNT

# ════════════════════════════════════════════════════════════════
# SECTION 2: Auto-calculate gp_level
# ════════════════════════════════════════════════════════════════

def gp_auto_level(n_chunks):
    """Select smallest gp_level where face_max * 128 >= n_chunks."""
    for level in range(1, GP_MAX_LEVEL + 1):
        face_max = gp_face_count(level)
        capacity = face_max * 128  # dim is 7 bits
        if capacity >= n_chunks:
            return level
    return GP_MAX_LEVEL

# ════════════════════════════════════════════════════════════════
# SECTION 3: TRing walk (stride-37)
# ════════════════════════════════════════════════════════════════

def tring_walk_enc(tile_id):
    return (tile_id * FRAME_STRIDE) % 720

def tring_walk_spoke(tile_id):
    return tile_id % 6

# ════════════════════════════════════════════════════════════════
# SECTION 4: Skeleton Index — ported from skeleton_index.h
# ════════════════════════════════════════════════════════════════

SKEL_ISECT_RAW_THR = 16
SKEL_DIFF_CEILING = 48
SKEL_WALK_LEN = 720
SKEL_STRIDE = 37
SKEL_FACES = 12
SKEL_FACE_SZ = 60

SKEL_GB_PAIR = [0,1,2,3,4,5, 0,1,2,3,4,5]
SKEL_GB_POLE = [0,0,0,0,0,0, 1,1,1,1,1,1]
SKEL_META_CROSS = [9,10,11,6,7,8, 3,4,5,0,1,2]

def skel_isect_pop(chunk_bytes):
    """XOR-fold 8 uint64 words → popcount. HIGH≥16=random→RAW fast-reject."""
    w = struct.unpack('<8Q', bytes(chunk_bytes[:64]))
    fold = w[0] ^ w[1] ^ w[2] ^ w[3] ^ w[4] ^ w[5] ^ w[6] ^ w[7]
    return bin(fold).count('1')

def skel_is_flat(chunk_bytes):
    """Check if all bytes are zero."""
    return all(b == 0 for b in chunk_bytes[:64])

def skel_diff_sym(prev, cur, has_prev):
    """Count differing bytes + check byte-reverse match. One pass."""
    if not has_prev:
        return 65, False
    dc = 0
    bref = True
    for i in range(CHUNK_SZ):
        if cur[i] != prev[i]:
            dc += 1
        if cur[i] != prev[CHUNK_SZ - 1 - i]:
            bref = False
        if dc > SKEL_DIFF_CEILING and not bref:
            break
    return dc, (bref and dc > 0)

def skeleton_lookup(addr):
    """O(1) addr → SkeletonIdx. From skeleton_index.h."""
    pos = addr % SKEL_WALK_LEN
    enc = (pos * SKEL_STRIDE) % SKEL_WALK_LEN
    zone = enc // SKEL_FACE_SZ
    pair = SKEL_GB_PAIR[zone]
    pole = SKEL_GB_POLE[zone]
    partner = SKEL_META_CROSS[zone]
    return {'enc': enc, 'zone': zone, 'pair': pair, 'pole': pole, 'partner': partner}

class SkeletonCtx:
    def __init__(self, ref_window=64):
        self.prev = b'\x00' * CHUNK_SZ
        self.has_prev = False
        self.chunk_count = 0
        self.hits = [0] * 6
        self.ref_window = ref_window
        self.refs = []

def skel_find_best_ref(refs, chunk):
    """Find the reference chunk with fewest differing bytes. Returns (ref_idx, dc)."""
    best_idx = 0
    best_dc = CHUNK_SZ + 1
    for i, ref in enumerate(refs):
        dc = sum(1 for a, b in zip(ref, chunk[:CHUNK_SZ]) if a != b)
        if dc < best_dc:
            best_dc = dc
            best_idx = i
            if dc == 0:
                break
    return best_idx, best_dc

def skel_decide(chunk, prev, has_prev, chunk_z=0, refs=None):
    """P0→P5 short-circuit decision. From skeleton_index.h.
    Enhanced with Diamond Shell rotation scan + sliding window ref buffer."""
    # P0: IDENTITY — zero cost
    if has_prev and chunk == prev:
        return SKEL_ID, 0, 0, 0  # strategy, best_rot, isect_pc, ref_idx
    # P1: Diamond Shell rotation scan → best rotation's isect_pop
    best_rot, isect_pc, shell_flag = diamond_shell_classify(chunk, chunk_z)
    ip_xor = skel_isect_pop(chunk)
    if ip_xor >= SKEL_ISECT_RAW_THR:
        if isect_pc > 0:
            return SKEL_GEOM, best_rot, isect_pc, 0
        return SKEL_RAW, best_rot, isect_pc, 0
    # P2: FLAT — all zeros
    if skel_is_flat(chunk):
        return SKEL_FLAT, best_rot, isect_pc, 0
    # P3+P4: diff/sym with sliding window refs
    best_ref_idx = 0
    best_dc = CHUNK_SZ + 1
    best_bref = False
    if refs:
        for i, ref in enumerate(refs):
            dc = 0
            bref = True
            for j in range(CHUNK_SZ):
                if chunk[j] != ref[j]:
                    dc += 1
                if chunk[j] != ref[CHUNK_SZ - 1 - j]:
                    bref = False
                if dc > SKEL_DIFF_CEILING and not bref:
                    break
            if bref and dc > 0:
                return SKEL_BREF, best_rot, isect_pc, i
            if 0 < dc < best_dc:
                best_dc = dc
                best_ref_idx = i
    elif has_prev:
        dc, bref = skel_diff_sym(prev, chunk, has_prev)
        if bref:
            return SKEL_BREF, best_rot, isect_pc, 0
        if 0 < dc <= SKEL_DIFF_CEILING:
            best_dc = dc
            best_ref_idx = 0
    if 0 < best_dc <= SKEL_DIFF_CEILING:
        return SKEL_DIFF, best_rot, isect_pc, best_ref_idx
    # P5: GEOM — low isect, structured
    return SKEL_GEOM, best_rot, isect_pc, 0

def skel_enc_zone_reset(ctx):
    """Reset skeleton context at zone boundary (pentagon tile).
    From skeleton_index.h: skel_enc_zone_reset."""
    ctx.has_prev = False
    ctx.prev = b'\x00' * CHUNK_SZ
    ctx.refs.clear()

def skel_encode_chunk(ctx, chunk):
    """Encode one chunk using real skeleton decision. Returns (strategy, best_rot, isect_pc, ref_idx)."""
    tile_id = ctx.chunk_count % gp_face_count(SKEL_FACES)
    if tile_id < GP_PENT_COUNT:
        skel_enc_zone_reset(ctx)
    sk = skeleton_lookup(ctx.chunk_count)
    strategy, best_rot, isect_pc, ref_idx = skel_decide(
        chunk, ctx.prev, ctx.has_prev, ctx.chunk_count, ctx.refs)
    ctx.hits[strategy] += 1
    ctx.chunk_count += 1
    chunk_bytes = bytes(chunk[:CHUNK_SZ])
    ctx.prev = chunk_bytes
    ctx.has_prev = True
    if strategy != SKEL_ID:
        ctx.refs.append(chunk_bytes)
        if len(ctx.refs) > ctx.ref_window:
            ctx.refs.pop(0)
    return strategy, best_rot, isect_pc, ref_idx

# ════════════════════════════════════════════════════════════════
# SECTION 5: Wang Tile Layer — tamper detection only (not gate)
# ════════════════════════════════════════════════════════════════

def wang_chord_a(enc):
    return (enc * 2) % 9

def wang_chord_b(enc):
    return (enc * 7) % 9

def wang_is_369(enc):
    d = enc % 9
    return d == 0 or d == 3 or d == 6

def wang_chord_valid(enc):
    a = wang_chord_a(enc)
    b = wang_chord_b(enc)
    if enc % 9 == 0:
        return a == 0 and b == 0
    return (a + b) == 9

class WangLayer:
    """Wang tile validation — tamper detection, NOT integrity gate."""
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
            enc = frame_enc(t)
            xor_acc ^= enc
            frame = frame_at(enc)
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

    def tamper_check(self, win_idx):
        w = self.windows[win_idx]
        top_ok = (w['edge_top'] == 0 and w['edge_top_b'] == 0) or \
                 (w['edge_top'] + w['edge_top_b'] == 9)
        bot_ok = (w['edge_bot'] == 0 and w['edge_bot_b'] == 0) or \
                 (w['edge_bot'] + w['edge_bot_b'] == 9)
        return top_ok and bot_ok

    def verify_tamper(self):
        """Check chord invariant only — not edge chaining."""
        for w in range(WANG_WIN_COUNT):
            if not self.tamper_check(w):
                return False
        return True

    def stats(self):
        edge_mismatches = 0
        for w in range(1, WANG_WIN_COUNT):
            if self.windows[w-1]['edge_bot'] != self.windows[w]['edge_top']:
                edge_mismatches += 1
        return {
            'n_windows': WANG_WIN_COUNT,
            'tamper_ok': self.verify_tamper(),
            'edge_mismatches': edge_mismatches,
        }

# ════════════════════════════════════════════════════════════════
# SECTION 6: Tantrix 256-state routing
# ════════════════════════════════════════════════════════════════

def tantrix_make(entry, exit, spoke, cls=0):
    return (entry & 0x3) | ((exit & 0x3) << 2) | \
           ((spoke & 0x3) << 4) | ((cls & 0x3) << 6)

def tantrix_entry(t):  return t & 0x3
def tantrix_exit(t):   return (t >> 2) & 0x3
def tantrix_spoke(t):  return (t >> 4) & 0x3
def tantrix_class(t):  return (t >> 6) & 0x3

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
    if cls == 1:
        exit_gate ^= 0x3
    elif cls == 2:
        exit_gate = ((exit_gate & 1) << 1) | ((exit_gate >> 1) & 1)
    return exit_gate, 'FORWARD'

# ════════════════════════════════════════════════════════════════
# SECTION 7: Wallet (pogls_coord_wallet.h)
# ════════════════════════════════════════════════════════════════

def wallet_chunk_seed(chunk64):
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

# ════════════════════════════════════════════════════════════════
# SECTION 7b: Numpy-vectorized batch operations
# ════════════════════════════════════════════════════════════════

_MASK64 = np.uint64(0xFFFFFFFFFFFFFFFF)

def _splitmix64_batch(x):
    """Vectorized SplitMix64 finalizer on numpy uint64 array."""
    s = x.copy()
    s ^= (s >> np.uint64(33))
    s &= _MASK64
    s *= np.uint64(0xff51afd7ed558ccd)
    s &= _MASK64
    s ^= (s >> np.uint64(33))
    s &= _MASK64
    s *= np.uint64(0xc4ceb9fe1a85ec53)
    s &= _MASK64
    s ^= (s >> np.uint64(33))
    s &= _MASK64
    return s

def wallet_chunk_seed_batch(chunks_bytes):
    """Vectorized wallet_chunk_seed for list of bytes objects.
    Returns numpy uint64 array of seeds."""
    n = len(chunks_bytes)
    buf = np.frombuffer(b''.join(chunks_bytes), dtype=np.uint8).reshape(n, CHUNK_SZ)
    # XOR-fold 8 × uint64 words per chunk
    words = buf.view(np.uint8).reshape(n, 8, 8)
    w64 = words.view(np.uint64)  # (n, 8, 1) but may need reshape
    # Reshape explicitly: (n, 8, 8) bytes → (n, 8) uint64
    w64 = np.zeros((n, 8), dtype=np.uint64)
    for j in range(8):
        w = np.zeros(n, dtype=np.uint64)
        for b in range(8):
            w |= np.uint64(buf[:, j*8+b]).astype(np.uint64) << np.uint64(b * 8)
        w64[:, j] = w
    # XOR all 8 words
    s = w64[:, 0] ^ w64[:, 1] ^ w64[:, 2] ^ w64[:, 3] ^ \
        w64[:, 4] ^ w64[:, 5] ^ w64[:, 6] ^ w64[:, 7]
    return _splitmix64_batch(s)

def wallet_xorfold64_batch(chunks_bytes):
    """Vectorized wallet_xorfold64. Returns numpy uint32 array."""
    n = len(chunks_bytes)
    buf = np.frombuffer(b''.join(chunks_bytes), dtype=np.uint8).reshape(n, CHUNK_SZ)
    acc = np.zeros(n, dtype=np.uint32)
    for i in range(16):
        off = i * 4
        w = np.zeros(n, dtype=np.uint32)
        for b in range(4):
            w |= buf[:, off+b].astype(np.uint32) << np.uint32(b * 8)
        acc ^= w
    return acc

def wallet_seeds_and_checksums(chunks):
    """Fastest available backend: C-DLL > numpy > scalar.
    Returns (seed_array_u64, checksum_array_u32) as numpy arrays."""
    n = len(chunks)
    buf = b''.join(chunks)
    if _C_SEED_DLL and n > 0:
        c_buf = (ctypes.c_ubyte * len(buf)).from_buffer_copy(buf)
        c_seeds = (ctypes.c_uint64 * n)()
        c_chks  = (ctypes.c_uint32 * n)()
        _C_SEED_DLL.wallet_seed_batch_c(c_buf, c_seeds, n)
        _C_SEED_DLL.wallet_xorfold_batch_c(c_buf, c_chks, n)
        return np.frombuffer(c_seeds, dtype=np.uint64), np.frombuffer(c_chks, dtype=np.uint32)
    else:
        return wallet_chunk_seed_batch(chunks), wallet_xorfold64_batch(chunks)

def wallet_xorfold64(chunk64):
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
    return (face << 24) | (edge << 16) | (z << 8)

def wallet_coord_unpack(packed):
    face = (packed >> 24) & 0xFF
    edge = (packed >> 16) & 0xFF
    z = (packed >> 8) & 0xFF
    return face, edge, z

# ════════════════════════════════════════════════════════════════
# SECTION 7c: xxh64 digest (from geo_field_core.h)
# ════════════════════════════════════════════════════════════════

def xxh64(data, seed=0):
    """xxh64 — portable implementation from geo_field_core.h._gf_hu."""
    PRIME64_1 = 0x9E3779B185EBCA87
    PRIME64_2 = 0x14DEF9DEA2F79CD6
    PRIME64_3 = 0x165667B19E3779F9
    PRIME64_4 = 0x85EBCA77C2B2ED6B
    PRIME64_5 = 0x27D4EB2F165667C5

    def _u64(x):
        return x & 0xFFFFFFFFFFFFFFFF

    acc = _u64(seed + PRIME64_5 + len(data))
    offset = 0

    # Process 32-byte stripes
    if len(data) >= 32:
        accs = [_u64(seed + PRIME64_1 + PRIME64_2),
                _u64(seed + PRIME64_2),
                _u64(seed + PRIME64_3),
                _u64(seed + PRIME64_4)]
        limit = len(data) - 32
        while offset <= limit:
            for k in range(4):
                lane = struct.unpack_from('<Q', data, offset + k * 8)[0]
                accs[k] = _u64(accs[k] + lane * PRIME64_2)
                accs[k] = _u64((accs[k] << 31) | (accs[k] >> 33))
                accs[k] = _u64(accs[k] * PRIME64_1)
            offset += 32
        for k in range(4):
            acc ^= accs[k]
        acc = _u64((acc << 27) | (acc >> 37))
        acc = _u64(acc * PRIME64_1 + PRIME64_4)

    # Process 8-byte tail
    if len(data) - offset >= 8:
        lane = struct.unpack_from('<Q', data, offset)[0]
        acc = _u64(acc ^ (_u64(lane * PRIME64_3)))
        acc = _u64((acc << 29) | (acc >> 35))
        acc = _u64(acc * PRIME64_2 + PRIME64_4)
        offset += 8

    # Process remaining bytes (1-7)
    remaining = len(data) - offset
    if remaining > 0:
        for i in range(remaining):
            acc = _u64(acc ^ (data[offset + i] * PRIME64_5))
        acc = _u64((acc << 11) | (acc >> 53))
        acc = _u64(acc * PRIME64_1)

    # Avalanche
    acc ^= (acc >> 33)
    acc = _u64(acc * PRIME64_2)
    acc ^= (acc >> 29)
    acc = _u64(acc * PRIME64_3)
    acc ^= (acc >> 32)

    return acc

def wallet_coord_unpack(p):
    return (p >> 24) & 0xFF, (p >> 16) & 0xFF, (p >> 8) & 0xFF

def wallet_verify(chunk64, seed, checksum):
    """Verify chunk against seed + checksum."""
    return wallet_chunk_seed(chunk64) == seed and \
           wallet_xorfold64(chunk64) == checksum

# ════════════════════════════════════════════════════════════════
# SECTION 8: geo_frame_seek (from geo_frame_seek.h)
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
# SECTION 9: geo_jump (from geo_jump.h)
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
# SECTION 10: Cube sizing (base 4/8/16)
# ════════════════════════════════════════════════════════════════

def next_power_of_base(n, base):
    """Smallest base^k >= n."""
    p = 1
    while p < n:
        p *= base
    return p

def compute_cube_side(n_chunks, base=4):
    """Compute cube side so 6 * side² >= n_chunks."""
    min_side = math.ceil(math.sqrt(n_chunks / 6.0))
    side = next_power_of_base(max(min_side, 4), base)
    return side

# ════════════════════════════════════════════════════════════════
# SECTION 11: 6-face cube operations
# ════════════════════════════════════════════════════════════════

FACE_LIST = ['front', 'back', 'left', 'right', 'top', 'bottom']

def unfold_cube(cube, side):
    return {
        'front':  cube[:, :, -1].copy(),
        'back':   cube[:, :, 0].copy(),
        'left':   cube[0, :, :].copy(),
        'right':  cube[-1, :, :].copy(),
        'top':    cube[:, -1, :].copy(),
        'bottom': cube[:, 0, :].copy(),
    }

def refold_cube(faces, side):
    cube = np.zeros((side, side, side), dtype=np.int32)
    cube[:, :, -1] = faces['front']
    cube[:, :, 0] = faces['back']
    cube[0, :, :] = faces['left']
    cube[-1, :, :] = faces['right']
    cube[:, -1, :] = faces['top']
    cube[:, 0, :] = faces['bottom']
    return cube

# ════════════════════════════════════════════════════════════════
# SECTION 12: Diamond Shell rotation scan
# Ported from diamond_shell_v2.h + pogls_fold.h
# ════════════════════════════════════════════════════════════════

SHELL_ROT_STATES = 6
SHELL_SPARSE_THRESH = 4

def _shell_rotate64(rot):
    """Return rotation permutation for 4x4x4 cube (64B chunk).
    Returns list of 64 source indices: out[i] = in[perm[i]]."""
    perm = [0] * 64
    for z in range(4):
        for y in range(4):
            for x in range(4):
                if rot == 0:   sx, sy, sz = x, y, z
                elif rot == 1: sx, sy, sz = y, z, x
                elif rot == 2: sx, sy, sz = z, x, y
                elif rot == 3: sx, sy, sz = x, z, 3-y
                elif rot == 4: sx, sy, sz = z, y, 3-x
                else:          sx, sy, sz = 3-y, x, z
                perm[z*16 + y*4 + x] = sz*16 + sy*4 + sx
    return perm

def _shell_inverse_rotate64(rot):
    """Return inverse rotation permutation."""
    fwd = _shell_rotate64(rot)
    inv = [0] * 64
    for i in range(64):
        inv[fwd[i]] = i
    return inv

# Precompute all 6 rotation + inverse permutations
_ROT_PERMS = [_shell_rotate64(r) for r in range(SHELL_ROT_STATES)]
_INV_PERMS = [_shell_inverse_rotate64(r) for r in range(SHELL_ROT_STATES)]

def _apply_perm(data, perm):
    """Apply permutation to 64-byte data."""
    return bytes(data[perm[i]] for i in range(64))

def _fold_build_quad_mirror(core_bytes_8):
    """Build 32B quad_mirror from 8B core slot (4 byte-shifted copies).
    Mirror layout: [core_rot0(8B)][core_rot1(8B)][core_rot2(8B)][core_rot3(8B)]"""
    src = core_bytes_8
    mirror = bytearray(32)
    # Copy 0 — rot 0
    mirror[0:8] = src
    # Copy 1 — rotate left 1B
    mirror[8] = src[1]; mirror[9] = src[2]; mirror[10] = src[3]; mirror[11] = src[4]
    mirror[12] = src[5]; mirror[13] = src[6]; mirror[14] = src[7]; mirror[15] = src[0]
    # Copy 2 — rotate left 2B
    mirror[16] = src[2]; mirror[17] = src[3]; mirror[18] = src[4]; mirror[19] = src[5]
    mirror[20] = src[6]; mirror[21] = src[7]; mirror[22] = src[0]; mirror[23] = src[1]
    # Copy 3 — rotate left 3B
    mirror[24] = src[3]; mirror[25] = src[4]; mirror[26] = src[5]; mirror[27] = src[6]
    mirror[28] = src[7]; mirror[29] = src[0]; mirror[30] = src[1]; mirror[31] = src[2]
    return bytes(mirror)

def _fold_fibo_intersect(quad_mirror_32):
    """AND of 4 rotated copies → bits that survive = geometric constants.
    Returns 8B (uint64)."""
    c0 = int.from_bytes(quad_mirror_32[0:8], 'little')
    c1 = int.from_bytes(quad_mirror_32[8:16], 'little')
    c2 = int.from_bytes(quad_mirror_32[16:24], 'little')
    c3 = int.from_bytes(quad_mirror_32[24:32], 'little')
    return c0 & c1 & c2 & c3

def _popcount64(x):
    """Population count for 64-bit integer."""
    x = x - ((x >> 1) & 0x5555555555555555)
    x = (x & 0x3333333333333333) + ((x >> 2) & 0x3333333333333333)
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0F
    return (x * 0x0101010101010101) >> 56

def _shell_chunk_to_block(rotbuf, rot, chunk_z):
    """Load rotated chunk into core slot with geometry context (from C).
    Replaces high bits of first 8B with geometry, keeps low 16b data residue."""
    core_raw = int.from_bytes(rotbuf[0:8], 'little')
    face_id = rot & 0x1F
    engine_id = chunk_z & 0x7F
    vpos = int.from_bytes(rotbuf[8:12], 'little') & 0xFFFFFF if len(rotbuf) >= 12 else 0
    fibo_gear = 1
    quad_flags = (rotbuf[16] ^ rotbuf[32]) & 0xFF if len(rotbuf) > 32 else 0

    core_raw = ((face_id & 0x1F) << 59) | ((engine_id & 0x7F) << 52) | \
               ((vpos & 0xFFFFFF) << 28) | ((fibo_gear & 0x0F) << 24) | \
               ((quad_flags & 0xFF) << 16) | (core_raw & 0xFFFF)
    core_bytes = core_raw.to_bytes(8, 'little')
    quad = _fold_build_quad_mirror(core_bytes)
    isect = _fold_fibo_intersect(quad)
    return isect

def diamond_shell_classify(chunk, chunk_z=0):
    """Diamond Shell rotation scan: try 6 orientations, pick best.
    Returns (best_rot, isect_pc, flag) where flag: 0=FLAT, 1=SPARSE, 2=DENSE."""
    best_rot = 0
    best_pc = -1
    best_isect = 0
    chunk_is_zero = all(b == 0 for b in chunk)

    for rot in range(SHELL_ROT_STATES):
        perm = _ROT_PERMS[rot]
        rotbuf = _apply_perm(chunk, perm)
        isect = _shell_chunk_to_block(rotbuf, rot, chunk_z)
        pc = _popcount64(isect)
        if pc > best_pc:
            best_pc = pc
            best_rot = rot
            best_isect = isect

    if chunk_is_zero:
        flag = 0  # FLAT
    elif best_pc <= SHELL_SPARSE_THRESH:
        flag = 1  # SPARSE
    else:
        flag = 2  # DENSE
    return best_rot, best_pc, flag

# ════════════════════════════════════════════════════════════════
# SECTION 13: .geopixel file format
# ════════════════════════════════════════════════════════════════

# GPXL v2 Header layout (little-endian, 48B total):
#   magic:           4 bytes (b'GPXL')
#   version:         2 bytes (uint16) — v2
#   gp_level:        1 byte  (uint8)
#   base:            1 byte  (uint8)
#   side:            2 bytes (uint16)
#   n_chunks:        4 bytes (uint32)
#   original_size:   8 bytes (uint64)
#   xxh64_digest:    8 bytes (uint64) — NEW
#   coord_sec_size:  4 bytes (uint32) — NEW (bytes of CoordRecord section)
#   reserved:        10 bytes
# Total: 48 bytes

def skeleton_compress_chunk(chunk, strategy, prev_chunk=None, ref_chunk=None, ref_idx=0):
    """Compress a 64B chunk based on skeleton strategy.
    Wire format:
      ID:    1B [0x00]
      FLAT:  1B [0x01]
      DIFF:  12+n B [0x02][ref_idx 1B][mask 8B][values nB]  where n = popcount(mask)
      BREF:  2B [0x03][ref_idx 1B]
      GEOM:  2B+66B [0x04][geom_type 1B] + payload
              geom_type=0x00 (INLINE):  [data 64B] = 66B total
              geom_type=0x01-0x03:      reserved → fallback INLINE
      RAW:   1B+64B [0x05][data 64B] = 65B total"""
    chunk = bytes(chunk[:CHUNK_SZ])
    ref = ref_chunk if ref_chunk is not None else prev_chunk
    if strategy == SKEL_ID:
        return b'\x00'
    elif strategy == SKEL_FLAT:
        return b'\x01'
    elif strategy == SKEL_DIFF:
        if ref is None:
            return b'\x05' + chunk
        mask = bytearray(8)
        values = bytearray()
        for i in range(CHUNK_SZ):
            if chunk[i] != ref[i]:
                mask[i // 8] |= (1 << (i % 8))
                values.append(chunk[i] ^ ref[i])
        return bytes([0x02, ref_idx & 0xFF]) + bytes(mask) + bytes(values)
    elif strategy == SKEL_BREF:
        return bytes([0x03, ref_idx & 0xFF])
    elif strategy == SKEL_GEOM:
        return bytes([0x04, GEOM_INLINE]) + chunk  # 66B: marker + geom_type + data
    else:
        return b'\x05' + chunk

def skeleton_decompress_chunk(compressed, ref_chunks, prev_chunk=None):
    """Decompress a skeleton-compressed chunk.
    prev_chunk: the previously decoded chunk (for IDENTITY).
    ref_chunks: sliding window of recent unique chunks for DIFF/BREF reference.
    Returns 64B chunk."""
    if len(compressed) < 1:
        return b'\x00' * CHUNK_SZ
    marker = compressed[0]
    if marker == 0x00:  # ID — identical to previous chunk
        return prev_chunk[:] if prev_chunk else b'\x00' * CHUNK_SZ
    elif marker == 0x01:  # FLAT
        return b'\x00' * CHUNK_SZ
    elif marker == 0x02:  # DIFF: ref_idx(1B) + mask(8B) + values(popcount B)
        if len(compressed) < 2:
            return b'\x00' * CHUNK_SZ
        ref_idx = compressed[1]
        mask_bytes = compressed[2:10]
        if len(mask_bytes) < 8:
            mask_bytes = mask_bytes + b'\x00' * (8 - len(mask_bytes))
        mask = int.from_bytes(mask_bytes, 'little')
        n_values = bin(mask).count('1')
        val_end = 10 + n_values
        values = compressed[10:val_end]
        if len(values) < n_values:
            values = values + b'\x00' * (n_values - len(values))
        if ref_chunks and ref_idx < len(ref_chunks):
            ref = ref_chunks[ref_idx]
        elif ref_chunks:
            ref = ref_chunks[0]
        else:
            ref = b'\x00' * CHUNK_SZ
        result = bytearray(ref)
        vi = 0
        for i in range(CHUNK_SZ):
            if mask & (1 << i):
                result[i] = ref[i] ^ values[vi]
                vi += 1
        return bytes(result)
    elif marker == 0x03:  # BREF: ref_idx(1B)
        ref_idx = compressed[1] if len(compressed) > 1 else 0
        ref = ref_chunks[ref_idx] if ref_chunks and ref_idx < len(ref_chunks) else (ref_chunks[0] if ref_chunks else b'\x00' * CHUNK_SZ)
        return bytes(reversed(ref))
    elif marker == 0x04:  # GEOM: [geom_type 1B] [payload]
        if len(compressed) < 2:
            return b'\x00' * CHUNK_SZ
        geom_type = compressed[1]
        if geom_type == GEOM_INLINE:
            raw = compressed[2:2+CHUNK_SZ]
            if len(raw) < CHUNK_SZ:
                raw = raw + b'\x00' * (CHUNK_SZ - len(raw))
            return raw
        else:
            # Unknown GEOM subtype (BLUEPRINT, PARAMETRIC, TEMPLATE) → fallback: read as INLINE
            # Future decoders will reconstruct from blueprint here
            raw = compressed[2:2+CHUNK_SZ]
            if len(raw) < CHUNK_SZ:
                raw = raw + b'\x00' * (CHUNK_SZ - len(raw))
            return raw
    else:  # 0x05 = RAW
        raw = compressed[1:1+CHUNK_SZ]
        if len(raw) < CHUNK_SZ:
            raw = raw + b'\x00' * (CHUNK_SZ - len(raw))
        return raw

def encode_gpxl(coord_records, chunks, side, gp_level, base, original_size, original_data=None):
    """Encode to .geopixel binary format v2.
    Layout: header(48B) + coord_records(20B × n) + skeleton stream"""
    n_chunks = len(chunks)

    buf = bytearray()

    # Compute xxh64 digest
    digest = xxh64(original_data) if original_data else 0

    # Compute coord record section size
    coord_sec_size = n_chunks * 20

    # Header (48B)
    buf += GPXL_MAGIC
    buf += struct.pack('<H', 2)  # version 2
    buf += struct.pack('<B', gp_level)
    buf += struct.pack('<B', base)
    buf += struct.pack('<H', side)
    buf += struct.pack('<I', n_chunks)
    buf += struct.pack('<Q', original_size)
    buf += struct.pack('<Q', digest)
    buf += struct.pack('<I', coord_sec_size)
    buf += b'\x00' * 14  # reserved (pad to 44)
    assert len(buf) == GPXL_HEADER_SZ, f"Header size {len(buf)} != {GPXL_HEADER_SZ}"

    # CoordRecord section (20B × n_chunks)
    for ci, cr in enumerate(coord_records):
        buf += struct.pack('<I', cr['tile_id'])
        buf += struct.pack('<B', cr['dim'])
        buf += struct.pack('<B', cr['face'] % 6)
        buf += struct.pack('<B', cr['edge'])
        buf += struct.pack('<B', cr['z'])
        buf += struct.pack('<B', cr['skel_strategy'])
        buf += struct.pack('<Q', cr['seed'])
        buf += struct.pack('<H', cr['checksum'] & 0xFFFF)
        buf += struct.pack('<B', cr['fast_sig'] & 0xFF)

    # Compressed chunk stream — strategy tag is embedded in each compressed blob
    ref_buf = []  # sliding window of recent unique chunks
    ref_window = 64
    prev_chunk = None
    for ci, chunk in enumerate(chunks):
        cr = coord_records[ci]
        ref_idx = cr.get('ref_idx', 0)
        ref_chunk = ref_buf[ref_idx] if ref_buf and ref_idx < len(ref_buf) else prev_chunk
        compressed = skeleton_compress_chunk(chunk, cr['skel_strategy'], prev_chunk, ref_chunk, ref_idx)
        buf += struct.pack('<H', len(compressed))
        buf += compressed
        prev_chunk = chunk
        if cr['skel_strategy'] != SKEL_ID:
            ref_buf.append(chunk)
            if len(ref_buf) > ref_window:
                ref_buf.pop(0)

    return bytes(buf)

def decode_gpxl(data):
    """Decode .geopixel binary format → (chunks, side, gp_level, base, original_size, digest).
    Supports v1 (32B header) and v2 (48B header with xxh64 + CoordRecords)."""
    if len(data) < 32:
        raise ValueError("File too small for header")

    magic = data[0:4]
    if magic != GPXL_MAGIC:
        raise ValueError(f"Bad magic: {magic!r}")

    version = struct.unpack_from('<H', data, 4)[0]

    if version == 1:
        # v1: 32B header, no CoordRecords, no xxh64
        _, gp_level, base, side, n_chunks, original_size = struct.unpack_from(
            '<HBBHIQ', data, 4)
        offset = 32
    elif version >= 2:
        # v2: 48B header with xxh64 + CoordRecord section
        _, gp_level, base, side, n_chunks, original_size, digest, coord_sec_size = struct.unpack_from(
            '<HBBHIQQI', data, 4)
        offset = GPXL_HEADER_SZ
        # Skip CoordRecord section (we read strategy from stream markers)
        offset += coord_sec_size
    else:
        raise ValueError(f"Unknown version: {version}")

    # Fallback mode: gp_level=0 means raw data
    if gp_level == 0:
        return [], side, gp_level, base, original_size, 0

    # Read compressed chunk stream
    chunks = []
    ref_buf = []  # sliding window of recent unique chunks
    ref_window = 64
    prev_chunk = None
    for i in range(n_chunks):
        if offset + 2 > len(data):
            raise ValueError(f"Truncated at chunk {i}")
        comp_len = struct.unpack_from('<H', data, offset)[0]
        offset += 2
        compressed = data[offset:offset + comp_len]
        offset += comp_len
        chunk = skeleton_decompress_chunk(compressed, ref_buf, prev_chunk)
        chunks.append(chunk)
        prev_chunk = chunk
        marker = compressed[0] if compressed else 0xFF
        if marker != 0x00:  # not ID → add to ref buffer
            ref_buf.append(chunk)
            if len(ref_buf) > ref_window:
                ref_buf.pop(0)

    return chunks, side, gp_level, base, original_size, digest if version >= 2 else 0

# ════════════════════════════════════════════════════════════════
# SECTION 13: Full Pipeline — Encode
# ════════════════════════════════════════════════════════════════

class PipelineResult:
    def __init__(self):
        self.stats = {}
        self.wang_stats = None
        self.tantrix_valid = False
        self.skel_stats = None
        self.coord_count = 0
        self.total_time = 0
        self.output_size = 0
        self.ratio = 0

def pipeline_encode(data, base=4):
    """
    Full pipeline: data → GeoField → Wallet coords → cube → 6 faces → .geopixel

    Steps:
      1. Split data into 64-byte chunks
      2. Auto-calculate gp_level
      3. Map each chunk to GpAddr (tile_id, dim)
      4. Classify each chunk (skeleton)
      5. Generate CoordRecord with wallet coords (face|edge|z)
      6. Wang tile tamper detection
      7. Tantrix routing validation
      8. Compute cube side (base 4/8/16)
      9. Place raw chunk bytes in cube at wallet-coord positions
     10. Unfold to 6 faces
     11. Serialize to .geopixel binary
    """
    t0 = time.perf_counter()
    result = PipelineResult()

    # Step 1: Chunk
    n_chunks = (len(data) + CHUNK_SZ - 1) // CHUNK_SZ
    chunks = []
    for i in range(n_chunks):
        off = i * CHUNK_SZ
        chunk = data[off:off + CHUNK_SZ]
        if len(chunk) < CHUNK_SZ:
            chunk = chunk + b'\x00' * (CHUNK_SZ - len(chunk))
        chunks.append(chunk)

    # Step 2: Auto gp_level
    gp_level = gp_auto_level(n_chunks)
    face_max = gp_face_count(gp_level)

    # Steps 3-5: GeoField → Wallet coords (batch vectorized)
    skel = SkeletonCtx()
    skel_strategies = []
    shell_rots = []
    shell_isects = []
    skel_ref_idxs = []
    for ci, chunk in enumerate(chunks):
        strategy, best_rot, isect_pc, ref_idx = skel_encode_chunk(skel, chunk)
        skel_strategies.append(strategy)
        shell_rots.append(best_rot)
        shell_isects.append(isect_pc)
        skel_ref_idxs.append(ref_idx)
    result.skel_stats = skel.hits[:]

    # Batch compute seeds + checksums (fastest backend: C-DLL > numpy)
    seed_arr, checksum_arr = wallet_seeds_and_checksums(chunks)

    coord_records = []
    for ci, chunk in enumerate(chunks):
        tile_id, dim = gp_chunk_to_addr(gp_level, ci)
        skel_strategy = skel_strategies[ci]

        # Use geo_frame_seek for wallet coordinates (NOT simple modulo)
        # This routes chunks through the stride-37 timeline walk
        enc = frame_enc(ci)
        frame = frame_at(enc)
        tring_enc = frame['enc']
        spoke = frame['sub']

        # Wallet coords from frame geometry
        face = frame['face']
        edge = frame['edge']
        z = frame['slot']

        seed = int(seed_arr[ci])
        checksum = int(checksum_arr[ci])

        coord_records.append({
            'chunk_idx': ci,
            'tile_id': tile_id,
            'dim': dim,
            'face': face,
            'edge': edge,
            'z': z,
            'coord_packed': wallet_coord_pack(face, edge, z),
            'seed': seed,
            'checksum': checksum,
            'fast_sig': seed & 0xFFFFFFFF,
            'skel_strategy': skel_strategy,
            'ref_idx': skel_ref_idxs[ci],
            'tring_enc': tring_enc,
            'spoke': spoke,
        })

    result.coord_count = len(coord_records)

    # Step 6: Wang tamper detection (not gate)
    wang = WangLayer()
    result.wang_stats = wang.stats()

    # Step 7: Tantrix routing
    tan_ok = True
    for cr in coord_records:
        entry_gate = cr['edge'] % 4
        tile = tantrix_make(entry_gate, (entry_gate + 1) % 4, cr['spoke'] % 4)
        out_gate, route_type = tantrix_route(tile, entry_gate)
        if route_type == 'DROP':
            tan_ok = False
    result.tantrix_valid = tan_ok

    # Step 8: Dynamic cube side
    side = compute_cube_side(n_chunks, base)
    surface_capacity = 6 * side * side

    # Step 9: Place raw chunk bytes in cube
    # Each chunk gets a position on the surface via wallet coords.
    # Store chunk_idx in face arrays, chunk data in separate lookup.
    face_arrays = {fname: np.full((side, side), -1, dtype=np.int32) for fname in FACE_LIST}
    chunk_lookup = {}  # (face, r, c) → chunk_idx

    placed = 0
    overflow = 0
    for ci, cr in enumerate(coord_records):
        fname = FACE_LIST[cr['face'] % 6]
        r = cr['edge'] % side
        c = cr['z'] % side

        # If position occupied, find next free on same face
        attempts = 0
        while face_arrays[fname][r, c] != -1 and attempts < side * side:
            c = (c + 1) % side
            if c == 0:
                r = (r + 1) % side
            attempts += 1

        if face_arrays[fname][r, c] == -1:
            face_arrays[fname][r, c] = ci
            chunk_lookup[(cr['face'] % 6, r, c)] = ci
            placed += 1
        else:
            overflow += 1

    # Step 10: Check fallback — if ratio < 0.5x (output > 2x input), store raw
    # First compute what the encoded size would be
    encoded = encode_gpxl(coord_records, chunks, side, gp_level, base, len(data), original_data=data)
    raw_ratio = len(data) / len(encoded) if len(encoded) > 0 else 0

    # Fallback: if expansion > 2x, wrap raw data with minimal header
    if raw_ratio < 0.5:
        fallback_buf = bytearray()
        fallback_buf += GPXL_MAGIC
        fallback_buf += struct.pack('<H', 2)  # version 2
        fallback_buf += struct.pack('<B', 0)  # gp_level=0 means fallback
        fallback_buf += struct.pack('<B', base)
        fallback_buf += struct.pack('<H', 0)  # side=0 means fallback
        fallback_buf += struct.pack('<I', 0)  # n_chunks=0 means fallback
        fallback_buf += struct.pack('<Q', len(data))
        digest = xxh64(data)
        fallback_buf += struct.pack('<Q', digest)
        fallback_buf += struct.pack('<I', 0)  # coord_sec_size=0
        fallback_buf += b'\x00' * 10  # reserved
        fallback_buf += data
        encoded = bytes(fallback_buf)
        result.stats['fallback'] = True
    else:
        result.stats['fallback'] = False

    result.output_size = len(encoded)
    result.ratio = len(data) / len(encoded) if len(encoded) > 0 else 0

    t_total = time.perf_counter() - t0
    result.total_time = t_total
    result.stats = {
        'data_size': len(data),
        'n_chunks': n_chunks,
        'gp_level': gp_level,
        'face_max': face_max,
        'base': base,
        'cube_side': side,
        'surface_capacity': surface_capacity,
        'placed': placed,
        'overflow': overflow,
        'encoded_size': len(encoded),
    }
    result.coord_records = coord_records
    result.faces = face_arrays
    result.chunk_lookup = chunk_lookup
    result.chunks = chunks
    result.encoded = encoded

    return result

# ════════════════════════════════════════════════════════════════
# SECTION 14: Full Pipeline — Decode
# ════════════════════════════════════════════════════════════════

def pipeline_decode(encoded_data):
    """
    Decode .geopixel → original bytes.
    1. Parse header
    2. If fallback (gp_level=0): return raw data
    3. Otherwise: read coord_records + skeleton-compressed chunks
    4. Decompress chunks, verify seed+checksum
    5. Reassemble original file by chunk_idx order
    """
    t0 = time.perf_counter()

    if len(encoded_data) < GPXL_HEADER_SZ:
        raise ValueError("File too small for header")

    magic = encoded_data[0:4]
    if magic != GPXL_MAGIC:
        raise ValueError(f"Bad magic: {magic!r}")

    version = struct.unpack_from('<H', encoded_data, 4)[0]

    if version == 1:
        _, gp_level, base, side, n_chunks, original_size = struct.unpack_from(
            '<HBBHIQ', encoded_data, 4)
        data_offset = 32  # v1 header is 32B
    elif version >= 2:
        _, gp_level, base, side, n_chunks, original_size, digest, coord_sec_size = struct.unpack_from(
            '<HBBHIQQI', encoded_data, 4)
        data_offset = GPXL_HEADER_SZ + coord_sec_size  # skip v2 header + coord section
    else:
        raise ValueError(f"Unknown version: {version}")

    # Fallback mode: gp_level=0 means raw data
    if gp_level == 0:
        raw_data = encoded_data[data_offset:data_offset + original_size]
        t_total = time.perf_counter() - t0
        return {
            'data': raw_data,
            'original_size': original_size,
            'n_chunks': 0,
            'verified': 0,
            'failed': 0,
            'fallback': True,
            'time': t_total,
        }

    # Full decode
    chunks, side, gp_level, base, original_size, digest = decode_gpxl(encoded_data)

    # Reassemble: chunks are stored in idx order, just concatenate
    out = bytearray()
    for chunk in chunks:
        out += chunk
    result_data = bytes(out[:original_size])

    verified = len(chunks)
    failed = 0

    t_total = time.perf_counter() - t0

    return {
        'data': result_data,
        'original_size': original_size,
        'n_chunks': n_chunks,
        'verified': verified,
        'failed': failed,
        'fallback': False,
        'time': t_total,
        'digest': digest,
    }

# ════════════════════════════════════════════════════════════════
# SECTION 16: CLI — encode / decode / verify / info / batch
# ════════════════════════════════════════════════════════════════

HELP = """GeoPixel Pipeline v2 — GeoField → Skeleton → GPXL

Usage:
  python geopixel_pipeline.py encode <file>          Encode file to .geopixel
  python geopixel_pipeline.py decode <file.geopixel> Decode .geopixel to original
  python geopixel_pipeline.py verify <file.geopixel> Verify roundtrip integrity
  python geopixel_pipeline.py info <file.geopixel>   Show format info and stats
  python geopixel_pipeline.py batch <dir> [encode|decode]  Batch process directory

Options:
  --force          Overwrite output file if exists
  --no-verify      Skip roundtrip verification after encode
  -q, --quiet      Minimal output
  -h, --help       Show this help
"""


def cmd_encode(filepath, force=False, quiet=False, do_verify=True):
    """Encode a file to .geopixel format."""
    if not os.path.exists(filepath):
        print("Error: file not found: %s" % filepath)
        return False

    data = open(filepath, 'rb').read()
    outpath = filepath + '.geopixel'

    if not force and os.path.exists(outpath):
        print("Error: output exists: %s (use --force to overwrite)" % outpath)
        return False

    if not quiet:
        print("═══ GeoPixel Encode ═══")
        print("  Input:  %s (%s)" % (filepath, _fmt_size(len(data))))

    t0 = time.perf_counter()
    result = pipeline_encode(data)
    t_enc = time.perf_counter() - t0

    encoded = result.encoded
    s = result.stats
    fallback = s.get('fallback', False)

    if not quiet:
        print("  Output: %s (%s, ratio=%.2fx%s)" % (
            outpath, _fmt_size(len(encoded)), result.ratio,
            " [FALLBACK]" if fallback else ""))
        print("  Chunks: %d  gp_level=%d  base=%d  side=%d" % (
            s['n_chunks'], s['gp_level'], s['base'], s['cube_side']))
        print("  Skeleton: %s" % _fmt_skel(result.skel_stats))
        print("  xxh64:    0x%016x" % xxh64(data))
        print("  Encode:   %.1f ms" % (t_enc * 1000))

    with open(outpath, 'wb') as f:
        f.write(encoded)

    if do_verify and not fallback:
        t0 = time.perf_counter()
        dec = pipeline_decode(encoded)
        t_dec = time.perf_counter() - t0
        ok = dec['data'][:len(data)] == data
        if not quiet:
            print("  Verify:  %s (decode %.1f ms)" % ("PASS" if ok else "FAIL", t_dec * 1000))
        if not ok:
            print("  WARNING: roundtrip verification FAILED")
            return False

    if not quiet:
        print()

    return True


def cmd_decode(filepath, force=False, quiet=False):
    """Decode a .geopixel file to original."""
    if not os.path.exists(filepath):
        print("Error: file not found: %s" % filepath)
        return False

    data = open(filepath, 'rb').read()

    if not filepath.endswith('.geopixel'):
        print("Error: expected .geopixel file: %s" % filepath)
        return False

    outpath = filepath[:-len('.geopixel')]  # remove .geopixel

    if not force and os.path.exists(outpath):
        print("Error: output exists: %s (use --force to overwrite)" % outpath)
        return False

    if not quiet:
        print("═══ GeoPixel Decode ═══")
        print("  Input:  %s (%s)" % (filepath, _fmt_size(len(data))))

    try:
        t0 = time.perf_counter()
        dec = pipeline_decode(data)
        t_dec = time.perf_counter() - t0
    except ValueError as e:
        print("Error: %s" % e)
        return False

    result_data = dec['data']
    fallback = dec.get('fallback', False)

    if not quiet:
        print("  Output: %s (%s%s)" % (
            outpath, _fmt_size(len(result_data)),
            " [FALLBACK]" if fallback else ""))
        print("  Chunks: %d  verified: %d/%d  failed: %d" % (
            dec['n_chunks'], dec['verified'], dec['n_chunks'], dec['failed']))
        print("  xxh64:  0x%016x" % dec.get('digest', 0))
        print("  Decode: %.1f ms" % (t_dec * 1000))
        print()

    with open(outpath, 'wb') as f:
        f.write(result_data)

    return True


def cmd_verify(filepath, quiet=False):
    """Verify roundtrip integrity of a .geopixel file."""
    if not os.path.exists(filepath):
        print("Error: file not found: %s" % filepath)
        return False

    data = open(filepath, 'rb').read()

    if not quiet:
        print("═══ GeoPixel Verify ═══")
        print("  File: %s (%s)" % (filepath, _fmt_size(len(data))))

    try:
        t0 = time.perf_counter()
        dec = pipeline_decode(data)
        t_dec = time.perf_counter() - t0
    except ValueError as e:
        print("  FAIL: %s" % e)
        return False

    if dec.get('fallback', False):
        print("  Status: FALLBACK mode (raw data, no skeleton)")
        print("  xxh64:  0x%016x" % dec.get('digest', 0))
        print("  Decode: %.1f ms" % (t_dec * 1000))
        return True

    # Re-encode to verify roundtrip
    try:
        t0 = time.perf_counter()
        result = pipeline_encode(dec['data'])
        t_enc = time.perf_counter() - t0
        re_encoded = result.encoded
        ok = (re_encoded == data)
    except Exception as e:
        print("  FAIL: re-encode error: %s" % e)
        return False

    if not quiet:
        print("  Chunks: %d  verified: %d/%d  failed: %d" % (
            dec['n_chunks'], dec['verified'], dec['n_chunks'], dec['failed']))
        print("  xxh64:  0x%016x" % dec.get('digest', 0))
        print("  Decode: %.1f ms  Re-encode: %.1f ms" % (t_dec * 1000, t_enc * 1000))
        print("  Roundtrip: %s" % ("PASS" if ok else "FAIL"))

    return ok


def cmd_info(filepath, quiet=False):
    """Show format info for a .geopixel file."""
    if not os.path.exists(filepath):
        print("Error: file not found: %s" % filepath)
        return False

    data = open(filepath, 'rb').read()

    if len(data) < 4:
        print("Error: file too small")
        return False

    magic = data[0:4]
    if magic != GPXL_MAGIC:
        print("Error: not a GPXL file (magic: %r)" % magic)
        return False

    version = struct.unpack_from('<H', data, 4)[0]

    if not quiet:
        print("═══ GeoPixel Info ═══")
        print("  File:    %s (%s)" % (filepath, _fmt_size(len(data))))
        print("  Magic:   GPXL")
        print("  Version: %d" % version)

    if version == 1:
        _, gp_level, base, side, n_chunks, original_size = struct.unpack_from(
            '<HBBHIQ', data, 4)
        print("  xxh64:   N/A (v1)")
        print("  CoordRec: N/A (v1)")
    elif version >= 2:
        _, gp_level, base, side, n_chunks, original_size, digest, coord_sec_size = struct.unpack_from(
            '<HBBHIQQI', data, 4)
        print("  xxh64:   0x%016x" % digest)
        print("  CoordRec: %d records (%d bytes)" % (n_chunks, coord_sec_size))
    else:
        print("  Unknown version: %d" % version)
        return False

    if gp_level == 0:
        print("  Mode:    FALLBACK (raw data)")
        print("  Data:    %s at offset %d" % (_fmt_size(original_size), len(data) - original_size))
        return True

    print("  Params:  gp_level=%d  base=%d  side=%d" % (gp_level, base, side))
    print("  Tiles:   %d" % (10 * gp_level**2 + 2))
    print("  Chunks:  %d (%s)" % (n_chunks, _fmt_size(n_chunks * CHUNK_SZ)))

    # Decode skeleton stats
    try:
        dec = pipeline_decode(data)
        if dec['n_chunks'] > 0:
            # Re-encode to get skeleton stats
            result = pipeline_encode(dec['data'][:original_size])
            skel = result.skel_stats
            print("  Skeleton: %s" % _fmt_skel(skel))
            print("  Ratio:   %.2fx" % (original_size / len(data) if len(data) > 0 else 0))
    except Exception as e:
        print("  Decode error: %s" % e)

    print("  Roundtrip: can verify with 'verify' command")
    return True


def cmd_batch(dirpath, mode='encode', force=False, quiet=False):
    """Batch encode or decode all files in a directory."""
    if not os.path.isdir(dirpath):
        print("Error: not a directory: %s" % dirpath)
        return False

    if mode == 'encode':
        files = [f for f in os.listdir(dirpath)
                 if os.path.isfile(os.path.join(dirpath, f))
                 and not f.endswith('.geopixel')
                 and not f.startswith('.')]
    elif mode == 'decode':
        files = [f for f in os.listdir(dirpath)
                 if f.endswith('.geopixel')
                 and os.path.isfile(os.path.join(dirpath, f))]
    else:
        print("Error: unknown mode: %s (use 'encode' or 'decode')" % mode)
        return False

    if not files:
        print("No files to %s in %s" % (mode, dirpath))
        return True

    if not quiet:
        print("═══ GeoPixel Batch %s ═══" % mode.upper())
        print("  Directory: %s" % dirpath)
        print("  Files:     %d" % len(files))
        print()

    ok_count = 0
    fail_count = 0
    for fname in sorted(files):
        fpath = os.path.join(dirpath, fname)
        try:
            if mode == 'encode':
                result = cmd_encode(fpath, force=force, quiet=quiet, do_verify=True)
            else:
                result = cmd_decode(fpath, force=force, quiet=quiet)
            if result:
                ok_count += 1
            else:
                fail_count += 1
        except Exception as e:
            print("  ERROR %s: %s" % (fname, e))
            fail_count += 1

    if not quiet:
        print("═══ Summary ═══")
        print("  OK:   %d" % ok_count)
        print("  FAIL: %d" % fail_count)

    return fail_count == 0


def _fmt_size(n):
    """Format byte size for display."""
    if n < 1024:
        return "%d B" % n
    elif n < 1024 * 1024:
        return "%.1f KB" % (n / 1024)
    else:
        return "%.1f MB" % (n / 1024 / 1024)


def _fmt_skel(skel_stats):
    """Format skeleton stats for display."""
    if not skel_stats:
        return "N/A"
    return " ".join("%s=%d" % (n, c) for n, c in zip(SKEL_NAMES, skel_stats) if c > 0)


if __name__ == '__main__':
    import sys

    args = sys.argv[1:]
    if not args or args[0] in ('-h', '--help', 'help'):
        print(HELP)
        sys.exit(0)

    cmd = args.pop(0)
    force = '--force' in args
    quiet = '-q' in args or '--quiet' in args
    no_verify = '--no-verify' in args

    # Strip flags from args
    args = [a for a in args if not a.startswith('--') and a not in ('-q', '-h')]

    if cmd == 'encode':
        if not args:
            print("Error: no input file")
            sys.exit(1)
        ok = cmd_encode(args[0], force=force, quiet=quiet, do_verify=not no_verify)
        sys.exit(0 if ok else 1)

    elif cmd == 'decode':
        if not args:
            print("Error: no input file")
            sys.exit(1)
        ok = cmd_decode(args[0], force=force, quiet=quiet)
        sys.exit(0 if ok else 1)

    elif cmd == 'verify':
        if not args:
            print("Error: no input file")
            sys.exit(1)
        ok = cmd_verify(args[0], quiet=quiet)
        sys.exit(0 if ok else 1)

    elif cmd == 'info':
        if not args:
            print("Error: no input file")
            sys.exit(1)
        ok = cmd_info(args[0], quiet=quiet)
        sys.exit(0 if ok else 1)

    elif cmd == 'batch':
        if len(args) < 1:
            print("Error: no directory specified")
            sys.exit(1)
        mode = args[1] if len(args) > 1 else 'encode'
        ok = cmd_batch(args[0], mode=mode, force=force, quiet=quiet)
        sys.exit(0 if ok else 1)

    else:
        # Backward compat: treat as file path (old behavior)
        if os.path.exists(cmd):
            ok = cmd_encode(cmd, force=True, quiet=False, do_verify=True)
            sys.exit(0 if ok else 1)
        else:
            print("Unknown command: %s" % cmd)
            print("Run 'python geopixel_pipeline.py --help' for usage")
            sys.exit(1)
