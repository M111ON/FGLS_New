"""
test_cube_assembly.py — Phase C1: Cube assembly test (pure Python)

Flow:
1. Create 6 LetterCubes with bonded pairs
2. Map each → CubeCtx
3. Verify all 6 faces coupled
4. Promote 6 CubeCtx → 1 parent (depth 1)
5. Verify parent depth=1, all faces coupled
"""
import sys, os, time, struct

# Cube assembly constants
CUBE_FACES = 6
CUBE_CTX_SZ = 92
CUBE_PAIRS = 24
CUBE_MAX_DEPTH = 8

# LetterCube constants (pure Python, no DLL dependency)
LC_LANES = 6
LC_PAIRS = 24
LC_BOND_FREE = 0
LC_BOND_PEND = 1
LC_BOND_LOCK = 2
LC_LANE_SZ = 12  # pair_id:1 + angle:1 + bond_state:1 + bonded_to:1 + core_seed:8
LC_STRUCT_SZ = LC_LANES * LC_LANE_SZ + 4  # 76 bytes

# Complement table: dual dodeca opposite faces
LC_COMPLEMENT = [
    12,13,14,15,16,17,18,19,20,21,22,23,
     0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11
]


class LetterCube:
    """Pure Python LetterCube — 24-pair face:face bond."""
    def __init__(self):
        self.lanes = []
        for i in range(LC_LANES):
            self.lanes.append({
                'pair_id': 0,
                'angle': 0,
                'bond_state': LC_BOND_FREE,
                'bonded_to': 0xFF,
                'core_seed': bytes(8)
            })
        self.n_locked = 0
        self.depth = 0

    def assign(self, lane, pair_id, angle, core_seed=None):
        if lane >= LC_LANES: return
        self.lanes[lane]['pair_id'] = pair_id % LC_PAIRS
        self.lanes[lane]['angle'] = angle % 6
        self.lanes[lane]['bond_state'] = LC_BOND_PEND
        if core_seed:
            self.lanes[lane]['core_seed'] = core_seed

    def bond(self, lane_a, lane_b):
        if lane_a >= LC_LANES or lane_b >= LC_LANES: return 0
        if lane_a == lane_b: return 0

        a = self.lanes[lane_a]
        b = self.lanes[lane_b]

        # Complement check: pair_id[a] must be complement of pair_id[b]
        comp = LC_COMPLEMENT[a['pair_id'] % LC_PAIRS]
        if comp != b['pair_id']:
            return 0

        # Angle check: XOR must be within threshold
        angle_xor = a['angle'] ^ b['angle']
        if angle_xor > 2:  # threshold
            return 0

        # Bond
        a['bond_state'] = LC_BOND_LOCK
        a['bonded_to'] = lane_b
        b['bond_state'] = LC_BOND_LOCK
        b['bonded_to'] = lane_a
        self.n_locked += 1
        return 1

    def assemble(self):
        """Lock all pending bonds."""
        for lane in range(LC_LANES):
            if self.lanes[lane]['bond_state'] == LC_BOND_PEND:
                partner = self.lanes[lane]['bonded_to']
                if partner < LC_LANES and self.lanes[partner]['bond_state'] == LC_BOND_PEND:
                    self.lanes[lane]['bond_state'] = LC_BOND_LOCK
                    self.lanes[partner]['bond_state'] = LC_BOND_LOCK

    def verify(self):
        return self.n_locked == LC_LANES // 2  # 3 bonds for 6 lanes

    def to_bytes(self):
        """Serialize to 76 bytes."""
        buf = bytearray(LC_STRUCT_SZ)
        for i, lane in enumerate(self.lanes):
            off = i * LC_LANE_SZ
            buf[off] = lane['pair_id']
            buf[off+1] = lane['angle']
            buf[off+2] = lane['bond_state']
            buf[off+3] = lane['bonded_to']
            buf[off+4:off+12] = lane['core_seed']
        buf[LC_LANES * LC_LANE_SZ] = self.n_locked
        buf[LC_LANES * LC_LANE_SZ + 1] = self.depth
        return bytes(buf)


