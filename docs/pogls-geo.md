# pogls_geo — Geometric Addressing Layer

Y-triangle address space: `144² = 20736 = 128 × 162`

## Address Space Design

The address space `POGLS_GEO_FULL = 20736` bridges two naturally occurring
dimensions in transformer models and Goldberg polyhedron topology:

`20736 = 128 × 162`

- **128 = 2⁷** — the `head_dim` of common transformer architectures (Q/K/V
  head size). A power-of-two that maps cleanly onto binary memory alignment
  and GPU tensor layouts.
- **162 = 2 × 3⁴** — the number of Y-triangle faces in the icosahedral
  Goldberg polyhedron (12 pentagons × 9 sub-faces per pentagon = 108, plus
  54 from neighbouring faces = 162 after subdivision). The `2 × 3⁴`
  factorisation reflects the ternary subdivision of each pentagon face.

`144 = 2⁴ × 3² = 16 × 9` — the tower/block size that sits at the geometric
mean of the two dimensions. Because `144² = 20736`, the address space can
be decomposed as a 144×144 grid, where one axis aligns with the transformer
head dimension and the other with the Y-triangle face count.

This dual factorisation — binary from the head dimension, ternary from the
icosahedral subdivision — is what makes the space "geometric": it can be
used as a flat linear address (`uint32_t` modulo 20736) or decomposed into
hierarchical tiers that mirror the polyhedron's natural topology.

## Coordinate System

A linear address in `[0, 20736)` decomposes into five tiers via the
constants:

| Constant             | Value | Derivation                      |
|----------------------|-------|---------------------------------|
| `METATRON_COLS`      | 4     | columns per metatron            |
| `METATRON_ROWS`      | 4     | rows per metatron               |
| `METATRON_CELLS`     | 16    | `4 × 4`                         |
| `METATRON_FLOORS`    | 3     | ternary height per cell         |
| `BLOCK`              | 48    | `16 × 3`                        |
| `TOWER`              | 144   | `48 × 3`                        |
| `FULL`               | 20736 | `144 × 144`                     |

Decomposition (`pogls_geo_coord`):

```
addr → tow(=addr/TOWER) → floor → cell → sector
                        ↓
                     block = cell / 4
```

- **sector** (0–15): finest grain, a `4×4` metatron of `METATRON_CELLS`
- **block** (0–2): derived as `cell / METATRON_COLS`, groups sectors into
  rows inside a cell
- **cell** (0–2): groups `3 × METATRON_CELLS` sectors (`BLOCK` size)
- **floor** (0–2): three floors per tower (`BLOCK` height)
- **tow** (0–143): the outermost tier (`TOWER` size)

Roundtrip — `pogls_geo_from_coord(pogls_geo_coord(addr, &c))` returns
`addr % 20736` — is exact for all valid addresses. The `block` field is
derived (not stored in the decomposition), so `from_coord` does not read
it; it recomputes from `cell`.

## Face Jump

`pogls_geo_jump(from, delta)` adds a signed integer delta to an address
with wrap at the `POGLS_GEO_FULL` boundary. The implementation uses
`int64_t` to handle negative deltas safely:

```
result = (from + delta) mod 20736
```

Negative deltas add `POGLS_GEO_FULL` until the value is non-negative, then
modulo. This is the primitive for traversing the face address space in a
cyclic manner — used by the SID (Session ID) perturbation engine to
"rotate" the capo across the 12 pentagon faces.

`pogls_geo_face_addr(base, face)` jumps by `face × BLOCK` (48-address
increments). Since each face owns `BLOCK` addresses, face 0–11 linearly
map onto the address space with wrap at 20736. This gives 12 × 48 = 576
addresses per full rotation, and `20736 / 48 = 432` rotations per full
space.

## Name Hash

`pogls_geo_name_hash(name)` implements **FNV-1a** (Fowler–Noll–Vo) with:

| Param       | Value      |
|-------------|------------|
| Offset base | `0x811c9dc5` |
| Prime       | `0x01000193` |
| Modulo      | `POGLS_GEO_FULL` (20736) |

All tensor names in the GGUF model (e.g. `blk.0.attn_q.weight`) hash to
a deterministic address in `[0, 20736)`. Zero-length and NULL names
return 0. This is used by the DRamTile store to map named tensors to
geometric addresses without a separate lookup table.

## Triplet Topology

The `pogls_geo_triplet_vert()` function returns the vertex index for a
given face, edge, and vertex position on that edge. The topology is a
**12-face icosahedron** pentagon system:

- 12 pentagon faces (indices 0–11)
- 5 edges per face
- 3 vertices per edge (the two endpoints + one mid-point)
- 12 distinct vertices (indices 0–11) in total — shared across faces

The triplet table (`triplets[12][5][3]` in the implementation) encodes the
CCW winding of each edge around each face. For example:

- Face 0: edges `[0-1-2]`, `[0-2-3]`, `[0-3-4]`, `[0-4-5]`, `[0-5-1]`
  — all five edges radiate from vertex 0 (the "cap").
- Faces 6–11: the bottom hemisphere — edges reference vertices 6–11,
  forming the mirror half.

This table is used by the triplet world (`TRIPLET_FACE_VERTS`,
`TRIPLET_PENTAKIS_TRI`) for geometric address-to-position mapping in the
Zeus-level pipeline (zero-copy disk → RAM → CPU → GPU).

## API Reference

| Function | Description |
|----------|-------------|
| `pogls_geo_coord(addr, c)` | Decompose linear address into tiered coord |
| `pogls_geo_from_coord(c)`  | Reconstruct linear address from coord (roundtrip) |
| `pogls_geo_jump(from, delta)` | Signed jump with wrap at 20736 |
| `pogls_geo_face_addr(base, face)` | Offset by `face × BLOCK` with wrap |
| `pogls_geo_addr_valid(addr)` | Check `addr < 20736` |
| `pogls_geo_name_hash(name)` | FNV-1a hash → modulo 20736 |
| `pogls_geo_triplet_vert(face, edge, vert_idx)` | Vertex lookup for icosa edge |
| `pogls_geo_tier_name(tier)` | Human-readable tier size string |

## Usage

```c
PoglsGeoCoord c;
pogls_geo_coord(10000, &c);
uint32_t back = pogls_geo_from_coord(&c);
uint32_t jumped = pogls_geo_jump(100, 50);

// Name → address (DRamTile tensor lookup)
uint32_t addr = pogls_geo_name_hash("blk.0.attn_q.weight");

// Face rotation (SID capo)
for (uint32_t face = 0; face < 12; face++) {
    uint32_t face_start = pogls_geo_face_addr(base, face);
    // ... swap face data via SID
}

// Triplet topology query
uint32_t v = pogls_geo_triplet_vert(3, 2, 1);
// v = 4 for face 3, edge 2, vert index 1
```
