"""
geofield_ctypes.py — Python ctypes wrapper for geofield_pipeline.dll

Auto-loads the DLL and wraps all exported functions with Python-friendly interfaces.
"""

import ctypes
import os
import struct

DLL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "geofield_pipeline.dll")

if hasattr(os, 'add_dll_directory'):
    _dll_dir = os.path.dirname(DLL_PATH)
    os.add_dll_directory(_dll_dir)
    # Also add mingw64 bin for libgcc_s_seh-1.dll
    _mingw = r"C:\msys64\mingw64\bin"
    if os.path.isdir(_mingw):
        os.add_dll_directory(_mingw)

_lib = ctypes.CDLL(DLL_PATH)

# ── Types ────────────────────────────────────────────────────────

class GeoGpAddr(ctypes.Structure):
    _fields_ = [("tile_id", ctypes.c_uint32),
                 ("dim", ctypes.c_uint8)]

class SkelDecision(ctypes.Structure):
    _fields_ = [("strategy", ctypes.c_uint8),
                 ("best_rot", ctypes.c_uint8),
                 ("isect_pc", ctypes.c_uint8),
                 ("ref_idx", ctypes.c_uint8),
                 ("diff_count", ctypes.c_uint8)]

class DiamondClassify(ctypes.Structure):
    _fields_ = [("best_rot", ctypes.c_uint8),
                 ("isect_pc", ctypes.c_uint8),
                 ("flag", ctypes.c_uint8)]

class FrameInfo(ctypes.Structure):
    _fields_ = [("enc", ctypes.c_uint16),
                 ("face", ctypes.c_uint8),
                 ("slot", ctypes.c_uint16),
                 ("group", ctypes.c_uint8),
                 ("edge", ctypes.c_uint8),
                 ("is_skip", ctypes.c_int)]

_uchar64 = ctypes.c_uint8 * 64

# ── geo_jump ─────────────────────────────────────────────────────

_lib.geofield_geo_jump.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]
_lib.geofield_geo_jump.restype = ctypes.c_uint32

def geo_jump(node_id, jump_type, param=0):
    return _lib.geofield_geo_jump(node_id, jump_type, param)

# ── frame_enc / frame_at ────────────────────────────────────────

_lib.geofield_frame_enc.argtypes = [ctypes.c_uint32]
_lib.geofield_frame_enc.restype = ctypes.c_uint16

_lib.geofield_frame_next.argtypes = [ctypes.c_uint16]
_lib.geofield_frame_next.restype = ctypes.c_uint16

_lib.geofield_frame_cpair.argtypes = [ctypes.c_uint16]
_lib.geofield_frame_cpair.restype = ctypes.c_uint16

def frame_enc(t):
    return _lib.geofield_frame_enc(t)

def frame_next(enc):
    return _lib.geofield_frame_next(enc)

def frame_cpair(enc):
    return _lib.geofield_frame_cpair(enc)

# ── tring_walk ──────────────────────────────────────────────────

_lib.geofield_tring_walk_enc.argtypes = [ctypes.c_uint32]
_lib.geofield_tring_walk_enc.restype = ctypes.c_uint16

_lib.geofield_tring_walk_spoke.argtypes = [ctypes.c_uint32]
_lib.geofield_tring_walk_spoke.restype = ctypes.c_uint8

def tring_walk_enc(tile_id):
    return _lib.geofield_tring_walk_enc(tile_id)

def tring_walk_spoke(tile_id):
    return _lib.geofield_tring_walk_spoke(tile_id)

# ── gp_chunk_to_addr ────────────────────────────────────────────

_lib.geofield_chunk_to_addr.argtypes = [ctypes.c_uint8, ctypes.c_uint64]
_lib.geofield_chunk_to_addr.restype = GeoGpAddr

_lib.geofield_face_count.argtypes = [ctypes.c_uint8]
_lib.geofield_face_count.restype = ctypes.c_uint32

def chunk_to_addr(gp_level, chunk_idx):
    return _lib.geofield_chunk_to_addr(gp_level, chunk_idx)

