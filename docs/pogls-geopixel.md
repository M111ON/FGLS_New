# pogls_geopixel — Pixel-Level Block Compression

64-byte block classifier: FLAT(2B), SMOOTH/GRADIENT(10B), EDGE(64B raw).

## Block Encoder

Splits input into 64-byte blocks and classifies each:

- **FLAT** (`0x00`): All 64 bytes identical → stored as `[tag][value]` = 2 bytes.
- **SMOOTH** (`0x01`): Near-constant — each byte within ±16 of the block mean → stored as `[tag][mean][8×residual]` = 10 bytes.
- **GRADIENT** (`0x02`): Piecewise-linear trend — bytes follow a slope + intercept with small residuals → stored as `[tag][slope][intercept][7×residual]` = 10 bytes.
- **EDGE** (`0x03`): High entropy / no pattern → stored as `[tag][64 raw bytes]` = 65 bytes (no compression, but self-describing).

## Full Tensor Encode

Iterator that splits a tensor into 64-byte blocks (zero-pads the final partial block), encodes each via `pogls_geopixel_encode_block`, and concatenates the tagged substreams. Decoder dispatches on the tag byte per block and reconstructs to the original size (rounded up to multiple of 64).

## Hilbert 2D Curve

8×8 block ordering via 8-bit Hilbert curve (order=3). Functions:

- `pogls_geopixel_hilbert_xy_to_d(x, y, order)` — 2D coordinates → 1D Hilbert index.
- `pogls_geopixel_hilbert_d_to_xy(d, order, &x, &y)` — 1D Hilbert index → 2D coordinates.

Preserves spatial locality: nearby (x,y) map to nearby d indices.

## Session Feed

Incremental frame counter (`frame_seq`), block accumulation (`block_count`), raw/compressed byte totals. Use `pogls_geopixel_session_init` to start, `pogls_geopixel_session_feed` to encode a tensor and update counters, and `pogls_geopixel_session_stats` to print summary.

## API Reference

| Function | Purpose |
|---|---|
| `pogls_geopixel_encode_block` | Encode one 64B block → tagged stream |
| `pogls_geopixel_decode_block` | Decode one tagged block → 64B |
| `pogls_geopixel_encode` | Encode full tensor (iterator over 64B blocks) |
| `pogls_geopixel_decode` | Decode full tagged stream |
| `pogls_geopixel_hilbert_xy_to_d` | 2D → 1D Hilbert index |
| `pogls_geopixel_hilbert_d_to_xy` | 1D Hilbert index → 2D |
| `pogls_geopixel_session_init` | Init session with expected block count |
| `pogls_geopixel_session_feed` | Feed tensor, accumulate stats |
| `pogls_geopixel_session_stats` | Print session statistics |

## Usage

```c
uint8_t src[128] = {0};
uint8_t dst[256];
uint32_t sz = pogls_geopixel_encode(dst, sizeof(dst), src, sizeof(src));
uint8_t dec[128];
pogls_geopixel_decode(dec, sizeof(dec), dst, sz);
```
