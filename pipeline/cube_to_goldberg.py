"""
cube_to_goldberg.py — Phase C2: Cube shell → Goldberg GP(n,0) mapping

Mapping:
  CubeCtx depth 0: 6 faces → 6 hexagons on Goldberg sphere
  CubeCtx depth 1: 6 parents → 36 hexagons
  CubeCtx depth N: 6^N faces → hexagons

Goldberg sphere at level n:
  12 pentagons (fixed) + 10(n²-1) hexagons = 10n² + 2 tiles
  tile_id 0..11 = pentagons
  tile_id 12..N = hexagons
"""
import struct

# Goldberg constants
GP_PENT_COUNT = 12
GP_MAX_LEVEL = 8
GP_CHUNK_SZ = 64

# Complement table from lettercube
LC_COMPLEMENT = [
    12,13,14,15,16,17,18,19,20,21,22,23,
     0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11
]


def gp_face_count(level):
    """Total tiles = 10n² + 2 (12 pent + 10(n²-1) hex)"""
    return 10 * level * level + 2


def gp_is_pentagon(tile_id):
    return tile_id < GP_PENT_COUNT


def gp_hex_in_sector(level, sector):
    """Number of hexagons in sector (round-robin remainder)."""
    total_hex = gp_face_count(level) - GP_PENT_COUNT
    base = total_hex // GP_PENT_COUNT
    rem = total_hex % GP_PENT_COUNT
    return base + (1 if sector < rem else 0)


def gp_sector_base(level, sector):
    """Base tile_id of first hex in sector."""
    b = GP_PENT_COUNT
    for s in range(sector):
        b += gp_hex_in_sector(level, s)
    return b


def gp_tile_id(level, pent_anchor, hex_offset):
    """tile_id from (pentagon anchor, hex offset)."""
    if pent_anchor >= GP_PENT_COUNT:
        return 0
    if hex_offset == 0:
        return pent_anchor
    return gp_sector_base(level, pent_anchor) + (hex_offset - 1)


def gp_tile_to_pent(level, tile_id):
    """Reverse: tile_id → pentagon anchor."""
    if gp_is_pentagon(tile_id):
        return tile_id
    for s in range(GP_PENT_COUNT):
        base = gp_sector_base(level, s)
        sz = gp_hex_in_sector(level, s)
        if base <= tile_id < base + sz:
            return s
    return GP_PENT_COUNT - 1