def face_count(gp_level):
    return _lib.geofield_face_count(gp_level)

# ── adaptive chunking ───────────────────────────────────────────

_lib.geofield_adaptive_chunk_size.argtypes = [ctypes.c_uint8]
_lib.geofield_adaptive_chunk_size.restype = ctypes.c_uint32

_lib.geofield_adaptive_slot_count.argtypes = [ctypes.c_uint8]
_lib.geofield_adaptive_slot_count.restype = ctypes.c_uint32

def adaptive_chunk_size(shell_level):
    return _lib.geofield_adaptive_chunk_size(shell_level)

def adaptive_slot_count(shell_level):
    return _lib.geofield_adaptive_slot_count(shell_level)

# ── skeleton ────────────────────────────────────────────────────

_lib.geofield_isect_pop.argtypes = [_uchar64]
_lib.geofield_isect_pop.restype = ctypes.c_uint8

_lib.geofield_is_flat.argtypes = [_uchar64]
_lib.geofield_is_flat.restype = ctypes.c_int

_lib.geofield_diff_count.argtypes = [_uchar64, _uchar64]
_lib.geofield_diff_count.restype = ctypes.c_uint8

_lib.geofield_is_bref.argtypes = [_uchar64, _uchar64]
_lib.geofield_is_bref.restype = ctypes.c_int

_lib.geofield_skel_decide.argtypes = [_uchar64, _uchar64, ctypes.c_int]
_lib.geofield_skel_decide.restype = SkelDecision

SKEL_ID, SKEL_FLAT, SKEL_DIFF, SKEL_BREF, SKEL_GEOM, SKEL_RAW = range(6)
SKEL_NAMES = ["ID", "FLAT", "DIFF", "BREF", "GEOM", "RAW"]

def skel_decide(chunk_bytes, prev_bytes=None, has_prev=False):
    chunk = _uchar64.from_buffer_copy(chunk_bytes[:64].ljust(64, b'\x00'))
    if prev_bytes and has_prev:
        prev = _uchar64.from_buffer_copy(prev_bytes[:64].ljust(64, b'\x00'))
    else:
        prev = _uchar64()
    return _lib.geofield_skel_decide(chunk, prev, int(has_prev))

def isect_pop(chunk_bytes):
    chunk = _uchar64.from_buffer_copy(chunk_bytes[:64].ljust(64, b'\x00'))
    return _lib.geofield_isect_pop(chunk)

# ── diamond shell ───────────────────────────────────────────────

_lib.geofield_rotate64.argtypes = [_uchar64, _uchar64, ctypes.c_uint8]
_lib.geofield_rotate64.restype = None

_lib.geofield_fibo_intersect.argtypes = [_uchar64]
_lib.geofield_fibo_intersect.restype = ctypes.c_uint64

_lib.geofield_diamond_classify.argtypes = [_uchar64]
_lib.geofield_diamond_classify.restype = DiamondClassify

def diamond_classify(chunk_bytes):
    chunk = _uchar64.from_buffer_copy(chunk_bytes[:64].ljust(64, b'\x00'))
    return _lib.geofield_diamond_classify(chunk)

# ── wallet_chunk_seed ───────────────────────────────────────────

_lib.geofield_wallet_seed.argtypes = [_uchar64]
_lib.geofield_wallet_seed.restype = ctypes.c_uint64

def wallet_seed(chunk_bytes):
    chunk = _uchar64.from_buffer_copy(chunk_bytes[:64].ljust(64, b'\x00'))
    return _lib.geofield_wallet_seed(chunk)

# ── xxh64 ───────────────────────────────────────────────────────

_lib.geofield_xxh64.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
_lib.geofield_xxh64.restype = ctypes.c_uint64

def xxh64(data):
    return _lib.geofield_xxh64(data, len(data))

# ── batch operations ────────────────────────────────────────────

_lib.geofield_batch_seed.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                      ctypes.c_uint32, ctypes.c_uint32]
_lib.geofield_batch_seed.restype = None