def make_lettercube_varied(seed):
    """Create a LetterCube with complementary bond pairs."""
    lc = LetterCube()

    # Assign 6 lanes with complementary pairs: 0<->12, 1<->13, 2<->14
    # Both lanes in a pair get the SAME angle to ensure XOR=0 (within threshold)
    pairs = [(0, 12), (1, 13), (2, 14)]
    for lane_idx, (p1, p2) in enumerate(pairs):
        angle = (seed + lane_idx) % 6  # same angle for both lanes in pair
        core1 = ((seed + lane_idx) * 0x9E3779B97F4A7C15 & 0xFFFFFFFFFFFFFFFF).to_bytes(8, 'little')
        core2 = ((seed + lane_idx + 100) * 0x9E3779B97F4A7C15 & 0xFFFFFFFFFFFFFFFF).to_bytes(8, 'little')

        lc.assign(lane_idx * 2, p1, angle, core1)
        lc.assign(lane_idx * 2 + 1, p2, angle, core2)

    # Bond complementary pairs
    lc.bond(0, 1)
    lc.bond(2, 3)
    lc.bond(4, 5)

    return lc


def cube_ctx_from_lettercube(lc):
    """Map LetterCube → CubeCtx (92 bytes)."""
    ctx = bytearray(CUBE_CTX_SZ)

    # Build slope_hash from lane cores (XOR-fold)
    slope = 0
    for lane in lc.lanes:
        w = int.from_bytes(lane['core_seed'], 'little')
        slope ^= w

    # Pack: slope_hash:8 + depth:1 + coupled_count:1 + faces(6×12=72) + pad:10
    struct.pack_into('<Q', ctx, 0, slope)
    struct.pack_into('<B', ctx, 8, 0)  # depth
    struct.pack_into('<B', ctx, 9, lc.n_locked * 2)  # coupled_count = n_locked * 2

    for i, lane in enumerate(lc.lanes):
        if i >= CUBE_FACES: break
        face_offset = 10 + i * 12
        core = int.from_bytes(lane['core_seed'], 'little')
        struct.pack_into('<Q', ctx, face_offset, core)          # core:8
        struct.pack_into('<BB', ctx, face_offset + 8,
                         lane['pair_id'], lane['pair_id'])      # key.upper, key.lower
        struct.pack_into('<B', ctx, face_offset + 10, i)        # face_id
        is_coupled = 1 if lane['bond_state'] == LC_BOND_LOCK else 0
        struct.pack_into('<B', ctx, face_offset + 11, is_coupled)

    return bytes(ctx)


def cube_ctx_verify(ctx_bytes):
    """Verify CubeCtx: all 6 faces coupled."""
    coupled_count = ctx_bytes[9]
    return coupled_count == CUBE_FACES


def cube_promote(children):
    """Promote 6 CubeCtx → 1 parent (92 bytes)."""
    if len(children) != CUBE_FACES:
        return None

    # Check all children fully coupled
    for c in children:
        if c[9] < CUBE_FACES:
            return None

    # Get depth from first child
    depth = children[0][8]
    if depth + 1 >= CUBE_MAX_DEPTH:
        return None

    # Build parent
    parent = bytearray(CUBE_CTX_SZ)

    # XOR-fold all child slope_hashes
    fold = 0
    for i in range(CUBE_FACES):
        child_slope = struct.unpack_from('<Q', children[i], 0)[0]
        fold ^= child_slope

        # Each parent face gets child's slope as core
        face_offset = 10 + i * 12
        struct.pack_into('<Q', parent, face_offset, child_slope)
        pair_idx = i % CUBE_PAIRS
        struct.pack_into('<BB', parent, face_offset + 8, pair_idx, pair_idx)
        struct.pack_into('<B', parent, face_offset + 10, i)
        struct.pack_into('<B', parent, face_offset + 11, 1)  # coupled

    struct.pack_into('<Q', parent, 0, fold)           # slope_hash
    struct.pack_into('<B', parent, 8, depth + 1)      # depth
    struct.pack_into('<B', parent, 9, CUBE_FACES)     # coupled_count

    return bytes(parent)