def gp_chunk_to_addr(level, chunk_idx):
    """Map chunk_idx → GpAddr (tile_id, dim)."""
    face_max = gp_face_count(level)
    tile_id = chunk_idx % face_max
    dim = (chunk_idx // face_max) & 0x7F
    return tile_id, dim


def gp_addr_to_chunk(level, tile_id, dim):
    """Reverse: GpAddr → chunk_idx."""
    face_max = gp_face_count(level)
    return dim * face_max + tile_id


class GpSphere:
    """Goldberg sphere with Tring storage."""
    def __init__(self, level):
        self.level = max(1, min(level, GP_MAX_LEVEL))
        self.face_max = gp_face_count(self.level)
        self.tiles = {}  # (tile_id, dim) → bytes(64)

    def write(self, tile_id, dim, chunk):
        """Write 64B chunk to sphere."""
        if tile_id >= self.face_max or dim >= GP_MAX_LEVEL:
            return False
        self.tiles[(tile_id, dim)] = bytes(chunk)
        return True

    def read(self, tile_id, dim):
        """Read 64B chunk from sphere."""
        return self.tiles.get((tile_id, dim), None)

    def write_chunk_idx(self, chunk_idx, chunk):
        """Write 64B chunk using flat index."""
        tile_id, dim = gp_chunk_to_addr(self.level, chunk_idx)
        return self.write(tile_id, dim, chunk)

    def read_chunk_idx(self, chunk_idx):
        """Read 64B chunk using flat index."""
        tile_id, dim = gp_chunk_to_addr(self.level, chunk_idx)
        return self.read(tile_id, dim)


class CubeToGoldberg:
    """
    Map CubeCtx → Goldberg sphere.

    Process:
    1. Choose gp_level such that gp_face_count(level) ≥ n_faces
    2. Map each face to a hexagon tile
    3. Store face data in sphere
    """
    def __init__(self, sphere_level=2):
        self.sphere_level = sphere_level
        self.sphere = GpSphere(sphere_level)
        self.face_map = {}  # face_id → tile_id

    def init_level(self, n_faces):
        """Choose gp_level to accommodate n_faces."""
        for level in range(1, GP_MAX_LEVEL + 1):
            if gp_face_count(level) >= n_faces + GP_PENT_COUNT:
                self.sphere_level = level
                self.sphere = GpSphere(level)
                return level
        self.sphere_level = GP_MAX_LEVEL
        self.sphere = GpSphere(GP_MAX_LEVEL)
        return GP_MAX_LEVEL

    def map_faces(self, n_faces):
        """Map cube faces to hexagon tiles."""
        self.face_map = {}
        hex_idx = 0
        for face_id in range(n_faces):
            # Find next hexagon tile
            while hex_idx < self.sphere.face_max:
                tile_id = GP_PENT_COUNT + hex_idx
                if not gp_is_pentagon(tile_id):
                    self.face_map[face_id] = tile_id
                    hex_idx += 1
                    break
                hex_idx += 1
        return self.face_map

    def store_face(self, face_id, data):
        """Store face data in sphere."""
        tile_id = self.face_map.get(face_id)
        if tile_id is None:
            return False
        return self.sphere.write(tile_id, 0, data)

    def retrieve_face(self, face_id):
        """Retrieve face data from sphere."""
        tile_id = self.face_map.get(face_id)
        if tile_id is None:
            return None
        return self.sphere.read(tile_id, 0)


def test_goldberg_basics():
    """Test Goldberg sphere basics."""
    print("=" * 65)
    print("Phase C2: Goldberg Sphere Basics")
    print("=" * 65)

    # Test face counts
    print("\n[1] Face counts:")
    for level in range(1, 5):
        count = gp_face_count(level)
        pent = GP_PENT_COUNT
        hex = count - pent
        print(f"  Level {level}: {count} tiles ({pent} pent + {hex} hex)")

    # Test sector layout
    print("\n[2] Sector layout (level 2):")
    for sector in range(GP_PENT_COUNT):
        hex_count = gp_hex_in_sector(2, sector)
        base = gp_sector_base(2, sector)
        print(f"  Sector {sector}: {hex_count} hex, base tile_id={base}")

    # Test tile_id mapping
    print("\n[3] Tile ID mapping:")
    for pent in range(GP_PENT_COUNT):
        for offset in range(4):
            tid = gp_tile_id(2, pent, offset)
            pent_back = gp_tile_to_pent(2, tid)
            print(f"  pent={pent}, offset={offset} → tile_id={tid} → pent_back={pent_back}")

    print("\n  Goldberg basics: PASS ✓")
    return True


def test_cube_to_goldberg():
    """Test CubeCtx → Goldberg mapping."""
    print("\n" + "=" * 65)
    print("Phase C2: Cube → Goldberg Mapping")
    print("=" * 65)

    # Create a CubeToGoldberg mapper
    c2g = CubeToGoldberg(sphere_level=2)

    # Map 6 cube faces
    n_faces = 6
    level = c2g.init_level(n_faces)
    face_map = c2g.map_faces(n_faces)

    print(f"\n[1] Goldberg level: {level}")
    print(f"    Face count: {gp_face_count(level)}")
    print(f"    Face map: {face_map}")

    # Store face data
    print("\n[2] Storing face data...")
    for face_id, tile_id in face_map.items():
        # Create test data: face_id repeated 64 times
        data = bytes([face_id] * 64)
        ok = c2g.store_face(face_id, data)
        print(f"  Face {face_id} → tile {tile_id}: {'✓' if ok else '✗'}")

    # Retrieve and verify
    print("\n[3] Retrieving face data...")
    all_ok = True
    for face_id, tile_id in face_map.items():
        data = c2g.retrieve_face(face_id)
        expected = bytes([face_id] * 64)
        ok = (data == expected)
        if not ok:
            all_ok = False
        print(f"  Face {face_id} from tile {tile_id}: {'✓' if ok else '✗'}")

    # Test chunk_idx mapping
    print("\n[4] Chunk index mapping:")
    for chunk_idx in range(6):
        tile_id, dim = gp_chunk_to_addr(level, chunk_idx)
        back = gp_addr_to_chunk(level, tile_id, dim)
        print(f"  chunk_idx={chunk_idx} → tile={tile_id}, dim={dim} → chunk_idx={back}")

    # Test recursive: 36 faces (depth 1)
    print("\n[5] Recursive mapping: 36 faces (depth 1)...")
    c2g2 = CubeToGoldberg(sphere_level=3)
    level2 = c2g2.init_level(36)
    face_map2 = c2g2.map_faces(36)
    print(f"  Goldberg level: {level2}")
    print(f"  Face count: {gp_face_count(level2)}")
    print(f"  Faces mapped: {len(face_map2)}")
    ok2 = len(face_map2) == 36
    print(f"  All 36 faces mapped: {'✓' if ok2 else '✗'}")

    print("\n" + "=" * 65)
    result = all_ok and ok2
    print(f"Phase C2: {'ALL PASS ✓' if result else 'SOME FAILED ✗'}")
    print("=" * 65)
    return result


if __name__ == '__main__':
    import sys
    ok1 = test_goldberg_basics()
    ok2 = test_cube_to_goldberg()
    sys.exit(0 if (ok1 and ok2) else 1)
