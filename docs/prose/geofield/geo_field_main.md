# geo_field_main.c

> 🟡 **[STALE]** — This file may be inactive or pending removal. Check before relying on it.

**Module:** `collection`  
**Path:** `collection/geopixel/geofield/geo_field_main.c`  
**Status:** `stale`  
**Note:** last modified 66d ago  
**Generated:** 2026-07-30 01:40  

## Description

* geo_field_main.c — GeoField Demo Program
* ═══════════════════════════════════════════════════════════════════════
* Demonstrations:
*   1. COMPLETE LOOP — encode → save → load → decode → verify
*   2. SCALE — zoom in/out by changing gp_level
*   3. SHAPE DIMENSION ACCESS — Metatron routing between pentagon faces
* Build:
*   See Makefile for include paths and linker flags.
* Usage:
*   geo_field_demo
* ═══════════════════════════════════════════════════════════════════════
═══════════════════════════════════════════════════════════════════════
DEMO 1 — Complete Loop Roundtrip
═══════════════════════════════════════════════════════════════════════
Encode synthetic data → decode → compare. Verifies bit-exact.
═══════════════════════════════════════════════════════════════════════
═══════════════════════════════════════════════════════════════════════
DEMO 2 — Scale (Zoom In/Out)
═══════════════════════════════════════════════════════════════════════
Show face_count changes across gp_levels + multi-res encode.

## API Functions

- `static int demo_complete_loop(void)`
- `else if (ret == -3)`
- `static void demo_scale(void)`
- `static void demo_shape_access(void)`
- `static int demo_file_roundtrip(void)`
- `int main(void)`

## Constants

- `#define DEMO_SZ 8192`
- `#define MR_SZ 4096`