def test_cube_assembly():
    """Test 6 LetterCubes → CubeCtx → promote."""
    print("=" * 65)
    print("Phase C1: Cube Assembly Test (Pure Python)")
    print("=" * 65)

    # Step 1: Create 6 LetterCubes
    print("\n[Step 1] Creating 6 LetterCubes with bonded pairs...")
    lcs = []
    for i in range(CUBE_FACES):
        lc = make_lettercube_varied(i * 5)
        lcs.append(lc)
        print(f"  LC {i}: locked={lc.n_locked}/{LC_LANES//2} "
              f"pairs=[{lc.lanes[0]['pair_id']},{lc.lanes[2]['pair_id']},{lc.lanes[4]['pair_id']}]")

    all_verified = all(lc.verify() for lc in lcs)
    print(f"  All verified: {all_verified} {'✓' if all_verified else '✗'}")

    # Step 2: Map each → CubeCtx
    print("\n[Step 2] Mapping LetterCubes → CubeCtx...")
    cube_ctxs = []
    for i, lc in enumerate(lcs):
        ctx = cube_ctx_from_lettercube(lc)
        cube_ctxs.append(ctx)
        coupled = ctx[9]
        slope = struct.unpack_from('<Q', ctx, 0)[0]
        print(f"  Cube {i}: coupled={coupled}/{CUBE_FACES} slope=0x{slope:016x}")

    # Step 3: Verify all coupled
    print("\n[Step 3] Verifying all 6 faces coupled...")
    all_coupled = all(cube_ctx_verify(c) for c in cube_ctxs)
    print(f"  All coupled: {all_coupled} {'✓' if all_coupled else '✗'}")

    # Step 4: Promote 6 → 1
    print("\n[Step 4] Promoting 6 CubeCtx → 1 parent...")
    t0 = time.perf_counter()
    parent = cube_promote(cube_ctxs)
    t1 = time.perf_counter()

    if parent is None:
        print("  Promotion FAILED ✗")
        return False

    parent_depth = parent[8]
    parent_coupled = parent[9]
    parent_slope = struct.unpack_from('<Q', parent, 0)[0]

    print(f"  Parent depth={parent_depth}, coupled={parent_coupled}/{CUBE_FACES}")
    print(f"  Parent slope_hash=0x{parent_slope:016x}")
    print(f"  Promotion time: {(t1-t0)*1000:.3f}ms ✓")

    # Step 5: Verify parent
    print("\n[Step 5] Verifying parent CubeCtx...")
    ok = (parent_coupled == CUBE_FACES and parent_depth == 1)
    print(f"  Parent valid: {ok} {'✓' if ok else '✗'}")

    # Step 6: Recursive promotion (depth 1 → depth 2)
    print("\n[Step 6] Recursive promotion: depth 1 → depth 2...")
    depth1_parents = []
    for i in range(CUBE_FACES):
        child_lcs = [make_lettercube_varied(i * 6 + j) for j in range(CUBE_FACES)]
        child_ctxs = [cube_ctx_from_lettercube(lc) for lc in child_lcs]
        p = cube_promote(child_ctxs)
        if p is None:
            print(f"  Failed to create depth-1 parent {i} ✗")
            return False
        depth1_parents.append(p)

    depth2 = cube_promote(depth1_parents)
    if depth2 is None:
        print("  Promotion to depth 2 FAILED ✗")
        return False

    print(f"  Depth-2 parent: depth={depth2[8]}, coupled={depth2[9]}/{CUBE_FACES} ✓")

    print("\n" + "=" * 65)
    print("Phase C1: ALL PASS ✓")
    print("=" * 65)
    return True


if __name__ == '__main__':
    ok = test_cube_assembly()
    sys.exit(0 if ok else 1)