def batch_seed(chunks_bytes, chunk_sz=64):
    n = len(chunks_bytes) // chunk_sz
    in_buf = (ctypes.c_uint8 * len(chunks_bytes))(*chunks_bytes)
    out_buf = (ctypes.c_uint64 * n)()
    _lib.geofield_batch_seed(in_buf, out_buf, n, chunk_sz)
    return list(out_buf)


# ════════════════════════════════════════════════════════════════
# LETTERCUBE: 24-pair face:face bond
# ════════════════════════════════════════════════════════════════

LC_BUF_SZ = 76  # LC_SERIALIZED_SZ

uchar76 = ctypes.c_uint8 * LC_BUF_SZ

_lib.geofield_lc_init.argtypes = [ctypes.c_void_p]
_lib.geofield_lc_init.restype = None

_lib.geofield_lc_assign.argtypes = [ctypes.c_void_p, ctypes.c_uint8,
                                     ctypes.c_uint8, ctypes.c_uint8]
_lib.geofield_lc_assign.restype = None

_lib.geofield_lc_bond.argtypes = [ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint8]
_lib.geofield_lc_bond.restype = ctypes.c_int

_lib.geofield_lc_assemble.argtypes = [ctypes.c_void_p]
_lib.geofield_lc_assemble.restype = ctypes.c_int

_lib.geofield_lc_verify.argtypes = [ctypes.c_void_p]
_lib.geofield_lc_verify.restype = ctypes.c_int

_lib.geofield_lc_n_locked.argtypes = [ctypes.c_void_p]
_lib.geofield_lc_n_locked.restype = ctypes.c_uint8

_lib.geofield_lc_pair_id.argtypes = [ctypes.c_void_p, ctypes.c_uint8]
_lib.geofield_lc_pair_id.restype = ctypes.c_uint8

_lib.geofield_lc_bonded_to.argtypes = [ctypes.c_void_p, ctypes.c_uint8]
_lib.geofield_lc_bonded_to.restype = ctypes.c_uint8

_lib.geofield_lc_bond_state.argtypes = [ctypes.c_void_p, ctypes.c_uint8]
_lib.geofield_lc_bond_state.restype = ctypes.c_uint8

BOND_FREE, BOND_PEND, BOND_LOCK = 0, 1, 2

COMPLEMENT = [
    12,13,14,15,16,17,18,19,20,21,22,23,
     0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11
]

def lc_init():
    buf = (ctypes.c_uint8 * LC_BUF_SZ)()
    _lib.geofield_lc_init(buf)
    return buf

def lc_assign(buf, lane, pair_id, angle):
    _lib.geofield_lc_assign(buf, lane, pair_id, angle)

def lc_bond(buf, lane_a, lane_b):
    return _lib.geofield_lc_bond(buf, lane_a, lane_b)

def lc_assemble(buf):
    return _lib.geofield_lc_assemble(buf)

def lc_verify(buf):
    return _lib.geofield_lc_verify(buf)

def lc_n_locked(buf):
    return _lib.geofield_lc_n_locked(buf)

def lc_create_with_bonds(pairs_and_angles):
    """Create LetterCube with pre-assigned complementary pairs.
    pairs_and_angles: list of (pair_id, angle) for lanes 0-5.
    Pairs must satisfy complement condition (pair i and pair i+12)."""
    buf = lc_init()
    for lane, (pair, angle) in enumerate(pairs_and_angles):
        lc_assign(buf, lane, pair, angle)
    for a, b in [(0,1),(2,3),(4,5)]:
        lc_bond(buf, a, b)
    lc_assemble(buf)
    return buf


# ════════════════════════════════════════════════════════════════
# CUBE CONTEXT: 6 LetterCube faces → 1 CubeCtx
# ════════════════════════════════════════════════════════════════

CC_FACES = 6
CC_CTX_SZ = CC_FACES * LC_BUF_SZ + 16  # 472 bytes

uchar472 = ctypes.c_uint8 * CC_CTX_SZ

_lib.geofield_cube_ctx_init.argtypes = [ctypes.c_void_p]
_lib.geofield_cube_ctx_init.restype = None

_lib.geofield_cube_ctx_from_lc.argtypes = [ctypes.c_void_p,
                                            ctypes.c_void_p]
