import ctypes, os
from pathlib import Path
from typing import Optional

MODULE_DIR = Path(__file__).resolve().parent.parent

class GeoJumpType:
    HILBERT  = 0
    PEANO    = 1
    PENTAGON = 2
    MOD      = 3
    INVERT   = 4
    GROUND   = 5
    CAPO     = 6

class GeoJumpRouter(ctypes.Structure):
    _fields_ = [
        ("type",   ctypes.c_int),
        ("param",  ctypes.c_uint32),
        ("param2", ctypes.c_uint32),
        ("param3", ctypes.c_uint32),
    ]

class GeoDna(ctypes.Structure):
    _fields_ = [
        ("head",       ctypes.c_uint32),
        ("tail",       ctypes.c_uint32),
        ("seed_key",   ctypes.c_uint32),
        ("fibo_round", ctypes.c_uint32),
        ("length",     ctypes.c_uint32),
        ("router",     GeoJumpRouter),
    ]

    def at(self, step: int) -> int:
        return geo_dna_at(ctypes.byref(self), step)

    def timeline(self, layer: int) -> int:
        return geo_dna_timeline(ctypes.byref(self), layer)

    def timeline_all(self) -> list:
        out = (ctypes.c_uint32 * 12)()
        geo_dna_timeline_all(ctypes.byref(self), out)
        return list(out)


