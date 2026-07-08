Complete Analysis: geo_field and geopixel Subsystems
1. File Paths Found and Their Purposes
geo_field core files:

I:\FGLS_new\collection\dgls\geo\include\geo_field_core.h — Main unified geometric encode/decode field (780 lines). Maps byte streams onto a Goldberg polyhedron coordinate system. Integrates GpSphere, FrustumBlock, Metatron routing, Trit decomposition, Flow chunking.
I:\FGLS_new\collection\dgls\geo\include\geo_field_core_3.h — Updated variant with heptagon fence + atomic reshape support (797 lines).
I:\FGLS_new\collection\dgls\geo\include\geo_field_ring.h — Ring 120 classifier + full pipeline routing (Junction 30, Core 24, geo_jump).
I:\FGLS_new\collection\dgls\geo\include\geo_field_climate.h — Geometric tensor field with attractors, capo shift, centroid computation (5 modalities: TEXT/AUDIO/IMAGE/VIDEO/GEO).
I:\FGLS_new\collection\geopixel\geofield\geo_field_main.c — Demo program with 4 demos (complete loop, scale, shape access, file roundtrip).
I:\FGLS_new\collection\geo_jump_module\test_geo_field_icosphere.c — 12-test suite for icosphere integration into geo_field.
geo_field bridge:

I:\FGLS_new\collection\dgls\geo\src\geo_field_bridge.c — C bridge: reads any file, runs geo_field roundtrip, outputs JSON with encode stats + roundtrip PASS/FAIL + skeleton histogram.
I:\FGLS_new\collection\geopixel\geofield\geo_field_bridge.c — Duplicate/alternate bridge.
I:\FGLS_new\collection\python_src\geo_field_bridge.py — Python wrapper for the bridge.
geopixel core files:

I:\FGLS_new\collection\geopixel\geopixel\geo_pixel.h — Core GeoPixel encode/decode (142 lines). Maps integer index → RGB pixel with 5 decoded fields (trit, spoke, coset, letter, fibo).
I:\FGLS_new\collection\geopixel\pogls_to_geopixel.h — Path A bridge: H64 → seed-only tiles (4B each, 16x ratio).
I:\FGLS_new\collection\dgls\diamond\hbv_bundle\pogls_to_geopixel.h — Path B bridge: H64 → full pixel synthesis tiles (64 pixels per tile).
I:\FGLS_new\core\bond_to_geopixel.h — Bond layer ↔ GeoPixel bridge. V1 (27px stripe), V2 (9px stripe), V3 (frame-predictive, 9B for 25B piece = 0.36x ratio).
I:\FGLS_new\runner\pogls_v3_geopixel.h — POGLS v3 header-only store (20KB header maps 256 tensors → GGUF offsets, 41,681x smaller than full data).
geopixel compression pipeline:

I:\FGLS_new\collection\geopixel\hbv_bundle\hamburger_classify.h — Tile classifier: FLAT/GRADIENT/EDGE/NOISE using integer variance (x100).
I:\FGLS_new\collection\geopixel\hbv_bundle\hamburger_encode.h — Hamburger tile codec: encode/decode with 7 codecs (SEED, DELTA, RICE3, HILBERT, FREQ, ZSTD19, HEX).
I:\FGLS_new\collection\dgls\diamond\include\diamond_shell_v2.h — Diamond Shell v2: 3D rotation scan (6 orientations) + fibo_intersect discriminator.
I:\FGLS_new\collection\dgls\diamond\include\diamond_shell_codec.h — Diamond Shell wire format + serialize/deserialize.
I:\FGLS_new\collection\dgls\diamond\include\binary_shell_codec.h — Binary shell: FLAT (2B), SPARSE (10+nz B), DENSE (6+zstd B).
I:\FGLS_new\collection\geopixel\geopixel_container_map.md — Container architecture document.
I:\FGLS_new\collection\geopixel\hbv_bundle\Pogls_full pipeline.md — Full pipeline analysis document.
I:\FGLS_new\docs\pogls-v3-geopixel.md — POGLS v3 docs.
2. The geo_field Pipeline Flow
[ANY binary data: file, mmap, network, sensor]
    │
    ▼  split into 64B chunks (zero-padded tail)
    │
    ▼  geo_field_encode()
    │
    ├── Map chunk_idx → GpAddr {tile_id, dim}
    │     tile_id = chunk_idx % face_max
    │     dim     = (chunk_idx / face_max) & 0x7F
    │
    ├── Compute tring encoding (stride-37 walk for position 0..719)
    │
    ├── Block addressing:
    │     block_idx = dim * blocks_per_layer + (tile_id / 54)
    │     slot      = tile_id % 54  (diamond slot within FrustumBlock)
    │
    ├── Zone boundary check: pentagon tiles → reset skeleton context
    │
    ├── Write 64B data into FrustumBlock at diamond_slot * 64
    │
    ├── Mark drain active (pentagon anchor primary + TRing spoke secondary)
    │
    ├── Set shadow zone occupied bit
    │
    └── Skeleton encode decision for stats (ID/FLAT/DIFF/BREF/GEOM/RAW)
    │
    ▼