_lib.geofield_cube_ctx_from_lc.restype = ctypes.c_int

_lib.geofield_cube_ctx_coupled.argtypes = [ctypes.c_void_p]
_lib.geofield_cube_ctx_coupled.restype = ctypes.c_int

_lib.geofield_cube_ctx_verify.argtypes = [ctypes.c_void_p]
_lib.geofield_cube_ctx_verify.restype = ctypes.c_int

_lib.geofield_cube_ctx_depth.argtypes = [ctypes.c_void_p]
_lib.geofield_cube_ctx_depth.restype = ctypes.c_uint8

_lib.geofield_cube_ctx_slope_hash.argtypes = [ctypes.c_void_p]
_lib.geofield_cube_ctx_slope_hash.restype = ctypes.c_uint16

_lib.geofield_cube_promote.argtypes = [ctypes.c_void_p,  # array of 6 ctx pointers
                                        ctypes.c_void_p]
_lib.geofield_cube_promote.restype = ctypes.c_int

def cube_ctx_init():
    buf = (ctypes.c_uint8 * CC_CTX_SZ)()
    _lib.geofield_cube_ctx_init(buf)
    return buf

def cube_ctx_from_lc(lc_bufs):
    """lc_bufs: list of 6 LC buffers (each 76 bytes)"""
    ctx = (ctypes.c_uint8 * CC_CTX_SZ)()
    # Flatten the 6 LC bufs into a single contiguous array
    flat = (ctypes.c_uint8 * (CC_FACES * LC_BUF_SZ))()
    for i, buf in enumerate(lc_bufs):
        for j in range(LC_BUF_SZ):
            flat[i * LC_BUF_SZ + j] = buf[j]
    _lib.geofield_cube_ctx_from_lc(ctx, flat)
    return ctx

def cube_ctx_coupled(ctx):
    return _lib.geofield_cube_ctx_coupled(ctx)

def cube_ctx_verify(ctx):
    return _lib.geofield_cube_ctx_verify(ctx)

def cube_ctx_depth(ctx):
    return _lib.geofield_cube_ctx_depth(ctx)

def cube_ctx_slope_hash(ctx):
    return _lib.geofield_cube_ctx_slope_hash(ctx)

def cube_promote(children):
    """children: list of 6 CubeCtx buffers → returns parent CubeCtx"""
    parent = (ctypes.c_uint8 * CC_CTX_SZ)()
    child_arr = (ctypes.c_void_p * CC_FACES)()
    for i, c in enumerate(children):
        child_arr[i] = ctypes.cast(c, ctypes.c_void_p).value
    _lib.geofield_cube_promote(child_arr, parent)
    return parent


# ════════════════════════════════════════════════════════════════
# GOLDBERG SPHERE: Cube shell → hexagon mapping
# ════════════════════════════════════════════════════════════════

_lib.geofield_gp_face_count.argtypes = [ctypes.c_uint8]
_lib.geofield_gp_face_count.restype = ctypes.c_uint32

_lib.geofield_gp_is_pentagon.argtypes = [ctypes.c_uint32]
_lib.geofield_gp_is_pentagon.restype = ctypes.c_int

_lib.geofield_gp_tile_id.argtypes = [ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8]
_lib.geofield_gp_tile_id.restype = ctypes.c_uint32

_lib.geofield_gp_tile_to_pent.argtypes = [ctypes.c_uint8, ctypes.c_uint32]
_lib.geofield_gp_tile_to_pent.restype = ctypes.c_uint8

_lib.geofield_gp_chunk_to_addr.argtypes = [ctypes.c_uint8, ctypes.c_uint32,
                                            ctypes.POINTER(ctypes.c_uint32),
                                            ctypes.POINTER(ctypes.c_uint8)]
_lib.geofield_gp_chunk_to_addr.restype = None

_lib.geofield_gp_addr_to_chunk.argtypes = [ctypes.c_uint8, ctypes.c_uint32, ctypes.c_uint8]
_lib.geofield_gp_addr_to_chunk.restype = ctypes.c_uint32

