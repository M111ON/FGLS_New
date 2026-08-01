# FGLS Extraction Bug Analysis

## Executive Summary

The extraction produces a 609.8MB file with magic=0x00000000 because the **v1 archive format did not store the GGUF header**, and the **v1 extract code read from wrong offsets**. The v2 code on disk fixes both issues, but must be recompiled and a fresh archive must be created with v2.

---

## BUG 1 (CRITICAL): GGUF header not stored in v1 archive body

**File:** `fgls_archive.c` (v1)
**Impact:** Extraction output has magic=0x00000000, all metadata lost

The v1 writer body layout was:
```
[FGLS_Header 64B] [TensorTable] [Body: tensor data only]
```

The v1 writer NEVER stored the original GGUF header (bytes 0..orig_data_start)
containing magic, version, metadata KV pairs, and tensor descriptions.

The v1 extract code allocated `calloc(1, hdr.orig_size)` (zeroed buffer) and
only wrote tensor data blocks — never the GGUF header. Result: first bytes
are 0x00000000 instead of 0x46554747 ("GGUF").

**v2 fix (fgls_archive.c lines 147-154):**
```c
/* Body: first the original GGUF header */
uint8_t *gguf_hdr = (uint8_t*)malloc(gf->tensor_data_start);
fseek(fp, 0, SEEK_SET);
fread(gguf_hdr, 1, gf->tensor_data_start, fp);
fwrite(gguf_hdr, 1, gf->tensor_data_start, fo);
free(gguf_hdr);
```

**v2 fix (fgls_extract.c lines 77-78):**
```c
fseek(fi, table_end, SEEK_SET);
fread(out, 1, hdr.orig_data_start, fi);  /* copy GGUF header to output */
```

---

## BUG 2 (CRITICAL): Wrong seek offsets in v1 extract reader

**File:** `fgls_extract.c` (v1)
**Impact:** All tensor data read from wrong archive positions → corrupted weights

In v1, `arch_offset` was relative to tensor data start (0-indexed from first
tensor). But the v1 reader computed body start as just `header_area_size`
(FGLS_HEADER_SZ + tensor table), then seeked to:
```
header_area_size + arch_offset
```

This was WRONG because even in v1, the body didn't have the GGUF header,
but the `arch_offset` was already correct (relative to tensor data). The real
issue was that the v1 code had NO concept of the GGUF header being in the body,
so when v2 added it, the offset became:
```
table_end + orig_data_start + arch_offset
```

The v1 code's `header_area_size` calculation was:
```c
uint64_t header_area_size = FGLS_HEADER_SZ;
for (uint32_t t = 0; t < hdr.n_tensors; t++)
    header_area_size += sizeof(FGLS_TensorEntry) + entries[t].name_len;
```

This is the file offset where the body STARTS. In v1, the body started with
tensor data directly, so `header_area_size + arch_offset` was correct for v1.
But in v2, the body starts with the GGUF header, so the seek must add
`hdr.orig_data_start`.

**v2 fix (fgls_extract.c lines 91, 98):**
```c
fseek(fi, table_end + hdr.orig_data_start + entries[t].arch_offset, SEEK_SET);
```

---

## BUG 3 (MODERATE): fseek on closed FILE* in v1 writer

**File:** `fgls_archive.c` (v1, around line 216)
**Impact:** Undefined behavior; header rewrite may silently corrupt or fail

The v1 writer pattern was:
```c
fclose(fo);          // line 212
fclose(fp);          // line 213
fseek(fo, 0, SEEK_SET);  // line 216 — UB! fo is closed
FILE *fup = fopen(fout, "r+b");  // line 217 — correct workaround
```

Operating on a closed FILE* is undefined behavior. The v1 code was "saved" by
the correct reopen on line 217, but line 216 itself is UB and should be removed.

