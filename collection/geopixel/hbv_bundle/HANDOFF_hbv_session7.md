# HBV Session 7 Handoff

## Session goal
Wire `geo_diamond_to_scan.h` (DiamondBlock → ScanEntry bridge) and
`fibo_hb_wire.h` (FiboTile → Hamburger encode/decode) into a unified
compile-clean pipeline. Resolve all dependency gaps from sessions 5–6.

## Status: COMPLETE

### Full pipeline verified
```
raw bytes → DiamondBlock → ScanEntry → GpxTileInput → FiboHbResult
                                                     ↕ encode/decode
                                              roundtrip PASS (lossless)
```

### Test results (test_full_pipeline.c)
```
encode: OK  ttype=GRAD codec=4 enc_sz=195 tile_id=211
decode: OK
roundtrip: PASS
W04 YCgCo roundtrip: PASS
tile_id: 211 = face(3)×60 + edge(2)×12 + z(7)%12  ✓
```

### Dependencies resolved this session
| Missing | Found in |
|---------|----------|
| `geo_diamond_field.h` | core.zip / pogls_engine/twin_core |
| `pogls_scanner.h` | core.zip / geo_headers |
| `gpx5_container.h` | hbv_session6_bundle.zip |
| `hamburger_classify.h` | hbv_session5_bundle.zip |
| `hamburger_encode.h` | hbv_session5_bundle.zip |
| `fibo_tile_dispatch.h` | hbv_session5_bundle.zip |

### ScanEntry fix
`geo_diamond_to_scan.h` originally defined its own 20B ScanEntry —
conflicted with canonical 64B ScanEntry in `pogls_scanner.h`.
Fixed: removed local struct definition, include `pogls_scanner.h` instead.
Field mapping: `reserved[0]` = field_id (D2S_FIELD_SPATIAL/TEMPORAL/CHROMA/GHOST).

### Key constants (frozen)
- tile_id = face×60 + edge×12 + (z%12) → 0..719 (sacred 720)
- FHW_TILE_PX = 64, FHW_RAW_SZ = 384B (int16le YCgCo)
- YCgCo: JFIF lossless lifting — exact roundtrip all 8-bit RGB

## Next session
- Run D01–D08 invariants (diamond_to_scan invariants)
- Run W01–W06 invariants (fibo_hb_wire invariants)
- Wire diamond_scan_stream → fibo_hb_encode_tile batch path
- Test with real BMP input (test_real_bmp.c from session5)

## Bundle contents
| File | Description |
|------|-------------|
| `geo_diamond_to_scan.h` | DiamondBlock → ScanEntry bridge (session 7 primary) |
| `fibo_hb_wire.h` | FiboTile → Hamburger encode/decode wire (session 7 primary) |
| `frustum_coord.h` | Frustum coordinate types |
| `geo_tring_walk.h` | TRing walk utilities |
| `gpx5_container.h` | GeoPixel5 container format |
| `gpx5_hbhf.h` | GPX5 hamburger format header |
| `hamburger_pipe.h` | Hamburger pipeline orchestration |
| `hb_header_frame.h` | HB frame header |
| `hb_manifest.h` | HB file manifest |
| `hb_tile_stream.h` | HB tile stream reader/writer |
| `hb_vault.h` | HB vault container |
| `fibo_shell_walk.h` | Fibonacci shell walk |

## Compile command (session 8 reference)
```bash
gcc -I<bundle_dir> -I<core_dir> -O2 \
    test_full_pipeline.c pogls_fold.c -o test_full
```