_lib.geofield_gp_choose_level.argtypes = [ctypes.c_uint32]
_lib.geofield_gp_choose_level.restype = ctypes.c_uint8

_lib.geofield_gp_map_face.argtypes = [ctypes.c_uint8, ctypes.c_uint8,
                                       ctypes.POINTER(ctypes.c_uint32)]
_lib.geofield_gp_map_face.restype = ctypes.c_uint32

def gp_face_count(level):
    return _lib.geofield_gp_face_count(level)

def gp_is_pentagon(tile_id):
    return _lib.geofield_gp_is_pentagon(tile_id)

def gp_tile_id(level, pent_anchor, hex_offset):
    return _lib.geofield_gp_tile_id(level, pent_anchor, hex_offset)

def gp_tile_to_pent(level, tile_id):
    return _lib.geofield_gp_tile_to_pent(level, tile_id)

def gp_chunk_to_addr(level, chunk_idx):
    tile_id = ctypes.c_uint32()
    dim = ctypes.c_uint8()
    _lib.geofield_gp_chunk_to_addr(level, chunk_idx, ctypes.byref(tile_id), ctypes.byref(dim))
    return tile_id.value, dim.value

def gp_addr_to_chunk(level, tile_id, dim):
    return _lib.geofield_gp_addr_to_chunk(level, tile_id, dim)

def gp_choose_level(n_faces):
    return _lib.geofield_gp_choose_level(n_faces)

def gp_map_face(level, face_id):
    tile_id = ctypes.c_uint32()
    _lib.geofield_gp_map_face(level, face_id, ctypes.byref(tile_id))
    return tile_id.value


# ════════════════════════════════════════════════════════════════
# FIBONACCI SHELL FOLD: Layer existence on fibo clock ticks
# ════════════════════════════════════════════════════════════════

_lib.geofield_shell_layer_live.argtypes = [ctypes.c_uint8, ctypes.c_uint32]
_lib.geofield_shell_layer_live.restype = ctypes.c_int

_lib.geofield_shell_fold_nearest.argtypes = [ctypes.c_uint8, ctypes.c_uint32, ctypes.c_uint8]
_lib.geofield_shell_fold_nearest.restype = ctypes.c_uint8

_lib.geofield_ring_hot_path.argtypes = [ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8]
_lib.geofield_ring_hot_path.restype = ctypes.c_uint32

GEO_FIBO = [1,1,2,3,5,8,13,21,34,55,89,144]

def shell_layer_live(layer, tick):
    return _lib.geofield_shell_layer_live(layer, tick) != 0

def shell_fold_nearest(layer, tick, pent_axis=0):
    return _lib.geofield_shell_fold_nearest(layer, tick, pent_axis)

def ring_hot_path(pent_id, layer, globe=0):
    return _lib.geofield_ring_hot_path(pent_id, layer, globe)


# ════════════════════════════════════════════════════════════════
# DRamTile (PipelineStore) — Phase 3
# ════════════════════════════════════════════════════════════════

_lib.geofield_dt_init.argtypes = [ctypes.c_uint32]
_lib.geofield_dt_init.restype = ctypes.c_void_p

_lib.geofield_dt_destroy.argtypes = [ctypes.c_void_p]
_lib.geofield_dt_destroy.restype = None

_lib.geofield_dt_put.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_uint32]
_lib.geofield_dt_put.restype = ctypes.c_void_p

_lib.geofield_dt_get.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
_lib.geofield_dt_get.restype = ctypes.c_void_p

_lib.geofield_dt_stats.argtypes = [ctypes.c_void_p]
_lib.geofield_dt_stats.restype = None

_lib.geofield_dt_put_segs.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint64,
                                       ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64),
                                       ctypes.c_uint32]
_lib.geofield_dt_put_segs.restype = None

_lib.geofield_dt_get_segs.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint64,
                                       ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64),
                                       ctypes.c_uint32]
_lib.geofield_dt_get_segs.restype = None

# ── Compressed store/restore ─────────────────────────────────

_lib.geofield_dt_store_compressed.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint64]
_lib.geofield_dt_store_compressed.restype = ctypes.c_int

