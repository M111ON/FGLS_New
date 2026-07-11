# pogls_tools — CLI Utilities

14 command-line tools for the POGLS pipeline.

## Tool Reference

| Tool | Purpose | Usage |
|------|---------|-------|
| pogls_compress | Compress tensor with ZSTD | pogls_compress input.bin output.pogls |
| pogls_decompress | Decompress tensor | pogls_decompress input.pogls output.bin |
| pogls_inspect | Show .pogls structure | pogls_inspect file.pogls |
| pogls_roundtrip | compress→decompress verify | pogls_roundtrip input.bin |
| pogls_build | Build .pogls from GGUF | pogls_build model.gguf output.pogls |
| pogls_cat | Concatenate .pogls files | pogls_cat a.pogls b.pogls out.pogls |
| pogls_diff | Diff two .pogls files | pogls_diff a.pogls b.pogls |
| pogls_verify | Verify .pogls integrity | pogls_verify file.pogls |
| pogls_test | Run test suite | pogls_test |
| addr_resolve | Name→address lookup | addr_resolve blk.0.attn_q.weight |
| gguf_dump | Dump GGUF structure | gguf_dump model.gguf |
| dramtile_dump | Dump DRamTile store | dramtile_dump store.bin |
| dramtile_bench | Benchmark DRamTile | dramtile_bench [count] |
| kv_delta_bench | Benchmark KV delta | kv_delta_bench [size] |

## Build

All tools compile against pogls_core.lib. See runner/Makefile for exact recipes.

```bash
gcc -O2 -std=c11 -Ipogls_core pogls_tools/pogls_inspect.c -lpogls_core -o pogls_inspect
```