class GeoRouter:
    METATRON_COLS   = 4
    METATRON_ROWS   = 4
    METATRON_FLOORS = 3
    BLOCK  = METATRON_COLS * METATRON_ROWS * METATRON_FLOORS
    TOWER  = BLOCK * METATRON_FLOORS
    FULL   = TOWER * TOWER

    def __init__(self, lib_path: Optional[str] = None):
        if lib_path:
            path = Path(lib_path)
        else:
            candidates = [
                MODULE_DIR / "libgeojump.dll",
                MODULE_DIR / "libgeojump.so",
                MODULE_DIR / "build" / "libgeojump.dll",
            ]
            path = next((p for p in candidates if p.exists()), None)
            if not path:
                raise FileNotFoundError(
                    f"libgeojump not found. Build with: make -C {MODULE_DIR}"
                )
        self._lib = ctypes.CDLL(str(path))
        self._setup_prototypes()

    def _setup_prototypes(self):
        lib = self._lib
        lib.geo_jump.restype = ctypes.c_uint32
        lib.geo_jump.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
        lib.geo_jump_r.restype = ctypes.c_uint32
        lib.geo_jump_r.argtypes = [ctypes.c_uint32, ctypes.POINTER(GeoJumpRouter)]
        lib.geo_clock_tick.restype = ctypes.c_uint32
        lib.geo_clock_tick.argtypes = [ctypes.c_uint32]
        lib.geo_pentagon_id.restype = ctypes.c_uint32
        lib.geo_pentagon_id.argtypes = [ctypes.c_uint32]
        lib.geo_shell_level.restype = ctypes.c_uint32
        lib.geo_shell_level.argtypes = [ctypes.c_uint32]
        lib.geo_dna_from_walk.argtypes = [ctypes.POINTER(GeoDna), ctypes.c_void_p, ctypes.c_uint32]
        lib.geo_dna_at.restype = ctypes.c_uint32
        lib.geo_dna_at.argtypes = [ctypes.POINTER(GeoDna), ctypes.c_uint32]
        lib.geo_dna_timeline.restype = ctypes.c_uint32
        lib.geo_dna_timeline.argtypes = [ctypes.POINTER(GeoDna), ctypes.c_uint32]
        lib.geo_dna_timeline_all.argtypes = [ctypes.POINTER(GeoDna), ctypes.c_void_p]

    def jump(self, node_id: int, jump_type: int, param: int = 0) -> int:
        return self._lib.geo_jump(node_id, jump_type, param)

    def jump_r(self, node_id: int, router: GeoJumpRouter) -> int:
        return self._lib.geo_jump_r(node_id, ctypes.byref(router))

    def clock_tick(self, node_id: int) -> int:
        return self._lib.geo_clock_tick(node_id)

    def pentagon_id(self, node_id: int) -> int:
        return self._lib.geo_pentagon_id(node_id)

    def shell_level(self, node_id: int) -> int:
        return self._lib.geo_shell_level(node_id)

    def jump_hilbert(self, node_id: int, col: int = 1, row: int = 1, floor: int = 1) -> int:
        r = GeoJumpRouter(GeoJumpType.HILBERT, col, row, floor)
        return self.jump_r(node_id, r)

    def jump_peano(self, node_id: int, col: int = 1, row: int = 1, floor: int = 1) -> int:
        r = GeoJumpRouter(GeoJumpType.PEANO, col, row, floor)
        return self.jump_r(node_id, r)

    def jump_ground(self, node_id: int, col: int = 1, row: int = 1) -> int:
        r = GeoJumpRouter(GeoJumpType.GROUND, col, row, 0)
        return self.jump_r(node_id, r)

    def jump_pentagon(self, node_id: int, layer: int = 0) -> int:
        return self.jump(node_id, GeoJumpType.PENTAGON, layer)

    def jump_pentagon_face(self, node_id: int, pent_id: int, layer: int = 0) -> int:
        r = GeoJumpRouter(GeoJumpType.PENTAGON, layer, pent_id, 0)
        return self.jump_r(node_id, r)

    def jump_mod(self, node_id: int, mult: int = 0) -> int:
        return self.jump(node_id, GeoJumpType.MOD, mult)

    def jump_invert(self, node_id: int, tower_off: int = 0) -> int:
        return self.jump(node_id, GeoJumpType.INVERT, tower_off)

    def jump_capo(self, node_id: int, key: int = 1) -> int:
        return self.jump(node_id, GeoJumpType.CAPO, key)

    def field_climate(self, node_id: int, anchor_id: int = 0) -> dict:
        anchor_id %= 24
        centroid = anchor_id * (self.FULL // 24) + (self.FULL // 48)
        d = abs(centroid - node_id)
        if d < 24:
            zone, climate = "INCIRCLE", "TROPICAL"
        elif d < 144:
            zone, climate = "MIDDLE", "TEMPERATE"
        elif d < 432:
            zone, climate = "BETWEEN", "BOREAL"
        else:
            zone, climate = "OUTSIDE", "TUNDRA"
        return {"node": node_id, "anchor_id": anchor_id,
                "centroid": centroid, "dist": d,
                "zone": zone, "climate": climate}

    def hilbert_cell(self, col: int, row: int) -> int:
        n = self.METATRON_COLS
        if col < 0 or col >= n: return 0
        if row < 0 or row >= self.METATRON_ROWS: return 0
        d = 0
        for s in range(n.bit_length() - 2, -1, -1):
            bit = 1 << s
            rx = 1 if (col & bit) else 0
            ry = 1 if (row & bit) else 0
            d = (d << 2) | ((3 * rx) ^ ry)
            if ry == 0:
                if rx:
                    col = n - 1 - col
                    row = n - 1 - row
                col, row = row, col
        return d

    def dodeca_adj(self, globe: int = 0, face: int = 0, edge: int = 0) -> dict:
        f = face % 12; e = edge % 5
        nf, ne = DODECA_ADJ_A[f][e]
        return {"face": nf, "edge": ne}

    def dodeca_globe_offset(self, globe: int = 0) -> int:
        return 10368 if globe else 0

    def dodeca_face_dist(self, f0: int, f1: int) -> int:
        return DODECA_DIST[f0 % 12][f1 % 12]

    def dodeca_walk(self, start_face: int, start_edge: int, hops: int, globe: int = 0) -> dict:
        f, e = start_face % 12, start_edge % 5
        for _ in range(hops):
            nf, ne = DODECA_ADJ_A[f][e]
            f, e = nf, ne
        return {"face": f, "edge": e}

    def face_flower(self, face: int) -> int:
        return 0 if face < 6 else 1

    def edge_is_boundary(self, face: int, edge: int) -> bool:
        nf, _ = DODECA_ADJ_A[face % 12][edge % 5]
        return self.face_flower(face) != self.face_flower(nf)

    def ring_step(self, face: int, ring: int, edge: int) -> dict:
        nf, _ = DODECA_ADJ_A[face % 12][edge % 5]
        if self.face_flower(nf) == self.face_flower(face):
            return {"face": nf, "ring": ring}
        dodeca = ring % 5
        flower = (ring // 5) % 2
        if flower == 1:
            return {"face": nf, "ring": (dodeca + 1) % 5}
        return {"face": nf, "ring": dodeca + 5}

    def ring_to_hex(self, face: int, vert_slot: int) -> int:
        return DODECA_FACE_VERTS[face % 12][vert_slot % 5]

    def hex_to_face(self, vert: int, face_slot: int) -> int:
        return DODECA_VERT_FACES[vert % 20][face_slot % 3]

    def shell_layer_live(self, layer: int, tick: int) -> bool:
        return (tick % GEO_FIBO[layer % 12] == 0)

    def shell_fold_nearest(self, layer: int, tick: int, pent_axis: int = 0) -> int:
        if pent_axis: return layer
        layer %= 12
        if tick % GEO_FIBO[layer] == 0: return layer
        for delta in range(1, 12):
            lo = layer - delta
            if lo >= 0 and tick % GEO_FIBO[lo] == 0: return lo
            hi = layer + delta
            if hi < 12 and tick % GEO_FIBO[hi] == 0: return hi
        return 0

    def ring_hot_path(self, pent_id: int, layer: int, globe: int = 0) -> int:
        base = (pent_id % 12) * (self.FULL // 12)
        offset = 10368 if globe else 0
        return (base + layer * 144 + offset) % self.FULL


class _ClimateField:
    MAX_ATTRACTORS = 8

    def __init__(self, seed: int = 0, key: int = 0, modality: int = 0, globe: int = 0, tick: int = 0):
        self.seed = seed % GEO_FULL
        self.key = key
        self.modality = modality
        self.globe = globe & 1
        self.tick = tick
        self.centroid = self.seed
        self.attract = []

    def modality_name(self) -> str:
        return {0:"TEXT",1:"AUDIO",2:"IMAGE",3:"VIDEO",4:"GEO"}.get(self.modality, "UNKNOWN")

    def add(self, node: int, weight: int = 128) -> bool:
        if len(self.attract) >= self.MAX_ATTRACTORS: return False
        face_stride = GEO_FULL // 12
        n = node % GEO_FULL
        layer = n // 144 % 12
        pent_id = (n // (GEO_FULL // 12)) % 12
        hot = (n % face_stride) % 144 == 0
        self.attract.append({"node": n, "weight": weight, "layer": layer,
                             "pent_id": pent_id, "flags": 1 if hot else 0})
        tw = sum(a["weight"] for a in self.attract)
        self.centroid = (sum(a["node"] * a["weight"] for a in self.attract) // tw) % GEO_FULL if tw else self.seed
        return True

    def capo(self, new_key: int):
        if new_key == self.key: return
        delta = (new_key - self.key) * 144 % GEO_FULL
        for a in self.attract:
            a["node"] = (a["node"] + delta) % GEO_FULL
        self.centroid = (self.centroid + delta) % GEO_FULL
        self.seed = (self.seed + delta) % GEO_FULL
        self.key = new_key

    def similarity(self, other: '_ClimateField') -> int:
        cnt_score = 128 if self.n_attract() == other.n_attract() else 64
        dist = abs(self.centroid - other.centroid)
        if dist > GEO_FULL // 2: dist = GEO_FULL - dist
        ds = 128 - (dist * 128) // (GEO_FULL // 2)
        return (cnt_score + ds) // 2

    def n_attract(self) -> int:
        return len(self.attract)

    def hot_paths(self) -> list:
        return [a for a in self.attract if a["flags"] & 1]

    def to_dict(self) -> dict:
        return {
            "seed": self.seed, "key": self.key,
            "modality": self.modality, "modality_name": self.modality_name(),
            "globe": self.globe, "tick": self.tick,
            "centroid": self.centroid, "n_attract": self.n_attract(),
            "attractors": self.attract,
        }


GEO_BLOCK       = GeoRouter.BLOCK
GEO_TOWER       = GeoRouter.TOWER
GEO_FULL        = GeoRouter.FULL
GEO_PENTAGONS   = 12
GEO_FIBO_CLOCK  = 1440
GEO_SHELL_TICK  = 12
GEO_MOD_PRIME   = 162

GEO_INCIRCLE = 0
GEO_MIDDLE   = 1
GEO_BETWEEN  = 2
GEO_OUTSIDE  = 3

GEO_CLIMATE_TROPICAL  = 0
GEO_CLIMATE_TEMPERATE = 1
GEO_CLIMATE_BOREAL    = 2
GEO_CLIMATE_TUNDRA    = 3

GEO_FIBO = [1,1,2,3,5,8,13,21,34,55,89,144]

DODECA_FACES = 12
DODECA_EDGES = 5
DODECA_VERTS = 20

DODECA_ADJ_A = [
    [(1,0),(2,2),(4,1),(6,1),(3,2)],
    [(0,0),(2,3),(5,1),(7,1),(3,3)],
    [(8,0),(4,2),(0,1),(1,1),(5,2)],
    [(9,0),(6,2),(0,4),(1,4),(7,2)],
    [(6,0),(0,2),(2,1),(8,1),(10,2)],
    [(7,0),(1,2),(2,4),(8,4),(11,2)],
    [(4,0),(0,3),(3,1),(9,1),(10,3)],
    [(5,0),(1,3),(3,4),(9,4),(11,3)],
    [(2,0),(4,3),(10,1),(11,1),(5,3)],
    [(3,0),(6,3),(10,4),(11,4),(7,3)],
    [(11,0),(8,2),(4,4),(6,4),(9,2)],
    [(10,0),(8,3),(5,4),(7,4),(9,3)],
]

DODECA_DIST = [
    [0,1,1,1,1,2,1,2,2,2,2,3],
    [1,0,1,1,2,1,2,1,2,2,3,2],
    [1,1,0,2,1,1,2,2,1,3,2,2],
    [1,1,2,0,2,2,1,1,3,1,2,2],
    [1,2,1,2,0,2,1,3,1,2,1,2],
    [2,1,1,2,2,0,3,1,1,2,2,1],
    [1,2,2,1,1,3,0,2,2,1,1,2],
    [2,1,2,1,3,1,2,0,2,1,2,1],
    [2,2,1,3,1,1,2,2,0,2,1,1],
    [2,2,3,1,2,2,1,1,2,0,1,1],
    [2,3,2,2,1,2,1,2,1,1,0,1],
    [3,2,2,2,2,1,2,1,1,1,1,0],
]

DODECA_VERT_FACES = [
    [0,1,2],[0,1,3],[0,2,4],[0,3,6],
    [1,2,5],[1,3,7],[0,4,6],[1,5,7],
    [2,4,8],[3,6,9],[2,5,8],[3,7,9],
    [4,6,10],[5,7,11],[4,8,10],[6,9,10],
    [5,8,11],[7,9,11],[8,10,11],[9,10,11],
]

DODECA_FACE_VERTS = [
    [1,0,2,6,3],[1,0,4,7,5],[10,8,2,0,4],
    [11,9,3,1,5],[12,6,2,8,14],[13,7,4,10,16],
    [12,6,3,9,15],[13,7,5,11,17],[10,8,14,18,16],
    [11,9,15,19,17],[19,18,14,12,15],[19,18,16,13,17],
]


_GEO_LIB_CACHE: dict = {}

def _get_geo_lib():
    """Return cached ctypes lib, load once."""
    if "_lib" in _GEO_LIB_CACHE:
        return _GEO_LIB_CACHE["_lib"]
    candidates = [
        MODULE_DIR / "libgeojump.dll",
        MODULE_DIR / "libgeojump.so",
        MODULE_DIR / "build" / "libgeojump.dll",
    ]
    path = next((p for p in candidates if p.exists()), None)
    if not path:
        return None
    try:
        lib = ctypes.CDLL(str(path))
        lib.geo_dna_at.restype = ctypes.c_uint32
        lib.geo_dna_at.argtypes = [ctypes.POINTER(GeoDna), ctypes.c_uint32]
        lib.geo_dna_timeline.restype = ctypes.c_uint32
        lib.geo_dna_timeline.argtypes = [ctypes.POINTER(GeoDna), ctypes.c_uint32]
        lib.geo_dna_timeline_all.argtypes = [ctypes.POINTER(GeoDna), ctypes.c_void_p]
        _GEO_LIB_CACHE["_lib"] = lib
        return lib
    except OSError:
        return None


def geo_dna_from_walk(dna, walk, fibo_round=0):
    return None  # implemented via ctypes call on GeoDna

def geo_dna_at(dna_ptr, step):
    lib = _get_geo_lib()
    if lib is None or not isinstance(dna_ptr, GeoDna):
        return 0
    return lib.geo_dna_at(ctypes.byref(dna_ptr), step)

def geo_dna_timeline(dna_ptr, layer):
    lib = _get_geo_lib()
    if lib is None or not isinstance(dna_ptr, GeoDna):
        return 0
    return lib.geo_dna_timeline(ctypes.byref(dna_ptr), layer)


def test():
    print("=" * 56)
    print("GeoRouter v3: Metatron Hilbert-Peano")
    print("=" * 56)

    try:
        r = GeoRouter()
    except FileNotFoundError as e:
        print(f"  SKIP: {e}")
        return

    print(f"\n[1] Metatron grid: {r.METATRON_COLS}×{r.METATRON_ROWS}×{r.METATRON_FLOORS}")
    print(f"    BLOCK={r.BLOCK}  TOWER={r.TOWER}  FULL={r.FULL}")

    print(f"\n[2] Hilbert 4×4 cell indices:")
    for row in range(r.METATRON_ROWS):
        vals = [f"{r.hilbert_cell(col, row):2d}" for col in range(r.METATRON_COLS)]
        print(f"  row {row}: " + " ".join(vals))

    print(f"\n[3] Hilbert jump (col,row,floor) from node 0:")
    h1 = r.jump_hilbert(0, 1, 1, 1)
    h2 = r.jump_hilbert(0, 4, 4, 3)
    print(f"  hilbert(1,1,1)={h1}  hilbert(4,4,3)={h2}  diff={h1 != h2}")

    print(f"\n[4] Peano jump (col,row,floor) from node 0:")
    p1 = r.jump_peano(0, 1, 1, 1)
    p2 = r.jump_peano(0, 4, 4, 3)
    print(f"  peano(1,1,1)={p1}  peano(4,4,3)={p2}  diff={p1 != p2}")

    print(f"\n[5] Hilbert vs Peano same position (1,1,1):")
    h = r.jump_hilbert(0, 1, 1, 1)
    p = r.jump_peano(0, 1, 1, 1)
    print(f"  hilbert={h}  peano={p}  same={h == p}")

    print(f"\n[6] Ground jump (col,row) from node 0:")
    g1 = r.jump_ground(0, 1, 1)
    g2 = r.jump_ground(0, 3, r.BLOCK)
    print(f"  ground(1,1)={g1}  ground(3,{r.BLOCK})={g2}")

    print(f"\n[7] Pentagon lift (radial line, same face):")
    print(f"  lift(0, layer=0)={r.jump(0, GeoJumpType.PENTAGON, 0)}  (face 1, layer 0)")
    print(f"  lift(0, layer=5)={r.jump(0, GeoJumpType.PENTAGON, 5)}  (face 1, layer 5)")
    print(f"  lift(1728, layer=0)={r.jump(1728, GeoJumpType.PENTAGON, 0)}  (face 2, layer 0)")
    print(f"  lift(1728, layer=7)={r.jump(1728, GeoJumpType.PENTAGON, 7)}  (face 2, layer 7)")
    print(f"  lift(0, PENTAGON_FACE(2,3))={r.jump_pentagon_face(0, 2, 3)}  (face 2, layer 3)")
    print(f"  lift(0, PENTAGON_FACE(12,11))={r.jump_pentagon_face(0, 12, 11)}  (face 12, layer 11)")

    print(f"\n[8] Node 20700 metadata:")
    print(f"  clock_tick={r.clock_tick(20700)}  pentagon_id={r.pentagon_id(20700)}  shell_level={r.shell_level(20700)}")

    print(f"\n[9] Verify wrap-around:")
    ok = True
    for t in range(6):
        n = r.jump(r.FULL - 1, t, 1)
        if not (0 <= n < r.FULL):
            print(f"  FAIL type={t}: {n}")
            ok = False
    print(f"  {'OK' if ok else 'FAIL'}")

    print(f"\n[10] Hilbert locality (adjacent cells close):")
    h_adj = r.jump_hilbert(0, 2, 2, 1) - r.jump_hilbert(0, 2, 1, 1)
    h_far = r.jump_hilbert(0, 4, 4, 3) - r.jump_hilbert(0, 1, 1, 1)
    print(f"  adjacent diff={abs(h_adj)}  far diff={abs(h_far)}")

    print(f"\n[11] Centroid container zones (py):")
    c_inner, c_outer = 24, 144
    for d in [0, 12, 48, 200]:
        z = 0 if d < c_inner else (1 if d < c_outer else (2 if d < c_outer * 3 else 3))
        zn = ["INCIRCLE","MIDDLE","BETWEEN","OUTSIDE"][z]
        print(f"  dist={d:3d} -> {zn}")

    print(f"\n[12] Dodeca adjacency (face 0 edges):")
    for e in range(5):
        nb = r.dodeca_adj(0, 0, e)
        print(f"  edge {e} -> face {nb['face']} edge {nb['edge']}")

    print(f"\n[13] Face distance matrix (face 0):")
    for f in range(12):
        print(f"  dist(0,{f}) = {r.dodeca_face_dist(0, f)}")

    print(f"\n[14] Ring_step (face 0, ring 0, all edges):")
    for e in range(5):
        rs = r.ring_step(0, 0, e)
        print(f"  edge {e} -> face {rs['face']} ring {rs['ring']}")

    print(f"\n[15] Hex conversion (face 0):")
    for vs in range(5):
        v = r.ring_to_hex(0, vs)
        f = r.hex_to_face(v, 0)
        print(f"  vert_slot {vs} -> vertex {v} -> face {f}")

    print(f"\n[16] Shell layer live (layer 0-11 @ tick 13):")
    for l in range(12):
        live = r.shell_layer_live(l, 13)
        print(f"  layer {l}: {'LIVE' if live else 'frozen'}")

    print(f"\n[17] Shell fold nearest (layer 7 @ tick 13):")
    fn = r.shell_fold_nearest(7, 13)
    print(f"  layer 7 @ tick 13 -> fold to layer {fn}")

    print(f"\n[18] Ring hot path (pent 0, all layers, globe A/B):")
    for l in range(12):
        a = r.ring_hot_path(0, l, 0)
        b = r.ring_hot_path(0, l, 1)
        print(f"  layer {l}: A={a} B={b}  diff={b-a}")

    print(f"\n[19] ClimateField:")
    cf = _ClimateField(seed=1000, modality=0)
    cf.add(500, 64)
    cf.add(2000, 128)
    cf2 = _ClimateField(seed=1000, modality=0)
    cf2.add(500, 64)
    cf2.add(2000, 128)
    sim = cf.similarity(cf2)
    print(f"  attractors={cf.n_attract()} hot_paths={len(cf.hot_paths())}")
    print(f"  centroid={cf.centroid} similarity={sim}")
    cf.capo(5)
    print(f"  after capo(5): key={cf.key} centroid={cf.centroid} seed={cf.seed}")
    print(f"  to_dict keys: {list(cf.to_dict().keys())}")

    print(f"\n  All tests complete v3")

if __name__ == "__main__":
    test()
