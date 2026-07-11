# pogls_bermuda — Routing + Compression Layer

A standalone pure-C geometry router and compression library for the POGLS 20736-address icosahedral space. Four subsystems in one header+source:

- **Stride-37 Routing** — Hilbert-curve-based address routing across 12 icosahedral zones
- **Diamond Shell** — 4×4×4 cube codec with fibo_intersect rotation discriminator
- **RLE Compression** — run-length encoding with 3-byte minimum run threshold
- **Shadow Bond** — FNV-1a keyed ring buffer for ephemeral key-value storage

No external dependencies beyond `<stdint.h>`, `<stddef.h>`, `<string.h>`. All integer arithmetic on hotpath — no floating-point for route or compression.

## Constants

| Constant | Value | Description |
|---|---|---|
| `POGLS_BERMUDA_MAX_ADDR` | 20736 | Total address space (144²) |
| `POGLS_BERMUDA_STRIDE` | 37 | Stride for Hilbert curve walk |
| `POGLS_BERMUDA_N_ZONES` | 12 | Number of icosahedral zones |
| `POGLS_BERMUDA_CHUNK_SZ` | 64 | Diamond Shell chunk size (4×4×4) |
| `POGLS_BERMUDA_ROT_STATES` | 6 | Cubic rotation orientations |
| `POGLS_BERMUDA_FLAG_FLAT` | 0 | All-zero chunk marker |
| `POGLS_BERMUDA_FLAG_DENSE` | 2 | Non-zero chunk marker |
| `POGLS_BERMUDA_SHADOW_CAP` | 144 | Shadow bond ring buffer capacity |

## Stride-37 Routing

Address routing over the 20736-position icosahedral space using a stride-37 Hilbert curve.

**Algorithm:**
1. Pick `slots` from stride: 512 (stride 1), 1024 (stride 2), 2048 (stride 3), or 4096 (stride 4)
2. Compute `walk_len` = smallest multiple of 12 >= slots, coprime to 37 (ensures full zone coverage)
3. Encode `from` via stride-37 multiplication: `enc = (from * 37) % walk_len`
4. Decompose into `zone = enc / (walk_len / 12)` and `offset = enc % (walk_len / 12)`
5. Map to target zone via face-dependent traversal mode
6. Decode via modular inverse of 37: `result = (new_enc * inv37) % slots`

### Four Traverse Modes

| Mode | Face Group | Behavior |
|---|---|---|
| **ORBIT** | faces 0–2 | Step forward by `1 + sub` zones |
| **CHIRAL** | faces 3–5 | Jump by `6 + sub * 2` zones (hemisphere flip) |
| **CROSS** | faces 6–8 | Cross-hemisphere mirror via CROSS LUT `[9,10,11,6,7,8,3,4,5,0,1,2]` |
| **HUB** | faces 9–11 | Pole routing: sub < 2 → south pole (zone 0), else north pole (zone 6) |

### Four Gear Levels

| stride | slots | Use case |
|---|---|---|
| 1 | 512 | Local neighborhood routing |
| 2 | 1024 | Zone-level routing |
| 3 | 2048 | Region-level routing |
| 4 | 4096 | Global topology routing |

### Face Scores

`pogls_bermuda_face_scores()` computes the stride-37 step distribution across all 12 zones for a given base address. Walks 12 consecutive addresses, Hilbert-encodes each, tallies zone hits, and normalizes to `[0, 1]` floats. Used for zone congestion analysis and routing decisions.

## Diamond Shell

A 4×4×4 cube codec that compresses 64-byte chunks by finding the optimal 3D rotation to maximize bit-plane alignment.

### Encoding

1. **Zero check**: if the entire 64-byte chunk is zero, emit 2 bytes `[FLAG=0][rot=0]`
2. **Rotation sweep**: try all 6 `_rotate64()` orientations, each time:
   - Apply 3D axis permutation: `(x,y,z)` remapped per rotation state
   - Run `_fibo_intersect()`: build 4 mirrored quad-planes from the first 8 bytes, AND them together → 64-bit intersection mask
   - Count popcount of the intersection: higher = better alignment
3. **Pick best rotation** (highest intersection popcount)
4. **Emit**: `[FLAG=2][best_rot][data:64]` = 66 bytes total

### Rotation Orientations

| rot | Mapping |
|---|---|
| 0 | Identity: `(x,y,z)` |
| 1 | `(y,z,x)` — cyclic X→Y |
| 2 | `(z,x,y)` — cyclic Y→Z |
| 3 | `(x,z,3-y)` — X-major mirror |
| 4 | `(z,y,3-x)` — Z-major mirror |
| 5 | `(3-y,x,z)` — Y-major mirror |

### Decoding

1. Read `[flag][rot]` header (2 bytes)
2. If `FLAT`: memset 64 zero bytes
3. If `DENSE`: read 64 bytes, apply `_inv_rotate64()` with the stored rotation

### Compression Ratio

- All-zero chunks: 2 bytes (32× compression vs 64 raw)
- Non-zero chunks: 66 bytes (3% overhead vs 64 raw)
- Best-case: highly patterned data with strong axis alignment

## RLE Compression

Portable run-length encoding with 3-byte minimum run threshold.

### Format

```
[literal_count:2][literals...][run_marker:2=0][run_value:1][run_length:2]...
```

