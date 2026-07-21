"""
geo_frame_seek.py — Python port of geo_frame_seek.h + tw_capture_int.h
═══════════════════════════════════════════════════════════════════════
Deterministic frame seek on Fibo 1440 timeline + integer-only triwheel capture.

No float. No trig. No malloc. Pure integer arithmetic.
1 seed + blueprint → everything. O(1) per operation.
"""

from dataclasses import dataclass
from typing import Tuple, List

# ════════════════════════════════════════════════════════════════════
#  HILBERT CURVE — d→xy mapping (from geo_diamond_field_v4.h)
# ════════════════════════════════════════════════════════════════════

def hilbert_d2xy(n: int, d: int) -> Tuple[int, int]:
    """Hilbert distance d → (x,y) on n×n grid. Pure integer. O(log n)."""
    x = y = 0
    s = 1
    t = d
    while s < n:
        rx = 1 & (t // 2)
        ry = 1 & (t ^ rx)
        if ry == 0:
            if rx == 1:
                x = s - 1 - x
                y = s - 1 - y
            x, y = y, x
        x += s * rx
        y += s * ry
        t //= 4
        s *= 2
    return x, y

def hilbert_xy2d(n: int, x: int, y: int) -> int:
    """(x,y) → Hilbert distance d on n×n grid. Pure integer. O(log n)."""
    d = 0
    s = n // 2
    while s > 0:
        rx = 1 if (x & s) else 0
        ry = 1 if (y & s) else 0
        d += s * s * ((3 * rx) ^ ry)
        if ry == 0:
            if rx == 1:
                x = s - 1 - x
                y = s - 1 - y
            x, y = y, x
        s //= 2
    return d

def hilbert_reorder(chunk: bytes, offset: int = 0) -> bytes:
    """
    Hilbert reorder: map 64 bytes → 8×8 grid using Hilbert curve.
    out[y*8 + x] = in[d] where (x,y) = hilbert_d2xy(8, (d + offset*16) % 64)
    This is the CONTAINER mapping — organizes data spatially.
    """
    n = 8
    assert len(chunk) == 64
    out = bytearray(64)
    for d in range(64):
        x, y = hilbert_d2xy(n, (d + offset * 16) % 64)
        out[y * n + x] = chunk[d]
    return bytes(out)

def hilbert_unreorder(chunk: bytes, offset: int = 0) -> bytes:
    """Inverse of hilbert_reorder: grid → Hilbert order."""
    n = 8
    assert len(chunk) == 64
    out = bytearray(64)
    for d in range(64):
        x, y = hilbert_d2xy(n, (d + offset * 16) % 64)
        out[d] = chunk[y * n + x]
    return bytes(out)

def hilbert_verify() -> int:
    """Verify Hilbert d2xy/xy2d roundtrip. Returns 0 on pass."""
    for n in [2, 4, 8]:
        for d in range(n * n):
            x, y = hilbert_d2xy(n, d)
            d2 = hilbert_xy2d(n, x, y)
            if d2 != d:
                return -1
            if x >= n or y >= n:
                return -2
    # Verify reorder/unreorder roundtrip
    import os
    for _ in range(100):
        chunk = os.urandom(64)
        for offset in range(16):
            reordered = hilbert_reorder(chunk, offset)
            unreordered = hilbert_unreorder(reordered, offset)
            if unreordered != chunk:
                return -3
    return 0

# ════════════════════════════════════════════════════════════════════
#  L-BLOCK — deterministic rotation from Hilbert position
# ════════════════════════════════════════════════════════════════════
#
# L-block: 4 cells in L-shape, rotation determined by Hilbert direction.
# "Summon" at any Hilbert address → deterministic rotation → align to grid.
#
# Shape (rotation 0, horizontal L):
#   X X X
#       X
#
# 4 rotations: 0°, 90°, 180°, 270° (determined by Hilbert traversal direction)

def hilbert_direction(d: int, n: int = 8) -> Tuple[int, int]:
    """Get traversal direction at Hilbert position d.
    Returns (dx, dy) — one of (1,0), (0,1), (-1,0), (0,-1).
    Direction = how curve enters position d from d-1.
    """
    d_prev = (d - 1) % (n * n)
    x_prev, y_prev = hilbert_d2xy(n, d_prev)
    x, y = hilbert_d2xy(n, d)
    dx = x - x_prev
    dy = y - y_prev
    # Normalize wrapping (grid edges)
    if dx > 1: dx = -1
    if dx < -1: dx = 1
    if dy > 1: dy = -1
    if dy < -1: dy = 1
    return dx, dy

def direction_to_rotation(dx: int, dy: int) -> int:
    """Map traversal direction to L-block rotation (0-3).
    right→0, down→1, left→2, up→3
    """
    if dx == 1 and dy == 0: return 0   # right
    if dx == 0 and dy == 1: return 1   # down
    if dx == -1 and dy == 0: return 2  # left
    if dx == 0 and dy == -1: return 3  # up
    return 0  # fallback (should not happen)

def lblock_shape(x: int, y: int, rot: int) -> List[Tuple[int, int]]:
    """Generate L-block cells at position (x,y) with rotation rot.
    Returns 4 (x,y) coordinates.

    Base shape (rot=0, right-pointing L):
        (0,0) (1,0) (2,0)
                        (2,1)
    """
    # Base L-shape: horizontal L pointing right
    base = [(0, 0), (1, 0), (2, 0), (2, 1)]

    if rot == 0:    # right → default
        return [(x + bx, y + by) for bx, by in base]
    elif rot == 1:  # down → 90° CW
        return [(x - by, y + bx) for bx, by in base]
    elif rot == 2:  # left → 180°
        return [(x - bx, y - by) for bx, by in base]
    elif rot == 3:  # up → 270° CW
        return [(x + by, y - bx) for bx, by in base]
    return [(x + bx, y + by) for bx, by in base]

def lblock_from_hilbert(d: int, n: int = 8) -> Tuple[List[Tuple[int, int]], int, Tuple[int, int]]:
    """Generate L-block from Hilbert position d on n×n grid.

    Returns: (cells, rotation, direction)
      cells: 4 (x,y) grid coordinates
      rotation: 0-3
      direction: (dx, dy) traversal direction
    """
    x, y = hilbert_d2xy(n, d)
    dx, dy = hilbert_direction(d, n)
    rot = direction_to_rotation(dx, dy)
    cells = lblock_shape(x, y, rot)
    return cells, rot, (dx, dy)

def lblock_verify() -> int:
    """Verify L-block properties. Returns 0 on pass."""
    n = 8

    # T0: direction_to_rotation maps all 4 directions
    dirs = [(1, 0), (0, 1), (-1, 0), (0, -1)]
    rots = [direction_to_rotation(dx, dy) for dx, dy in dirs]
    if len(set(rots)) != 4:
        return -1  # not all 4 rotations covered

    # T1: each rotation produces 4 unique cells
    for rot in range(4):
        cells = lblock_shape(4, 4, rot)
        if len(cells) != 4:
            return -2
        if len(set(cells)) != 4:
            return -3  # duplicate cells

    # T2: lblock_from_hilbert returns valid cells for all positions
    for d in range(n * n):
        cells, rot, (dx, dy) = lblock_from_hilbert(d, n)
        if rot not in range(4):
            return -4
        if len(cells) != 4:
            return -5
        for cx, cy in cells:
            if not isinstance(cx, int) or not isinstance(cy, int):
                return -6  # non-integer coordinates

    # T3: same Hilbert position always gives same rotation (deterministic)
    for d in range(n * n):
        _, rot1, _ = lblock_from_hilbert(d, n)
        _, rot2, _ = lblock_from_hilbert(d, n)
        if rot1 != rot2:
            return -7

    # T4: rotation distribution — should be roughly uniform
    rot_count = [0, 0, 0, 0]
    for d in range(n * n):
        _, rot, _ = lblock_from_hilbert(d, n)
        rot_count[rot] += 1
    for rc in rot_count:
        if rc < 10:  # each rotation should appear at least 10 times in 64 positions
            return -8

    # T5: L-block cells are on grid (0..n-1) — check subset
    for d in range(0, n * n, 7):  # sample every 7th
        cells, _, _ = lblock_from_hilbert(d, n)
        for cx, cy in cells:
            if cx < -n or cx >= 2 * n or cy < -n or cy >= 2 * n:
                return -9  # cells too far from grid (allow some margin for rotation)

    return 0

# ════════════════════════════════════════════════════════════════════
#  CONSTANTS (FROZEN — matching C headers)
# ════════════════════════════════════════════════════════════════════

FRAME_CYCLE    = 1440    # fibo timeline length
FRAME_STRIDE   = 37      # prime walk, gcd(37,1440)=1
FRAME_FACE_SZ  = 120     # slots per face (1440/12)
FRAME_EDGES    = 12      # edges per frame (9H + 3P)
FRAME_H_ACTIVE = 9       # Hilbert active edges
FRAME_P_STEPS  = 4       # Peano steps on line-12
FRAME_ICO_NODES = 162    # icosphere L2 (81×2)
FRAME_PEANO_GRID = 81    # 3^4 ternary space

TW_SCALE       = 207360  # 12^4 × 10
TW_N_SECTORS   = 10
TW_SLOTS_PER   = 6
TW_COMBINED_SLOTS = 12  # 6 hex + 6 tri
TW_MARGIN_NUM  = 9
TW_MARGIN_DEN  = 1000

# ════════════════════════════════════════════════════════════════════
#  BOUNDARY DIRECTION VECTORS (from tw_capture_int.h)
# ════════════════════════════════════════════════════════════════════

TW_BOUNDARY_DIR = [
    [      0, 207360],
    [ 121883, 167758],
    [ 197211,  64078],
    [ 197211, -64078],
    [ 121883,-167758],
    [      0,-207360],
    [-121883,-167758],
    [-197211, -64078],
    [-197211,  64078],
    [-121883, 167758],
]

# Combined grid: 10 sectors × 12 slots (6 hex + 6 tri)
TW_COMBINED_GRID = [
  [[      0, 238464],[ -26937, 222912],[ -26937, 191808],[      0, 176256],[  26937, 191808],[  26937, 222912],[ 119232, 206517],[  88127, 206517],[  72575, 179580],[  88128, 152643],[ 119232, 152643],[ 134784, 179580]],
  [[ 140166, 192922],[ 109232, 196172],[  90949, 171009],[ 103601, 142594],[ 134534, 139342],[ 152817, 164507],[ 217848,  96993],[ 192684, 115274],[ 164269, 102624],[ 161018,  71690],[ 186181,  53407],[ 214597,  66059]],
  [[ 226792,  73689],[ 203678,  94502],[ 174097,  84890],[ 167629,  54466],[ 190744,  33653],[ 220326,  43265],[ 233253, -49580],[ 223642, -19998],[ 193218, -13532],[ 172404, -36646],[ 182016, -66228],[ 212441, -72695]],
  [[ 226792, -73689],[ 220326, -43265],[ 190744, -33653],[ 167629, -54466],[ 174097, -84890],[ 203678, -94502],[ 159564,-177213],[ 169176,-147632],[ 148363,-124517],[ 117938,-130984],[ 108328,-160566],[ 129140,-183681]],
  [[ 140166,-192922],[ 152817,-164507],[ 134534,-139342],[ 103601,-142594],[  90949,-171009],[ 109232,-196172],[  24926,-237160],[  50090,-218877],[  46839,-187942],[  18424,-175292],[  -6740,-193574],[  -3488,-224507]],
  [[      0,-238464],[  26937,-222912],[  26937,-191808],[      0,-176256],[ -26937,-191808],[ -26937,-222912],[-119232,-206517],[ -88128,-206517],[ -72576,-179580],[ -88128,-152643],[-119233,-152643],[-134785,-179580]],
  [[-140166,-192922],[-109232,-196172],[ -90949,-171009],[-103601,-142594],[-134534,-139342],[-152817,-164507],[-217849, -96994],[-192685,-115275],[-164270,-102625],[-161019, -71691],[-186182, -53408],[-214598, -66060]],
  [[-226792, -73689],[-203678, -94502],[-174097, -84890],[-167629, -54466],[-190744, -33653],[-220326, -43265],[-233254,  49579],[-223643,  19997],[-193219,  13531],[-172405,  36645],[-182017,  66227],[-212442,  72694]],
  [[-226792,  73689],[-220326,  43265],[-190744,  33653],[-167629,  54466],[-174097,  84890],[-203678,  94502],[-159565, 177212],[-169177, 147631],[-148364, 124516],[-117939, 130983],[-108329, 160565],[-129141, 183680]],
  [[-140166, 192922],[-152817, 164507],[-134534, 139342],[-103601, 142594],[ -90949, 171009],[-109232, 196172],[ -24927, 237159],[ -50091, 218876],[ -46840, 187941],[ -18425, 175291],[   6739, 193573],[   3487, 224506]],
]

# ════════════════════════════════════════════════════════════════════
#  DUALFRAME — one complete 12-edge unit
# ════════════════════════════════════════════════════════════════════

@dataclass
class DualFrame:
    enc:      int   # 0..1439
    face:     int   # 0..11 dodecahedron face
    slot:     int   # 0..119 slot within face
    ico_idx:  int   # 0..161 icosphere address
    phase:    int   # iteration phase
    h_group:  int   # Hilbert group 0..2
    h_edge:   int   # Hilbert edge 0..2
    h_is_skip: int  # 1 = invert point
    p_step:   int   # Peano step 0..3
    p_sub:    int   # Peano sub 0..2

# ════════════════════════════════════════════════════════════════════
#  GEO FRAME SEEK — O(1) deterministic frame decomposition
# ════════════════════════════════════════════════════════════════════

def frame_enc(t: int) -> int:
    """enc at time t: stride-37 walk on 1440 cycle"""
    return (t * FRAME_STRIDE) % FRAME_CYCLE

def frame_at(enc: int) -> DualFrame:
    """Decompose enc (0..1439) into full DualFrame. O(1)."""
    face = enc // FRAME_FACE_SZ          # 0..11
    slot = enc % FRAME_FACE_SZ           # 0..119

    h_group  = face % 3
    h_edge   = enc % 3
    h_is_skip = 1 if (enc % FRAME_EDGES) >= FRAME_H_ACTIVE else 0

    p_step = (enc // 3) % FRAME_P_STEPS
    p_sub  = enc % 3
    ico_idx = enc % FRAME_ICO_NODES
    phase   = (enc // FRAME_EDGES) % 12

    return DualFrame(
        enc=enc, face=face, slot=slot,
        ico_idx=ico_idx, phase=phase,
        h_group=h_group, h_edge=h_edge, h_is_skip=h_is_skip,
        p_step=p_step, p_sub=p_sub,
    )

def frame_seek(t: int) -> DualFrame:
    """Seek to time t → DualFrame. O(1)."""
    return frame_at(frame_enc(t))

def frame_next(enc: int) -> int:
    """Next enc in walk"""
    return (enc + FRAME_STRIDE) % FRAME_CYCLE

def frame_prev(enc: int) -> int:
    """Prev enc in walk"""
    return (enc + FRAME_CYCLE - FRAME_STRIDE) % FRAME_CYCLE

def frame_cpair(enc: int) -> int:
    """Diameter flip (north↔south pole)"""
    return (enc + FRAME_CYCLE // 2) % FRAME_CYCLE

# ════════════════════════════════════════════════════════════════════
#  VERIFY — call once at init
# ════════════════════════════════════════════════════════════════════

def geo_frame_seek_verify() -> int:
    """Verify invariants. Returns 0 on pass."""
    # T0: Hilbert verify
    rc = hilbert_verify()
    if rc != 0:
        return rc - 100

    # T0b: L-block verify
    rc = lblock_verify()
    if rc != 0:
        return rc - 200

    # T1: stride-37 full cycle
    visited = set()
    e = 0
    for _ in range(FRAME_CYCLE):
        if e in visited:
            return -1
        visited.add(e)
        e = frame_next(e)
    if e != 0:
        return -2

    # T2: frame_enc / frame_at roundtrip
    for t in range(FRAME_CYCLE):
        f = frame_seek(t)
        if f.enc != frame_enc(t):
            return -3
        if f.face > 11:
            return -4
        if f.h_group > 2:
            return -5
        if f.h_edge > 2:
            return -6
        if f.p_step >= FRAME_P_STEPS:
            return -7
        if f.p_sub > 2:
            return -8
        if f.ico_idx >= FRAME_ICO_NODES:
            return -9

    # T3: cpair self-inverse
    for enc in range(FRAME_CYCLE):
        if frame_cpair(frame_cpair(enc)) != enc:
            return -10

    # T4: prev(next(enc)) == enc
    for enc in range(FRAME_CYCLE):
        if frame_prev(frame_next(enc)) != enc:
            return -11

    # T5: 9+3=12 edges
    skip_count = 0
    active_count = 0
    for enc in range(FRAME_EDGES):
        f = frame_at(enc)
        if f.h_is_skip:
            skip_count += 1
        else:
            active_count += 1
    if active_count != FRAME_H_ACTIVE:
        return -12
    if skip_count != 3:
        return -13

    # T6: phase cycles 0..11
    for enc in range(144):
        f = frame_at(enc)
        if f.phase >= 12:
            return -14

    return 0

# ════════════════════════════════════════════════════════════════════
#  TW CAPTURE — Integer-only hierarchical sparse triwheel
# ════════════════════════════════════════════════════════════════════

@dataclass
class TWCaptureInt:
    zone: int         # 0..9 primary sector
    slot: int         # 0..59 (primary * 6 + local)
    resid_x: int      # residual from slot centroid
    resid_y: int
    drain: int        # 1 if near boundary
    drain_zone: int
    drain_slot: int
    drain_resid_x: int
    drain_resid_y: int
    is_tri: int       # 0=hex centroid, 1=tri centroid

def _tw_cross(ax: int, ay: int, bx: int, by: int) -> int:
    """Cross product z-component, int64"""
    return ax * by - ay * bx

def _tw_mag2(x: int, y: int) -> int:
    return x * x + y * y

def _tw_abs64(v: int) -> int:
    return v if v >= 0 else -v

def tw_capture_int_combined(vx: int, vy: int) -> TWCaptureInt:
    """
    Combined capture: searches all 12 centroids/sector (hex+tri).
    Python port of tw_capture_int_combined() from tw_capture_int.h.
    """
    # Find primary sector via cross-product sign test
    mincross_abs = -1
    primary = 0
    for k in range(TW_N_SECTORS):
        cross_k = _tw_cross(TW_BOUNDARY_DIR[k][0], TW_BOUNDARY_DIR[k][1], vx, vy)
        cross_kn = _tw_cross(TW_BOUNDARY_DIR[(k+1) % TW_N_SECTORS][0],
                             TW_BOUNDARY_DIR[(k+1) % TW_N_SECTORS][1], vx, vy)
        if cross_k <= 0 and cross_kn >= 0:
            primary = k
        a = _tw_abs64(cross_k)
        if mincross_abs < 0 or a < mincross_abs:
            mincross_abs = a

    # Search 12 centroids (6 hex + 6 tri)
    best = 0
    best_is_tri = 0
    best_dist = -1
    for j in range(TW_COMBINED_SLOTS):
        dx = vx - int(TW_COMBINED_GRID[primary][j][0])
        dy = vy - int(TW_COMBINED_GRID[primary][j][1])
        d = dx * dx + dy * dy
        if best_dist < 0 or d < best_dist:
            best_dist = d
            best = j
            best_is_tri = 1 if j >= TW_SLOTS_PER else 0

    local = best % TW_SLOTS_PER
    slot = primary * TW_COMBINED_SLOTS + best  # 0..119, encodes full combined index
    resid_x = vx - TW_COMBINED_GRID[primary][best][0]
    resid_y = vy - TW_COMBINED_GRID[primary][best][1]

    # Drain test: |cross|^2 * DEN^2  vs  vmag2 * SCALE^2 * NUM^2
    vmag2 = _tw_mag2(vx, vy)
    lhs = mincross_abs * mincross_abs * TW_MARGIN_DEN * TW_MARGIN_DEN
    rhs = vmag2 * TW_SCALE * TW_SCALE * TW_MARGIN_NUM * TW_MARGIN_NUM

    drain = 0
    drain_zone = 0
    drain_slot = 0
    drain_resid_x = 0
    drain_resid_y = 0

    if lhs < rhs:
        # Near boundary — find neighbor sector
        cross_prev = _tw_cross(TW_BOUNDARY_DIR[primary][0], TW_BOUNDARY_DIR[primary][1], vx, vy)
        cross_next = _tw_cross(TW_BOUNDARY_DIR[(primary+1) % TW_N_SECTORS][0],
                               TW_BOUNDARY_DIR[(primary+1) % TW_N_SECTORS][1], vx, vy)
        if _tw_abs64(cross_prev) < _tw_abs64(cross_next):
            secondary = (primary - 1 + TW_N_SECTORS) % TW_N_SECTORS
        else:
            secondary = (primary + 1) % TW_N_SECTORS

        drain = 1
        drain_zone = secondary
        # Search combined grid in secondary sector
        d_best = 0
        d_bd = -1
        for j in range(TW_COMBINED_SLOTS):
            dx = vx - TW_COMBINED_GRID[secondary][j][0]
            dy = vy - TW_COMBINED_GRID[secondary][j][1]
            d = dx * dx + dy * dy
            if d_bd < 0 or d < d_bd:
                d_bd = d
                d_best = j
        drain_slot = secondary * TW_COMBINED_SLOTS + d_best  # full combined index
        drain_resid_x = vx - TW_COMBINED_GRID[secondary][d_best][0]
        drain_resid_y = vy - TW_COMBINED_GRID[secondary][d_best][1]

    return TWCaptureInt(
        zone=primary, slot=slot,
        resid_x=resid_x, resid_y=resid_y,
        drain=drain, drain_zone=drain_zone, drain_slot=drain_slot,
        drain_resid_x=drain_resid_x, drain_resid_y=drain_resid_y,
        is_tri=best_is_tri,
    )

# ════════════════════════════════════════════════════════════════════
#  RECONSTRUCT — lossless exact integer
# ════════════════════════════════════════════════════════════════════

def tw_reconstruct_int_combined(cap: TWCaptureInt) -> Tuple[int, int]:
    """Reconstruct (vx, vy) from capture. Lossless."""
    sector = cap.zone
    combined_idx = cap.slot - sector * TW_COMBINED_SLOTS  # 0..11
    vx = TW_COMBINED_GRID[sector][combined_idx][0] + cap.resid_x
    vy = TW_COMBINED_GRID[sector][combined_idx][1] + cap.resid_y
    return vx, vy

# ════════════════════════════════════════════════════════════════════
#  PIPELINE: tensor → capture → frame permute → delta
# ════════════════════════════════════════════════════════════════════

def tensor_to_captures(data_bytes: bytes) -> list:
    """
    Convert raw bytes to list of TWCaptureInt.
    Each pair of bytes → (vx, vy) → capture.
    data_bytes: raw bytes from tensor
    """
    n_pairs = len(data_bytes) // 2
    captures = []
    scale = TW_SCALE // 128
    for i in range(n_pairs):
        b0 = data_bytes[2*i]
        b1 = data_bytes[2*i + 1]
        # Sign-extend uint8 to int
        vx = (b0 if b0 < 128 else b0 - 256) * scale
        vy = (b1 if b1 < 128 else b1 - 256) * scale
        cap = tw_capture_int_combined(vx, vy)
        captures.append(cap)
    return captures


def permute_by_frame(captures: list, n_frames: int) -> list:
    """
    Permute captures by frame_seek ordering.
    Each frame gets FRAME_EDGES=12 captures.
    Order within frame determined by DualFrame.slot.
    """
    CHUNK_PER_FRAME = FRAME_EDGES  # 12
    total_needed = n_frames * CHUNK_PER_FRAME
    # Pad captures if needed
    while len(captures) < total_needed:
        captures.append(TWCaptureInt(zone=0, slot=0, resid_x=0, resid_y=0,
                                     drain=0, drain_zone=0, drain_slot=0,
                                     drain_resid_x=0, drain_resid_y=0, is_tri=0))

    permuted = []
    for fi in range(n_frames):
        df = frame_seek(fi)
        base = fi * CHUNK_PER_FRAME
        # Order chunks within frame by slot mapping
        for ci in range(CHUNK_PER_FRAME):
            src_idx = base + (ci + df.slot) % CHUNK_PER_FRAME
            permuted.append(captures[src_idx])
    return permuted


def delta_encode_frames(captures: list, n_frames: int) -> list:
    """
    Delta encode between adjacent frames.
    Frame 0 is seed (stored raw).
    Frame i>0: store (capture - prediction) where prediction = seed via frame_at.
    """
    CHUNK_PER_FRAME = FRAME_EDGES
    deltas = []

    # Frame 0 = seed (raw)
    seed = captures[:CHUNK_PER_FRAME]
    deltas.append(('seed', seed))

    # Frame i>0: delta from seed via slot mapping
    for fi in range(1, n_frames):
        df = frame_seek(fi)
        frame_caps = captures[fi * CHUNK_PER_FRAME : (fi+1) * CHUNK_PER_FRAME]
        deltas_frame = []
        for ci in range(CHUNK_PER_FRAME):
            seed_ci = (ci + df.slot) % CHUNK_PER_FRAME
            seed_cap = seed[seed_ci]
            cur_cap = frame_caps[ci]
            # Delta = current - seed prediction
            dx = cur_cap.resid_x - seed_cap.resid_x
            dy = cur_cap.resid_y - seed_cap.resid_y
            is_same = (cur_cap.zone == seed_cap.zone and
                       cur_cap.slot == seed_cap.slot and dx == 0 and dy == 0)
            deltas_frame.append({
                'is_same': is_same,
                'zone': cur_cap.zone,
                'slot': cur_cap.slot,
                'resid_x': dx,
                'resid_y': dy,
            })
        deltas.append(('delta', deltas_frame))

    return deltas


# ════════════════════════════════════════════════════════════════════
#  FULL PIPELINE ENTRY POINT
# ════════════════════════════════════════════════════════════════════

def pipeline_geo_seek(data_bytes: bytes, max_frames: int = 100) -> dict:
    """
    Full pipeline: bytes → tw_capture → frame permute → delta encode.
    Returns metadata + deltas.
    """
    # Verify frame seek invariants
    rc = geo_frame_seek_verify()
    if rc != 0:
        raise RuntimeError(f"geo_frame_seek verify failed: {rc}")

    n_pairs = len(data_bytes) // 2
    n_frames = min(max_frames, (n_pairs + FRAME_EDGES - 1) // FRAME_EDGES)

    print(f"  geo_seek: {len(data_bytes)} bytes → {n_pairs} pairs → {n_frames} frames")

    # Step 1: bytes → captures
    captures = tensor_to_captures(data_bytes)

    # Step 2: permute by frame
    permuted = permute_by_frame(captures, n_frames)

    # Step 3: delta encode
    deltas = delta_encode_frames(permuted, n_frames)

    # Stats
    same_count = 0
    delta_frames = 0
    for dtype, frames in deltas:
        if dtype == 'delta':
            delta_frames += 1
            for d in frames:
                if d.get('is_same', False):
                    same_count += 1

    return {
        'n_frames': n_frames,
        'seed_count': FRAME_EDGES,
        'delta_count': delta_frames * FRAME_EDGES,
        'same_count': same_count,
        'deltas': deltas,
    }


# ════════════════════════════════════════════════════════════════════
#  BENCHMARK — throughput
# ════════════════════════════════════════════════════════════════════

def bench_geo_seek(n_bytes: int = 76800) -> dict:
    """Benchmark geo_frame_seek + tw_capture throughput."""
    import time
    import os

    # Generate random test data
    data_bytes = os.urandom(n_bytes)

    # Verify
    rc = geo_frame_seek_verify()
    assert rc == 0, f"verify failed: {rc}"

    # Benchmark tw_capture
    t0 = time.perf_counter()
    captures = tensor_to_captures(data_bytes)
    t_capture = time.perf_counter() - t0

    # Benchmark frame_seek
    n_frames = len(captures) // FRAME_EDGES
    t0 = time.perf_counter()
    for fi in range(min(n_frames, 1000)):
        _ = frame_seek(fi)
    t_seek = time.perf_counter() - t0

    # Benchmark delta encode
    permuted = permute_by_frame(captures, min(n_frames, 1000))
    t0 = time.perf_counter()
    deltas = delta_encode_frames(permuted, min(n_frames, 1000))
    t_delta = time.perf_counter() - t0

    return {
        'n_bytes': n_bytes,
        'n_pairs': len(captures),
        'n_frames': n_frames,
        'capture_us_per_pair': t_capture / len(captures) * 1e6,
        'seek_us_per_frame': t_seek / min(n_frames, 1000) * 1e6,
        'delta_us_per_frame': t_delta / min(n_frames, 1000) * 1e6,
    }


if __name__ == "__main__":
    print("=== geo_frame_seek verify ===")
    rc = geo_frame_seek_verify()
    print(f"  verify: {'PASS' if rc == 0 else f'FAIL ({rc})'}")

    print("\n=== L-block samples ===")
    for d in [0, 1, 5, 10, 21, 42, 63]:
        cells, rot, (dx, dy) = lblock_from_hilbert(d, 8)
        cells_str = " ".join(f"({x},{y})" for x, y in cells)
        print(f"  d={d:2d}  dir=({dx},{dy})  rot={rot}  cells=[{cells_str}]")

    # Rotation distribution
    rot_count = [0, 0, 0, 0]
    for d in range(64):
        _, rot, _ = lblock_from_hilbert(d, 8)
        rot_count[rot] += 1
    print(f"  rotation distribution: {rot_count}")

    print("\n=== frame_at samples ===")
    for enc in [0, 37, 74, 111, 148, 185, 1439]:
        f = frame_at(enc)
        print(f"  enc={enc:4d}  face={f.face}  slot={f.slot:3d}  "
              f"ico={f.ico_idx:3d}  phase={f.phase:2d}  "
              f"H({f.h_group},{f.h_edge},skip={f.h_is_skip})  "
              f"P({f.p_step},{f.p_sub})")

    print("\n=== benchmark ===")
    result = bench_geo_seek(76800)
    for k, v in result.items():
        if isinstance(v, float):
            print(f"  {k}: {v:.2f}")
        else:
            print(f"  {k}: {v}")
