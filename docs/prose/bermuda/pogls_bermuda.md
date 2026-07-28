# pogls_bermuda.c

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `pogls_bermuda`  
**Path:** `pogls_bermuda/pogls_bermuda.c`  
**Status:** `active`  
**Note:** modified 16d ago  
**Generated:** 2026-07-28 10:24  

## Description

* pogls_bermuda.c — Implementation
* Stride-37 Hilbert routing with face traversal (orbit/chiral/cross/hub).
* Diamond Shell: 3D rotation + fibo_intersect rotation discriminator.
* RLE: run-length encoding with 3-byte minimum run.
* Shadow bond: FNV-1a keyed ring buffer (144 entries).
═══════════════════════════════════════════════════════════════════
INTERNAL HELPERS
═══════════════════════════════════════════════════════════════════
═══════════════════════════════════════════════════════════════════
ROUTING
═══════════════════════════════════════════════════════════════════
* pogls_bermuda_route: stride-37 routing from source address to target face.
* stride=1 → 512-slot gear, stride=2 → 1024, stride=3 → 2048, stride=4 → 4096.
* Face 0-11 selects the zone to route into.
* Uses orbit/chiral/cross/hub modes based on face group.
* pogls_bermuda_face_scores: computes distribution of stride-37 steps
* across all 12 zones, normalized to [0,1].
═══════════════════════════════════════════════════════════════════
DIAMOND SHELL — 3D rotation + fibo_intersect discriminator
═══════════════════════════════════════════════════════════════════

## Structures

- `typedef struct`

## API Functions

- `static uint32_t _popcount64(uint64_t x)`
- `static uint32_t _modinv37(uint32_t m)`
- `static uint32_t _walk_len(uint32_t slots)`
- `static uint32_t _hilbert_enc(uint32_t pos, uint32_t N)`
- `static uint32_t _hilbert_dec(uint32_t idx, uint32_t N, uint32_t inv37)`
- `uint32_t pogls_bermuda_route(uint32_t from, uint32_t face, uint32_t stride)`
- `return _hilbert_dec(new_enc, slots, inv37)`
- `void pogls_bermuda_face_scores(uint32_t base, float scores[12])`
- `static void _rotate64(uint8_t out[64], const uint8_t in[64], uint8_t rot)`
- `static void _inv_rotate64(uint8_t out[64], const uint8_t in[64], uint8_t rot)`
- `static void _build_quad_mirror(uint64_t quad[4], const uint8_t core[8])`
- `static uint64_t _fibo_intersect(const uint8_t rotbuf[64])`
- `static uint32_t _diamond_classify(const uint8_t chunk[64],`
- `uint32_t pogls_bermuda_diamond_compress(uint8_t *dst, size_t dst_cap,`
- `uint32_t pogls_bermuda_diamond_decompress(uint8_t *dst, size_t dst_cap,`
- `uint32_t pogls_bermuda_rle_compress(uint8_t *dst, size_t dst_cap,`
- `uint32_t pogls_bermuda_rle_decompress(uint8_t *dst, size_t dst_cap,`
- `static uint64_t _fnv1a(const char *s)`
- `static void _shadow_init(void)`
- `int pogls_bermuda_shadow_write(const char *key, const uint8_t *data, size_t sz)`

## Constants

- `#define RLE_MIN_RUN 3`

