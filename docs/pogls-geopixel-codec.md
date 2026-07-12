# geopixel-codec — Standalone Tile Codec

Independent Geopixel codec in `geopixel/`. Handles pixel-level encode/decode,
tile classification, GPX container formats, and Hilbert-curve spatial ordering.

## Architecture

```
input stream → [classify_tile] → FLAT/GRADIENT/EDGE/NOISE → [encode] → GPX5 blob
GPX5 blob → [decode] → [geo_pixel] → RGB pixels
```

## Files

### Core Codec — `include/geopixel/`

| File | Role |
|---|---|
| `geo_pixel.h` | Core encode/decode: integer index → RGB pixel with 5 decoded fields (trit, spoke, coset, letter, fibo). W=27 grid, O(1) all operations. |
| `geo_gpx_decode.h` | Standalone GPX2 decoder (411 lines, header-only). Decodes to BMP or RGB buffer. Depends on libpng + libzstd. |
| `geo_goldberg_tile.h` | Goldberg tile geometry integration for pixel pipeline. |
| `geo_tring_addr.h` | TRing address encoder for pixel-level addressing. |
| `geo_o4_connector.h` | O4 grid bridge connector for geopixel decode. |
| `gpx4_container.h` | GPX4 container format. |
| `gpx4_container_o22.h` | GPX4 + O22 GEO layer variant. |
| `gpr1_container.h` | GPR1 container format. |
| `geo_tring_walk.h` | TRing walk in geopixel namespace. |
| `geo_gpx_anim_o23.h` | GPX4 animated pattern support. |

### Hamburger Codec — `include/hamburger/`

| File | Role |
|---|---|
| `hamburger_classify.h` | Tile classifier (256 lines). Returns GPX5_TTYPE_FLAT/GRADIENT/EDGE/NOISE. Integer variance, no float. |
| `hamburger_encode.h` | Full encode/decode (1253 lines). Wraps H1 pipe dispatch + H2 invert recorder + LUT build. |
| `hamburger_pipe.h` | Pipe dispatch for Hamburger codec lanes. |
| `gpx5_container.h` | GPX5 container format. |
| `gpx5_hbhf.h` | GPX5 header-frame bridge. |

### HB Vault — `include/hbv/`

| File | Role |
|---|---|
| `hb_vault.h` | HB vault main header — deflate/inflate tile bundles. |
| `hb_tile_stream.h` | Tile stream reader/writer. |
| `hb_manifest.h` | Tile manifest/metadata. |
| `hb_header_frame.h` | Header frame for GPX5. |
| `goldberg_adj.h` | Goldberg adjacency tables. |
| `frustum_coset.h` | Frustum coset addressing. |
| `frustum_coord.h` | Frustum coordinate system. |
| `fibo_tile_dispatch.h` | Fibonacci tile dispatch. |
| `fibo_shell_walk.h` | Fibonacci shell walk. |
| `fibo_layer_header.h` | Fibonacci layer header. |
| `fibo_hb_wire.h` | Fibonacci HB wire protocol. |

### Source

| File | Role |
|---|---|
| `src/geopixel_v21_o25.c` | **Main codec implementation** (2302 lines). Full pipeline encode/decode with Goldberg integration, blob dedup via stamp hash, circuit-based classification, threaded atomics. |

### Bridge Files — `include/`

| File | Role |
|---|---|
| `pogls_to_geopixel.h` | Path A bridge: H64 → seed-only tiles (4B each, 16x ratio). |
| `bond_to_geopixel.h` | Bond layer ↔ GeoPixel bridge. V1/V2/V3. |
| `pogls_bond.h` | POGLS bond system. |
| `pogls_config.h` | POGLS configuration. |
| `pogls_hilbert64_encoder.h` | Hilbert 64-bit encoder. |
| `hex_tile.h` | Hex tile type definitions. |
| `geo_jump.h` | Geo jump header (jump curves). |
| `geo_frame_seek.h` | Frame seeking in geometric streams. |

## Tests

14 test binaries in `tests/`:

| Test | Scope |
|---|---|
| `test_hamburger` | Hamburger codec roundtrip |
| `test_hamburger2` | Hamburger variant |
| `test_tile_stream` | Tile stream read/write |
| `test_calibrate_bmp` | BMP calibration |
| `test_classify_thresh` | Classify threshold validation |
| `test_multicycle_hilbert` | Multi-cycle Hilbert test |
| `test_multicycle_stream` | Multi-cycle stream test |
| `test_hbhf_wire` | HB header-frame wire |
| `test_header_frame` | Header frame test |
| `test_frame_seek` | Frame seeking test |
| `codec_tile_test` | Tile codec roundtrip |
| `gpxtool` | GPX tool |
| `test_gear_proof` | Gear mechanism proof |
| `test_pipeline_proof` | Pipeline proof-of-concept |

## Build

```bash
cd geopixel
make                    # build all tests + demos
make check              # run all 14 tests
make demos              # demo programs
make clean
```

Requires: zstd, libpng, zlib.

## Related

- `pogls-geopixel` (runner integration) — v3 FrameStore block compression
- `docs/pogls-geopixel.md` — runner geopixel API
- `geopixel/tools/` — Python encode/decode scripts