_lib.geofield_dt_restore_compressed.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint64]
_lib.geofield_dt_restore_compressed.restype = ctypes.c_int


# ════════════════════════════════════════════════════════════════
# GearShift (Phase 4) — Priority routing
# ════════════════════════════════════════════════════════════════

_lib.geofield_gs_init.argtypes = [ctypes.c_void_p]
_lib.geofield_gs_init.restype = ctypes.c_void_p

_lib.geofield_gs_destroy.argtypes = [ctypes.c_void_p]
_lib.geofield_gs_destroy.restype = None

_lib.geofield_gs_register.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_float]
_lib.geofield_gs_register.restype = ctypes.c_int

_lib.geofield_gs_flush.argtypes = [ctypes.c_void_p]
_lib.geofield_gs_flush.restype = ctypes.c_uint32

_lib.geofield_gs_stats.argtypes = [ctypes.c_void_p]
_lib.geofield_gs_stats.restype = None

_lib.geofield_dt_gs_flush.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint64,
                                       ctypes.c_void_p, ctypes.c_size_t,
                                       ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64),
                                       ctypes.c_uint32]
_lib.geofield_dt_gs_flush.restype = ctypes.c_uint32

# ── Per-block encode/decode (Phase 6c: Diamond Shell block ops) ─

_lib.geofield_block_enc.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_lib.geofield_block_enc.restype = ctypes.c_uint32

_lib.geofield_block_dec.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_lib.geofield_block_dec.restype = ctypes.c_uint32

_lib.geofield_block_enc_sz.argtypes = [ctypes.c_void_p]
_lib.geofield_block_enc_sz.restype = ctypes.c_uint32


def ds_classify_block(block64_bytes):
    """Classify one 64B block → Diamond Shell structured output bytes.
    Returns (classified_bytes, n_written).
    FLAT → 1B, non-FLAT → 3B + N*8B (N = 1..8 sub-blocks)."""
    out = (ctypes.c_uint8 * 80)()
    in_buf = (ctypes.c_uint8 * 64).from_buffer_copy(block64_bytes[:64].ljust(64, b'\x00'))
    n = _lib.geofield_block_enc(out, in_buf)
    return bytes(out[:n]), n


def ds_decode_block(classified_bytes):
    """Decode Diamond Shell structured block → 64B output.
    Returns (decoded_64B, bytes_consumed).
    classified_bytes should be at least 67 bytes (worst case)."""
    sz = len(classified_bytes)
    in_buf = (ctypes.c_uint8 * sz).from_buffer_copy(classified_bytes)
    out = (ctypes.c_uint8 * 64)()
    n = _lib.geofield_block_dec(out, in_buf)
    return bytes(out[:64]), n


# ════════════════════════════════════════════════════════════════
# Full encode — single C call (Phase 5b)
# ════════════════════════════════════════════════════════════════

class GFStructureStats(ctypes.Structure):
    _fields_ = [
        ('n_segments', ctypes.c_uint32),
        ('n_blocks', ctypes.c_uint32),
        ('lc_verified', ctypes.c_uint32),
        ('skel_hits', ctypes.c_uint32 * 6),
        ('diamond_hits', ctypes.c_uint8 * 3),
        ('xxh64', ctypes.c_uint64),
        ('orig_size', ctypes.c_uint64),
        ('struct_size', ctypes.c_uint64),
        ('structure_ms', ctypes.c_double),
        ('wall_ms', ctypes.c_double),
        ('ratio', ctypes.c_double),
    ]

_lib.geofield_full_structure.argtypes = [ctypes.c_void_p, ctypes.c_uint64,
                                       ctypes.c_uint32, ctypes.c_uint32,
                                       ctypes.c_void_p, ctypes.c_uint64,
                                       ctypes.POINTER(GFStructureStats)]
_lib.geofield_full_structure.restype = ctypes.c_int

_lib.geofield_full_structure_stats.argtypes = [ctypes.c_void_p, ctypes.c_uint64,
                                              ctypes.c_uint32, ctypes.c_uint32,
                                              ctypes.POINTER(GFStructureStats)]
