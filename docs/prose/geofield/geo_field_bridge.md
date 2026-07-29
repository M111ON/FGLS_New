# geo_field_bridge.c

> 🟡 **[STALE]** — This file may be inactive or pending removal. Check before relying on it.

**Module:** `collection`  
**Path:** `collection/geopixel/geofield/geo_field_bridge.c`  
**Status:** `stale`  
**Note:** last modified 65d ago  
**Generated:** 2026-07-29 17:34  

## Description

* geo_field_bridge.c — Geofield → JSON Bridge for Python Bond Layer
* ═══════════════════════════════════════════════════════════════════════
* Compile:
*   gcc -O2 -I. -I../../geofield -o build/geo_field_bridge geo_field_bridge.c
* Usage:
*   build/geo_field_bridge <file> [gp_level=2]
* Output: JSON to stdout
*   {
*     "file":        "<path>",
*     "size":        <bytes>,
*     "gp_level":    <level>,
*     "chunks":      <total_64B_chunks>,
*     "blocks":      <frustum_blocks>,
*     "zone_resets": <count>,
*     "skeleton":    { "ID":<n>, "FLAT":<n>, "DIFF":<n>, "BREF":<n>, "GEOM":<n>, "RAW":<n> },
*     "roundtrip":   <"PASS"|"FAIL">,
*     "topology":    { "content_type": "binary", "best_scale": 64, "routing_hint": "geometric" }
*   }
* ═══════════════════════════════════════════════════════════════════════
══════════════════════════════════════════════════════════════

## API Functions

- `static uint64_t fnv64(const uint8_t *data, size_t len)`
- `static void print_json_escaped(const char *s)`
- `else if (*s == '"') putchar('\\'), putchar('"')`
- `else putchar(*s)`
- `static void print_json(const char *fpath,`
- `int main(int argc, char **argv)`

## Constants

- `#define FNV_OFFSET 0xCBF29CE484222325ULL`
- `#define FNV_PRIME  0x00000100000001B3ULL`

