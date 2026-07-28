# pogls_build.c

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `pogls_tools`  
**Path:** `pogls_tools/pogls_build.c`  
**Status:** `active`  
**Note:** modified 16d ago  
**Generated:** 2026-07-28 10:23  

## Description

* pogls_build.c — One-shot GGUF → POGLS build (wrapper)
* Usage: pogls_build <model.gguf> <output.pogls> [--compress] [--verify]
* Wraps the existing gguf_to_pogls pipeline:
*   1. Build POGLS from GGUF
*   2. Optionally verify the output

## API Functions

- `static void usage(const char *prog)`
- `static int build_pogls(const char *gguf_path, const char *out_path, int compress)`
- `static int verify_file(const char *path)`
- `int main(int argc, char **argv)`
- `else if (strcmp(argv[i], "--verify") == 0) verify = 1`
- `else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)`
- `else if (!gguf) gguf = argv[i]`
- `else if (!out) out = argv[i]`