[FrustumBlock array: each 4896B, 54 diamond slots]
    │
    ▼  geo_field_save()
[Serialized .geofield file: header(32B) + blocks(n×4896B)]

─── DECODE PATH ───

[.geofield file]
    │
    ▼  geo_field_load()
    │
    ▼  geo_field_decode()
    │
    ├── For each chunk_idx:
    │     GpAddr → block_idx → diamond_slot → read 64B
    │
    ▼
[Original data (bit-exact)]
Key constants:

Chunk size: 64B (1 CPU cache line)
FrustumBlock: 4896B (54 diamond slots × 64B + metadata)
gp_level 1..8: subdivision depth (face_count = 10n²+2)
gp_level=4: 162 tiles (= icosphere f=4 vertices)
gp_level=8: 642 tiles
3. The geopixel Encoder Types and Block Format
GeoPixel RGB Encoding (geo_pixel.h):

// Encode: idx → RGB
R = ((idx%27) << 3) | (idx%6)       // trit(5b) | spoke(3b)
G = ((idx%9)  << 4) | (idx%26 & 0xF) // coset(4b) | letter_lo(4b)
B = idx % 144                         // fibo clock position

// Decode: RGB → 5 fields  
GeoFields { trit(0..26), spoke(0..5), coset(0..8), letter(0..25 low 4 bits), fibo(0..143) }
Hamburger Tile Classification (hamburger_classify.h) — 4 types:

Type	Variance Threshold (x100)	Codec	Description
FLAT (TTYPE=0)	avg_var < 400	CODEC_SEED	≤16 unique YCgCo combos, low variance. Store 4B seed + 6B sample = 8B per tile.
GRADIENT (TTYPE=1)	avg_var < 10,000	CODEC_FREQ	Smooth structure. Block avg + LEB128 zigzag residuals per channel.
EDGE (TTYPE=2)	avg_var < 80,000	CODEC_ZSTD19	Structured high-variance. Zstd level 19 (or raw fallback).
NOISE (TTYPE=3)	avg_var ≥ 80,000	CODEC_ZSTD19	True high-frequency. Zstd level 19 compression.
Hamburger Codecs (hamburger_encode.h):

Codec	ID	Operation	Typical Output
NONE	0	Copy verbatim (passthrough)	Same as input
SEED	1	FLAT: 8B header + 6B sample; non-flat: fall to DELTA	8B for FLAT tiles
DELTA	2	XOR each byte vs seed-derived prediction	Input size + 2B header
RICE3	3	Rice(k=3) on zigzag'd deltas	Variable (bit stream)
HILBERT	4	Walk/no-walk bitmask per position	Bitmask + differing bytes
FREQ	5	Block avg + LEB128 signed residuals	Variable, good for smooth
ZSTD19	6	Zstd level 19 (raw fallback if no win)	Compressed or raw
HEX	7	7-byte hex tile codec	Compact hex storage
RAW	8	Explicit passthrough fallback	Same as input
Diamond Shell v2 (diamond_shell_v2.h) — 64B chunks treated as 4x4x4 cube:

Flag	Wire Size	Condition	Discriminator
FLAT (0)	2B (flag+rot)	fibo_intersect popcount == 0	No geometric structure
SPARSE (1)	9B (flag+rot+seed)	popcount ≤ 4	Weak structure
DENSE (2)	17B (flag+rot+diff_a+diff_b)	popcount > 4	Strong structure
BATCH (3)	5B (batch_id+chunk_z)	Belongs to larger batch	Part of N-chunk group
The discriminator fold_fibo_intersect() ANDs 4 byte-rotated copies of the DiamondBlock's core slot — surviving bits are geometric invariants of that data. High popcount = strong geometric alignment.

Binary Shell (binary_shell_codec.h) — simplified variant with zstd:

Flag	Size	Description
FLAT (0)	2B	All-zero chunk
SPARSE (1)	10 + nz×2 B	≤16 non-zero bytes in best rotation (store index+value pairs)
DENSE (2)	6 + zstd_sz B	Zstd-compressed rotated 64B (or raw 70B if compression no win)
4. How They Compose Together (geo_field → geopixel = Maximum Compression)
The two systems are not stacked directly in a single pipeline. They represent different tiers of the POGLS geometric compression hierarchy:

