# pogls_verify.c

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `pogls_tools`  
**Path:** `pogls_tools/pogls_verify.c`  
**Status:** `active`  
**Note:** modified 17d ago  
**Generated:** 2026-07-29 19:44  

## Description

* pogls_verify.c — Verify POGLS file integrity
* Usage: pogls_verify <file.pogls>
* Checks: magic, header, tensor meta validity, data section alignment.
* Reports PASS/FAIL per check.

## API Functions

- `static void usage(const char *prog)`
- `int main(int argc, char **argv)`

## Constants

- `#define CHECK(name, cond, ...) do { \`

