# Hilbert L-Block Container — Value Proposition

## Definition

> L-block เปลี่ยนหน่วยพื้นฐานของไฟล์จาก "byte chunk" เป็น "topology-aware container" ที่รู้ทั้งตำแหน่ง เพื่อนบ้าน และจุดเชื่อม ทำให้ทุกขั้นตั้งแต่ indexing, caching, prediction ไปจนถึง codec ใช้โครงสร้างเดียวกันได้

## What L-block actually provides (corrected)

| Value | From L-block directly? | Notes |
|---|---|---|
| Locality / Cache-friendly | ✅ Yes | 3-cell L-blocks are co-accessed units → prefetch, cache hit, SIMD batching, fewer page faults. MORE valuable than compression. |
| Connector graph for prediction | ✅ Yes | Floor/block connectors create A-B/C-D dependency structure → gradient, delta, predictive codecs work. Strongest architectural argument. |
| Shared node_id all stages | ✅ Yes | Universal address language across Bond/Shell/Pixel stages. |
| O(1) access | ⚠️ Deterministic indexing (RDH/geo_jump) | L-block benefits from this, doesn't create it. |
| Compression 32x | ❌ FLAT codec | FLAT compresses zeros. L-block organizes data. Separate credits. |

## What L-block does NOT provide

- **O(1) access**: That's deterministic indexing via bitmap popcount (RDH, geo_jump). L-block benefits from it.
- **Compression**: FLAT codec handles zero compression (32x per 64B chunk). L-block groups data, doesn't compress.
- **New functionality**: Pipeline stages (Bond, Shell, Pixel) already exist. L-block adds spatial structure they can exploit.

## Why Locality matters more than compression

Compression ratio is a static metric — it tells you file size. Locality is a dynamic metric — it affects:
- **Prefetch**: Adjacent L-block cells on Hilbert curve are spatially nearby → prefetch next block while processing current
- **Cache hit**: 3-cell L-block fits in L1/L2 cache line → all 3 cells loaded together
- **SIMD batching**: L-block cells can be processed in parallel via SIMD (same operation on 3 cells)
- **Page fault reduction**: mmap pages contain full L-blocks → fewer page faults during random access

## Connector graph — strongest architectural argument

Without connectors:
```
Block A    Block B    Block C
(standalone) (standalone) (standalone)
```
Each block is isolated → no cross-block prediction → codec只能用 intra-block patterns.

With connectors:
```
A ---- B
|      |
C ---- D
```
Dependency graph → codec can use inter-block patterns → gradient/delta prediction across boundaries.

## Implementation

Files:
- `runner/pogls_hilbert_container/pogls_hilbert_container.h` — standalone (87 tests)
- `runner/pogls_hilbert_container/pogls_hc_geojump.h` — GeoJump integration (220 tests)
- `runner/pogls_hilbert_container/test_pogls_hilbert_container.c`
- `runner/pogls_hilbert_container/test_pogls_hc_geojump.c`

Pipeline insertion point:
```
Input → Chunk(64B) → [L-block Container] → Bond → Shell → Pixel → Hamburger → GPX5
```