TIER 1: GEO_FIELD  — Geometric Address Space (zero-copy routing)
──────────────────────────────────────────────────────────────
  ANY binary stream → 64B chunks → Goldberg sphere tile addressing
  → FrustumBlock storage → IO via pointer swap (no memcpy hotpath)
  
  Use case: SID tensor swap, zero-copy disk↔RAM↔GPU
  Output: .geofield file or in-memory FrustumBlock array

TIER 2: DIAMOND SHELL — 3D Cube Compression (per-chunk)
──────────────────────────────────────────────────────────────
  64B chunk → 4×4×4 cube → 6 rotation tests → fold_fibo_intersect
  → FLAT/SPARSE/DENSE classification → compressed wire format
  
  Use case: Tensor weight compression, chunk-level storage
  Output: 2B..17B per chunk (FLAT=2B, SPARSE=9B, DENSE=17B)

TIER 3: HAMBURGER/GEOPIXEL — Tile Codec (visual fingerprint)
──────────────────────────────────────────────────────────────
  ScanEntry[] → H64 HilbertPacket → GeopixelBridge → tile classify
  → FLAT/GRADIENT/EDGE/NOISE → per-tile codec → .gpx5 file
  
  Use case: Visual fingerprinting, content-addressable storage
  Output: .gpx5 file (seed + invert chain, O(1) random access)

TIER 4: BOND_TO_GEOPIXEL — Deterministic Geometric Storage
──────────────────────────────────────────────────────────────
  PoglsPiece → V3 compress: [geo_key(8B) + fold_axis(1B)] = 9B
  Bond piece is fully deterministic from (geo_key, fold_axis)
  → RGB image is a deterministic projection, never stored
  
  Use case: Bond layer metadata, geometric DNA
  Output: 9B for 25B piece = 0.36× ratio
The stacking hierarchy (maximum compression path):

RAW DATA (any bytes)
    │
    ▼  geo_field_encode → FrustumBlocks (geometric addressing)
    │
    ▼  For each 64B chunk in each FrustumBlock:
    │   diamond_shell classify → FLAT(2B)/SPARSE(9B)/DENSE(17B)
    │
    ▼  (Optional) H64 Hilbert grid + hamburger encode → .gpx5
    │
    ▼  (Optional) Bond layer V3: 9B per 25B piece = 0.36×
5. Compression Ratios from Benchmarks
Technique	Raw Size	Encoded Size	Ratio	Data Type
GeoPixel Path A (seed-only)	4096B (64 tiles)	256B (4B/tile + 16B hdr)	16×	H64 Hilbert cells
Diamond Shell FLAT	64B	2B	32×	All-zero chunks
Diamond Shell SPARSE	64B	9B	7.1×	Sparse non-zero chunks
Diamond Shell DENSE	64B	17B	3.76×	Dense non-zero chunks
Binary Shell SPARSE	64B	10-42B	1.5-6.4×	≤16 non-zero bytes
Binary Shell DENSE	64B	~70B	0.91×	Zstd can't compress random data
Bond V3 (frame-predictive)	25B (PoglsPiece)	9B	2.78× (0.36× ratio)	Geometric metadata
POGLS v3 (header-only)	4.79 GB (GGUF)	20.2 KB	41,681×	Tensor weight lookup
Real-world benchmark from bench_v3_integrated.c:

L2 batch of 64 similar chunks (2 varying bytes each): ~708B for 4KB raw = ~5.8× ratio
Level distribution (C source file): mixed levels, sparse encoding wins for structured data
6. Key Code Snippets
GeoField Encode: chunk → GpAddr → FrustumBlock (geo_field_core.h:158-232):

static inline int geo_field_encode_chunk(GeoField *gf, uint64_t chunk_idx,
                                          const uint8_t chunk[64],
                                          SkelEncCtx *skel, GeoFieldEncodeStats *stats)
{
    GpAddr a = gp_chunk_to_addr(gf->gp_level, chunk_idx);     // 1. map to address
    uint32_t block_idx = (uint32_t)a.dim * blocks_per_layer + a.tile_id / 54;
    FrustumBlock *blk = &gf->blocks[block_idx];
    if (gp_is_zone_boundary(a.tile_id)) skel_enc_zone_reset(skel);  // 2. zone boundary
    uint8_t diamond_slot = (uint8_t)(a.tile_id % 54);
    memcpy(blk->data + diamond_slot * 64, chunk, 64);            // 3. write into block
    // mark drain active
    blk->meta.drain_state[drain_idx % 12] |= 0x01u;
}
GeoPixel Encode: index → RGB (geo_pixel.h:50-57):