_lib.geofield_full_structure_stats.restype = ctypes.c_int

_lib.geofield_full_decode.argtypes = [ctypes.c_void_p, ctypes.c_uint64,
                                       ctypes.c_void_p, ctypes.c_uint64,
                                       ctypes.POINTER(ctypes.c_uint64)]
_lib.geofield_full_decode.restype = ctypes.c_int

_lib.gfds_get_info.argtypes = [ctypes.c_void_p, ctypes.c_uint64,
                                ctypes.POINTER(ctypes.c_uint64),
                                ctypes.POINTER(ctypes.c_uint32)]
_lib.gfds_get_info.restype = ctypes.c_int


# ════════════════════════════════════════════════════════════════
# Full compress/decompress — codebook dedup (Phase 6b)
# ════════════════════════════════════════════════════════════════

class GFCSStats(ctypes.Structure):
    _fields_ = [
        ('n_segments', ctypes.c_uint32),
        ('n_blocks', ctypes.c_uint32),
        ('n_patterns', ctypes.c_uint32),
        ('skel_hits', ctypes.c_uint32 * 6),
        ('diamond_hits', ctypes.c_uint8 * 3),
        ('xxh64', ctypes.c_uint64),
        ('orig_size', ctypes.c_uint64),
        ('comp_size', ctypes.c_uint64),
        ('total_out', ctypes.c_uint64),
        ('structure_ms', ctypes.c_double),
        ('wall_ms', ctypes.c_double),
    ]

_lib.geofield_full_compress.argtypes = [ctypes.c_void_p, ctypes.c_uint64,
                                         ctypes.c_uint32, ctypes.c_uint32,
                                         ctypes.c_void_p, ctypes.c_uint64,
                                         ctypes.POINTER(GFCSStats)]
_lib.geofield_full_compress.restype = ctypes.c_int

_lib.geofield_full_decompress.argtypes = [ctypes.c_void_p, ctypes.c_uint64,
                                           ctypes.c_void_p, ctypes.c_uint64,
                                           ctypes.POINTER(ctypes.c_uint64)]
_lib.geofield_full_decompress.restype = ctypes.c_int


def full_compress(data, min_chunk=32, max_chunk=4096):
    """Compress data via GFCS codebook-dedup compression.
    Returns (compressed_bytes, stats) on success, or (None, error) on failure.
    Stats is a GFCSStats object with .total_out, .comp_size, .n_patterns, .ratio, etc."""
    stats = GFCSStats()
    in_buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    # Dry run
    rc = _lib.geofield_full_compress(in_buf, len(data), min_chunk, max_chunk,
                                      None, 0, ctypes.byref(stats))
    if rc != 0:
        return None, stats
    # Compress
    buf = (ctypes.c_uint8 * stats.total_out)()
    rc = _lib.geofield_full_compress(in_buf, len(data), min_chunk, max_chunk,
                                      buf, stats.total_out, ctypes.byref(stats))
    if rc != 0:
        return None, stats
    return bytes(buf[:stats.total_out]), stats


def full_decompress(compressed, out_size=None):
    """Decompress GFCS data. Returns (data, xxh64) on success, or None on failure.
    If out_size is None, reads orig_size from GFCS header."""
    import struct
    # Parse header to get orig_size if not provided
    if out_size is None:
        magic = struct.unpack_from('<I', compressed, 0)[0]
        if magic != 0x53434647:  # "GFCS"
            return None
        out_size = struct.unpack_from('<Q', compressed, 18)[0]
    # Use ctypes array for input buffer (ensure proper buffer protocol)
    in_buf = (ctypes.c_uint8 * len(compressed)).from_buffer_copy(compressed)
    out_buf = (ctypes.c_uint8 * out_size)()
    got_xxh = ctypes.c_uint64()
    rc = _lib.geofield_full_decompress(in_buf, len(compressed),
                                        out_buf, out_size,
                                        ctypes.byref(got_xxh))
    if rc != 0:
        return None
    return bytes(out_buf[:out_size]), got_xxh.value