**v2 fix (fgls_archive.c lines 196-203):**
```c
/* Rewrite header BEFORE closing — no fclose/fopen needed */
struct stat st;
stat(fout, &st);
hdr.kept_weights = kept_total;
hdr.arch_size = (uint64_t)st.st_size;
fseek(fo, 0, SEEK_SET);     // fo is still open here — correct
fwrite(&hdr, 1, FGLS_HEADER_SZ, fo);
fclose(fo);
```

---

## BUG 4 (MINOR): FGLS_HEADER_SZ=64 but sizeof(FGLS_Header)=72

**File:** `fgls_archive.h`
**Impact:** 8 bytes of `reserved[16]` truncated on every write/read

The packed struct is 72 bytes:
```
offset  field           size
0       magic           4
4       version         4
8       n_tensors       4
12      n_baked         4
16      orig_size       8
24      arch_size       8
32      orig_data_start 8
40      kept_weights    8
48      total_weights   8    ← last field fully within 64 bytes
56      reserved        16   ← only first 8 bytes written (56-63)
64      (truncated)     8    ← reserved[8..15] LOST
```

`total_weights` (offset 48-55) IS within the 64-byte write, so it's preserved.
Only `reserved[8..15]` is lost. Since reserved is for future use, this is not
a correctness bug but is fragile — any new field added after total_weights
will silently break.

**Fix:** Change `#define FGLS_HEADER_SZ 64` to `#define FGLS_HEADER_SZ 72`
or use `sizeof(FGLS_Header)` everywhere.

---

## BUG 5 (MINOR): stat() on unflushed file in v2 writer

**File:** `fgls_archive.c` (v2, line 198)
**Impact:** `arch_size` in header may be slightly wrong (off by up to buffer size)

```c
stat(fout, &st);                // queries OS, but C runtime buffers not flushed
hdr.arch_size = (uint64_t)st.st_size;  // may miss unflushed tail bytes
```

The `stat()` call queries the filesystem, but `fo`'s C runtime buffer hasn't
been flushed yet. On Windows with MSYS, the OS may report a size that doesn't
include the last up-to-buffer-size bytes. This makes `hdr.arch_size` slightly
too small.

**Impact is cosmetic:** `arch_size` is informational, not used by the extract
code for any computation.

**Fix:** Add `fflush(fo);` before `stat(fout, &st);`

---

## BUG 6 (DOCUMENTATION): arch_offset comment says "relative to body start"

**File:** `fgls_archive.h` line 53
**Impact:** Misleading for future code authors

```
uint64_t arch_offset;  /* byte offset in archive body (relative to body start) */
```

Actual semantics: relative to **tensor data start** (after the GGUF header
prefix in the body). The format comment on line 63 is correct:
```
[tensor data: per-tensor offsets are relative to this point]
```

**Fix:** Change line 53 comment to:
```
/* byte offset in archive body tensor data section (after GGUF header) */
```

---

## Root Cause of the Reported Failure

The extraction outputs magic=0x00000000 because **the archive was created with
the v1 writer** (which doesn't store the GGUF header), and the v1 extract
code was used. Two compounding failures:

1. **No GGUF header in archive body** (Bug 1) — the v1 writer stored only
   tensor data, not the GGUF metadata. The extract code has nothing to
   restore for bytes 0..orig_data_start.

2. **Extract never restores GGUF header** (Bug 1, reader side) — the v1
   reader only processes tensor entries; it never reads or writes the GGUF
   header region. The output buffer stays zeroed at offset 0.

The "28% of weights restored" figure reflects that tensor data IS placed at
correct positions in the output (the v1 seek logic `header_area_size +
arch_offset` was correct for v1's body layout), but the output file fails
magic validation because the GGUF header is all zeros. llama.cpp won't load
it.

**To reproduce the fix:** the v2 code on disk fixes both issues, but must be:
1. **Recompiled** — no .exe binaries exist in the workspace
2. **Run on a fresh archive** — no .fgls files exist; a new archive must be
   created with the v2 writer from the original GGUF
