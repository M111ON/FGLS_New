# pogls_decompress.c

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `pogls_tools`  
**Path:** `pogls_tools/pogls_decompress.c`  
**Status:** `active`  
**Note:** modified 17d ago  
**Generated:** 2026-07-29 11:03  

## Description

* pogls_decompress.c — Decompress file → raw data
* Usage: pogls_decompress <input> <output>
* Reads [4B comp_type][4B orig_sz][4B comp_sz][data] format.
* Decompresses (or copies for RAW) to output file.

## API Functions

- `static void usage(const char *prog)`
- `int main(int argc, char **argv)`