- `literal_count`: 16-bit LE, number of literal bytes following
- `run_marker`: `0x0000` signals a run
- `run_value`: byte value to repeat
- `run_length`: 16-bit LE, repetition count
- Runs shorter than 3 bytes are emitted as literals

### Limitations

- Maximum literal segment: 65535 bytes
- Maximum run length: 65535 repetitions
- Not designed for incompressible data (expands by 2-byte header per segment)

## Shadow Bond

An FNV-1a keyed ring buffer for ephemeral key-value storage. 144 entries, each holding a 32-byte key and 64-byte value.

### Operations

- **Write**: FNV-1a hash the key → slot index (mod 144). Overwrite existing entry at that slot unconditionally. 64 bytes max per value.
- **Read**: FNV-1a hash the key → slot index. Verify `used` flag and `strcmp` key match. Returns 0 on miss.
- **Collision**: hash collisions overwrite the colliding key. No chaining — first-write-wins semantics do not apply.

## API Reference

### Stride-37 Routing

```c
uint32_t pogls_bermuda_route(uint32_t from, uint32_t face, uint32_t stride);
```
Route address `from` (mod 20736) to `face` (0–11) with gear `stride` (1–4). Returns target address in `[0, slots-1]`.

```c
void pogls_bermuda_face_scores(uint32_t base, float scores[12]);
```
Compute stride-37 zone distribution for `base` address, normalized to `[0, 1]`.

### Diamond Shell Compression

```c
uint32_t pogls_bermuda_diamond_compress(uint8_t *dst, size_t dst_cap,
                                        const uint8_t *src, size_t src_sz);
```
Compress `src_sz` bytes from `src` into `dst`. Processes in 64-byte chunks. Each chunk written as `[flag:1][rot:1][data:64]` for non-zero, or `[flag:1][rot:1]` for all-zero. Returns total bytes written, or 0 on dst_cap overflow.

```c
uint32_t pogls_bermuda_diamond_decompress(uint8_t *dst, size_t dst_cap,
                                          const uint8_t *src, size_t src_sz);
```
Reverse of compress. Returns bytes written to dst, or 0 on error.

### RLE Compression

```c
uint32_t pogls_bermuda_rle_compress(uint8_t *dst, size_t dst_cap,
                                    const uint8_t *src, size_t src_sz);
```
Run-length encode src into dst. Format: `[literal_count:2][literals...][0x0000][value][run_length:2]...`. Returns bytes written, or 0 on overflow.

```c
uint32_t pogls_bermuda_rle_decompress(uint8_t *dst, size_t dst_cap,
                                      const uint8_t *src, size_t src_sz);
```
Reverse of RLE compress. Returns bytes written to dst, or 0 on error.

### Shadow Bond

```c
int pogls_bermuda_shadow_write(const char *key, const uint8_t *data, size_t sz);
```
Write `sz` bytes (max 64) keyed by `key` (max 31 chars). Returns 1 on success.

```c
int pogls_bermuda_shadow_read(const char *key, uint8_t *data, size_t cap);
```
Read data for `key`. Returns 1 on hit, 0 on miss.

## Usage

```c
#include "pogls_bermuda.h"
#include <stdio.h>

int main(void) {
    /* Diamond Shell: compress 64 bytes */
    uint8_t src[64] = {0};
    for (int i = 0; i < 64; i++) src[i] = (uint8_t)(i * 17 + 31);

    uint8_t enc[256];
    uint32_t enc_sz = pogls_bermuda_diamond_compress(enc, sizeof(enc), src, sizeof(src));
    printf("compressed %zu -> %u bytes\n", sizeof(src), enc_sz);

    uint8_t dec[64];
    uint32_t dec_sz = pogls_bermuda_diamond_decompress(dec, sizeof(dec), enc, enc_sz);
    printf("decompressed -> %u bytes, %s\n", dec_sz,
           memcmp(src, dec, sizeof(src)) == 0 ? "OK" : "MISMATCH");

    /* Route: stride-37 address routing */
    uint32_t routed = pogls_bermuda_route(42, 3, 2);
    printf("route(42, face 3, stride 2) = %u\n", routed);

    /* Shadow bond: ephemeral key-value */
    uint8_t val[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    pogls_bermuda_shadow_write("my-key", val, 4);
    uint8_t readback[4];
    if (pogls_bermuda_shadow_read("my-key", readback, 4))
        printf("shadow read: %02x %02x %02x %02x\n",
               readback[0], readback[1], readback[2], readback[3]);

    return 0;
}
```

## Building

```sh
gcc -O2 -std=c11 -I. test_pogls_bermuda.c pogls_bermuda.c -o test_pogls_bermuda.exe
```

## Test Suite

14 tests covering all subsystems:

| Group | Tests | Coverage |
|---|---|---|
| Stride-37 Routing | 3 (basic, deterministic, edge) | All faces, all strides, boundary addresses, deterministic roundtrip |
| Face Scores | 1 | Sum to 1.0, non-negative, base-dependent |
| Diamond Shell | 4 (all-zero, pattern, multi-chunk, mixed) | Zero-only, dense pattern, 3-chunk, mixed zero/pattern |
| RLE | 4 (uniform, random, mixed, edge) | Uniform run, random data, mixed runs+literals, single/two-byte/empty |
| Shadow Bond | 1 | Write+read, wrong-key miss, overwrite |