static inline GeoPixel geo_pixel_encode(uint32_t idx, uint32_t W) {
    uint32_t i = (W > 0) ? (idx % W) : idx;
    GeoPixel p;
    p.r = (uint8_t)(((i % 27) << 3) | (i % 6));   // trit(5b) | spoke(3b)
    p.g = (uint8_t)(((i % 9)  << 4) | (i % 26 & 0xFu)); // coset(4b) | letter_lo(4b)
    p.b = (uint8_t)(i % 144);                       // fibo clock position
    return p;
}
Diamond Shell: rotation scan + fibo_intersect discriminator (diamond_shell_v2.h:211-270):

for (uint8_t rot = 0; rot < 6; rot++) {
    _shell_rotate64(rotbuf, chunk, rot);                        // 3D re-index
    DiamondBlock db = _shell_chunk_to_block(rotbuf, rot, chunk_z);
    uint64_t isect = fold_fibo_intersect(&db);                  // AND of 4 rotated copies
    int pc = __builtin_popcountll(isect);                       // geometric invariant bits
    if (pc > best_pc) { best_pc = pc; best_isect = isect; best_rot = rot; }
}
if (best_pc == 0)           r.flag = SHELL_FLAG_FLAT;   // 2B
else if (best_pc <= 4)      r.flag = SHELL_FLAG_SPARSE; // 9B
else                        r.flag = SHELL_FLAG_DENSE;  // 17B
Hamburger tile classifier: FLAT/GRADIENT/EDGE/NOISE (hamburger_classify.h:56-135):

// Variance of best predictor (MED/GRAD/LEFT/TOP/AVG) scaled ×100
if (is_flat && avg_var_x100 < 400)   return GPX5_TTYPE_FLAT;     // → CODEC_SEED (8B)
if (avg_var_x100 < 10000)            return GPX5_TTYPE_GRADIENT; // → CODEC_FREQ
if (avg_var_x100 < 80000)            return GPX5_TTYPE_EDGE;     // → CODEC_ZSTD19
return GPX5_TTYPE_NOISE;                                          // → CODEC_ZSTD19
Bond V3: 25B piece → 9B seed (bond_to_geopixel.h:293-318):

// Compress: store only geo_key(8B) + fold_axis(1B)
// Decompress: re-derive bond_L, bond_R from geo_key via fibo_addr()
// This works because geometry is deterministic:
//   bond_L = fibo_addr(geo_key ^ SALT_L)
//   bond_R = fibo_addr(geo_key ^ SALT_R)
static inline void bond_piece_compress_v3(const PoglsPiece *p, uint8_t out[9]) {
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)(p->geo_key >> (i * 8u));
    out[8] = _gpv3_shape_to_axis(p->shape);  // shape → fold_axis (0..7)
}
FrustumBlock file format (from geo_field_core.h:633-650):

[header: 32B]
  magic "GEOF" (4B) + version (1B) + gp_level (1B) + pad (1B)
  n_blocks (4B) + orig_size (8B) + xxh64 digest (8B) + pad (4B)
[blocks: n×4896B]
  Each FrustumBlock: data[3456B] + meta[1440B] = 4896B
    - 54 diamond slots × 64B = 3456B data zone
    - drain_state[12], shadow_state[28], merkle_roots, etc.
Summary
geo_field is the geometric address space layer — it scatters arbitrary byte data onto a Goldberg icosahedral sphere, mapping 64B chunks to {tile_id, dim} addresses in FrustumBlocks (4896B each). It provides zoom (via gp_level 1..8), shape-routed access (Metatron ORBITAL/CHIRAL/CROSS/HUB), and bit-exact round-trip.

geopixel is the geometric pixel encoding layer — it converts addresses/indices into RGB pixels (GeoPixel: trit+spoke+coset+letter+fibo in 3 bytes), then classifies tiles into FLAT/GRADIENT/EDGE/NOISE for optimal codec selection (SEED/FREQ/ZSTD19). The hamburger encoder stores only the seed + invert chain, achieving O(1) random access on decode.

The diamond_shell operates between them at the chunk level — it rotates 64B as a 4x4x4 cube through 6 orientations, finds the geometrically best alignment via fold_fibo_intersect(), and packs as FLAT(2B)/SPARSE(9B)/DENSE(17B).

Maximum compression chain: Raw data → geo_field (geometric scatter) → diamond_shell (3D rotation + fibo_intersect encoding) → optional hamburger (.gpx5 tile codec). Each layer exploits a different aspect of geometric invariance in the data.