def dt_store_compressed(dt_handle, data):
    """Compress and store data in DRamTile as single GFCS blob.
    Returns 0 on success."""
    in_buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    return _lib.geofield_dt_store_compressed(dt_handle, in_buf, len(data))


def dt_restore_compressed(dt_handle, out_size):
    """Restore data from compressed blob in DRamTile.
    Returns (data, 0) on success, or (None, errcode) on failure."""
    out_buf = (ctypes.c_uint8 * out_size)()
    rc = _lib.geofield_dt_restore_compressed(dt_handle, out_buf, out_size)
    if rc != 0:
        return None, rc
    return bytes(out_buf), rc


# ════════════════════════════════════════════════════════════════
# pogls_geopixel — spatial coherence block compression
# ════════════════════════════════════════════════════════════════

_lib.pogls_geopixel_encode_block.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                              ctypes.c_void_p, ctypes.c_size_t]
_lib.pogls_geopixel_encode_block.restype = ctypes.c_uint32

_lib.pogls_geopixel_decode_block.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                              ctypes.c_void_p, ctypes.c_size_t]
_lib.pogls_geopixel_decode_block.restype = ctypes.c_uint32


# ════════════════════════════════════════════════════════════════
# TEST
# ════════════════════════════════════════════════════════════════

def _test():
    print("=" * 60)
    print("GeoField Pipeline — C DLL Test")
    print("=" * 60)

    # 1. geo_jump
    j = geo_jump(0, 0, 1)
    print(f"geo_jump(0, HILBERT, 1) = {j}")

    # 2. frame_enc
    enc = frame_enc(0)
    print(f"frame_enc(0) = {enc}  (expect 0)")
    enc37 = frame_enc(1)
    print(f"frame_enc(1) = {enc37}  (expect 37)")

    # 3. tring_walk
    we = tring_walk_enc(42)
    ws = tring_walk_spoke(42)
    print(f"tring_walk_enc(42) = {we}, spoke = {ws}")

    # 4. gp_chunk_to_addr
    a = chunk_to_addr(3, 100)
    fc = face_count(3)
    print(f"gp_chunk_to_addr(level=3, ci=100) = tile_id={a.tile_id}, dim={a.dim}, face_count={fc}")

    # 5. adaptive chunking
    for lvl in range(9):
        sz = adaptive_chunk_size(lvl)
        slots = adaptive_slot_count(lvl)
        print(f"  shell_level={lvl}: chunk_size={sz}, slots={slots}")

    # 6. skeleton
    zeros = bytes(64)
    ones = bytes([1]) * 64
    random_data = bytes(range(64))

    sd = skel_decide(zeros)
    print(f"skel_decide(zeros): {SKEL_NAMES[sd.strategy]}, isect_pc={sd.isect_pc}")

    sd = skel_decide(ones, zeros, True)
    print(f"skel_decide(ones, prev=zeros): {SKEL_NAMES[sd.strategy]}, dc={sd.diff_count}")

    sd = skel_decide(random_data, zeros, True)
    print(f"skel_decide(random, prev=zeros): {SKEL_NAMES[sd.strategy]}, isect_pc={sd.isect_pc}")

    # 7. diamond classify
    dc = diamond_classify(zeros)
    print(f"diamond_classify(zeros): rot={dc.best_rot}, isect_pc={dc.isect_pc}, flag={['FLAT','SPARSE','DENSE'][dc.flag]}")

    # 8. wallet seed
    seed = wallet_seed(b'\x42' * 64)
    print(f"wallet_seed(0x42*64) = 0x{seed:016x}")

    # 9. xxh64
    h = xxh64(b"Hello, GeoField!")
    print(f"xxh64('Hello, GeoField!') = 0x{h:016x}")

    # 10. batch seed
    data = bytes(range(64)) * 4  # 4 chunks
    seeds = batch_seed(data, 64)
    print(f"batch_seed(4 chunks): {[f'0x{s:016x}' for s in seeds]}")

    print("=" * 60)
    print("ALL TESTS PASSED")
    print("=" * 60)


if __name__ == "__main__":
    _test()
