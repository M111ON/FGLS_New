# pogls_compress.c

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `pogls_tools`  
**Path:** `pogls_tools/pogls_compress.c`  
**Status:** `active`  
**Note:** modified 17d ago  
**Generated:** 2026-07-29 09:11  

## Description

* pogls_compress.c — Compress raw data → ZSTD-compressed file
* Usage: pogls_compress <input> <output> [-l level]
* Reads raw bytes, compresses with ZSTD, writes output.
* If compression ratio < 1.10x, stores raw (no compression).

## API Functions

- `static void usage(const char *prog)`
- `int main(int argc, char **argv)`

