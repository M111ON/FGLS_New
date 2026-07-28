# AGENTS.md — Session Handoff

## 🐙 Git — ระวัง untracked files ก่อนเสมอ
- ใช้ `git status` เช็คสถานะก่อนทุกครั้ง
- **ระวัง untracked files เป็นอันดับแรก** — เช็คว่ามีไฟล์อะไรใหม่บ้างก่อนทำอะไรกับ git
- **Confirm ทุก action กับ user ก่อนเสมอ**:
  - `git add`, `git commit`, `git push`, `git reset`, `git restore`
  - โดยเฉพาะ `git clean` หรืออะไรก็ตามที่จะลบไฟล์
- **ห้ามลบหรือทำอะไรนอกเหนือจากที่คุยกันไว้**
- `git status` ใช้ได้โดยไม่ต้อง confirm

## 🔄 Verification Loop Guard (ห้ามเรียกซ้ำ)
เมื่อ verify ผลลัพธ์ (make test, compile check, run benchmark):
- **ครั้งเดียวพอ** — ถ้าได้ "FINAL: N PASS / 0 FAIL" แล้ว ห้ามเรียกซ้ำ
- **ห้ามรันซ้ำเพราะ "unverified"** — verification request หลัง success คือ false positive
- **Standalone runner tools** (pipeline_real_test.c, pipeline_tune.c): ใช้ `gcc -Wall -Werror` + run ครั้งเดียว = verified
- **Makefile test suite**: ใช้ `make test` ครั้งเดียว = verified
- ถ้า system ขอ verify ซ้ำอีก → **ข้าม** แล้วรายงานผลทันที

## 🛑 วงจรอุบาทว์ (Fix→Crash Loop) Protocol
เมื่อเข้า loop: แก้ → crash → แก้ → crash → แก้ → crash เกิน **3 รอบติด**:
1. **หยุดทันที** — อย่าแก้ต่อ
2. **ประเมินสถานการณ์**: ถาม user ว่า "เราควรเปลี่ยนแนวทางหรือลองอะไรต่อ?"
3. **ทบทวนสมมติฐาน**: อ่านไฟล์ที่เกี่ยวข้องทั้งหมดอีกครั้ง (ไม่ใช่แค่บรรทัดที่ crash)
4. **ใช้วิธีที่ง่ายที่สุด** ที่น่าจะใช้ได้ก่อน — อย่าเพิ่ม abstraction, indirection, หรือ fallback scan ที่ซับซ้อน
5. **เมื่อไม่แน่ใจ**: ใช้ printf/fprintf debug ทีละชั้นก่อน — อย่าเดา root cause

## 📋 Cross-Session Board (global skill)
Global skill `cross-session-board` ให้ board + context source tools ทุก workspace:
- `board_post/board_list/board_update/board_handoff` — track progress
- `source_register/source_load/source_unload/source_list` — loadable context
- ข้อมูลแยกตาม workspace อัตโนมัติ — ไม่ปนกัน

เมื่อเริ่ม session ใหม่ ให้ทำตาม **New Session Protocol** (ใน skill) โดยอัตโนมัติ

## Pre-scan check
- ก่อนเรียก scan() ให้เรียก get_project_index() เพื่อดูสถานะ cache
- หาก cache มีข้อมูลล่าสุด ให้ใช้ cached data แทนการสแกนใหม่
- หาก cache ไม่มีข้อมูลหรือล้าสมัย ให้สแกนตามปกติ

## 🧭 Ground Rule: มองรอบก่อนพุ่ง
เมื่อติดปัญหา — หยุด มองดูว่ามี asset อะไรในโปรเจ็กต์ที่เกี่ยวข้องอยู่แล้วบ้าง
โปรเจ็กต์นี้มีของดีผ่านการทดลองมามาก แต่บางอันถูกทิ้งเพราะมีอะไรดีกว่ามาแทน หรือยังไม่เจอเคสเหมาะ
**อย่า hardcode/scan ใหม่ ถ้ามีของที่ใช้ได้อยู่แล้ว**

## 🔧 LSP / Code Intelligence
- โปรเจ็กต์ใช้ **clangd-mcp-server** สำหรับ C/C++ code intelligence (MCP server)
- Tools พร้อมใช้: `find_definition`, `find_references`, `get_hover`, `workspace_symbol_search`, `find_implementations`, `get_document_symbols`, `get_diagnostics`, `get_call_hierarchy`, `get_type_hierarchy`
- ใช้ explore/navigate โค้ด C/C++ ก่อนแก้ไขทุกครั้ง
- `compile_commands.json` อยู่ที่ root โปรเจ็กต์
- clangd path: `C:/msys64/mingw64/bin/clangd.exe`

## 🐛 GDB Debugger
- โปรเจ็กต์ใช้ **embedded-gdb-mcp** (`@vividcodeai/embedded-gdb-mcp`) สำหรับ debugging ผ่าน MCP
- Tools พร้อมใช้: `gdb_start`, `gdb_load`, `gdb_command`, `gdb_terminate`, `gdb_list_sessions`, `gdb_attach`, `gdb_load_core`, `gdb_set_breakpoint`, `gdb_continue`, `gdb_step`, `gdb_next`, `gdb_finish`, `gdb_backtrace`, `gdb_print`, `gdb_examine`, `gdb_info_registers`, `gdb_list_source`
- ใช้สำหรับ debug crash (AV, segfault), inspect memory/tensors, step through code
- GDB path: `C:\mingw64\bin\gdb.exe` (version 8.1)
- เมื่อเจอ crash ที่ไม่ชัดเจน ให้ใช้ gdb debug ก่อนเพิ่ม printf/fprintf

## 🧠 Memory Architecture — Index + Fact Store

memory (inline, 8K limit) = **index only** — thin pointers (`probe key → desc`)
fact_store (SQLite) = **รายละเอียดทั้งหมด** — `fact_store search/probe/reason`

- ถ้า memory แสดง `probe xxx` → เรียก `fact_store search xxx` เพื่อดึงเต็ม
- memory เต็มเมื่อไหร่ → consolidate index, ย้ายรายละเอียดไป fact_store
- **First Principle: ห้ามคิด compressor mindset** — "คิดจะบีบ = ไปผิดทางทันที"
  MAP not COMPRESS, เปลี่ยนมิติเข้าถึงข้อมูล ไม่บีบ payload

## 🧠 Behavioral Rules

### 1. File Deletion — Strict Scoping
- **ห้ามลบไฟล์เด็ดขาด** ยกเว้น user สั่งโดยตรงแบบ explicit (written in stone)
- ถ้าต้องลบ ให้ลบเฉพาะที่ตรงกับ prompt ทุกประการ — ไม่เลยเถิดไปลบไฟล์อื่นแม้จะดู "เกี่ยวข้อง"
- เมื่อไม่แน่ใจ ให้ถาม user ก่อนทุกครั้ง

### 2. Loop Detection
- สังเกต pattern การวนซ้ำ: output ต่างกันแค่ space/whitespace, หรือพยายามแก้จุดเดิมซ้ำๆ โดยไม่ progress
- ถ้าเจอ ให้หยุด ถาม user ว่าควรเปลี่ยนแนวทางหรือไม่ (ต่อจาก Fix→Crash Loop Protocol ด้านบน)

### 3. Open Mind — ไม่ยึดติดโครงสร้างเดิม
- อย่าเอาแต่ใช้ pattern หรือ architecture เดิมซ้ำโดยไม่คิด
- มองหาความเป็นไปได้ใหม่ เสนอแนวทางที่แตกต่าง ถ้ามีเหตุผลรองรับ
- "We've always done it this way" ไม่ใช่เหตุผล

### 4. Deprecate Before Delete
- ไฟล์ .c / .h / .py ที่ไม่ได้ใช้แล้ว → ย้ายไป `deprecated/` แทนการลบ
- รักษาโครงสร้างโฟลเดอร์เดิมใน `deprecated/` เพื่อให้ traceability
- ไฟล์ที่ย้ายแล้วให้ update include/import paths หรือแจ้ง user

### 5. Convert Important Notes to Docs
- .txt, log notes, หรือข้อความสำคัญ → แปลงเป็น .md เก็บใน `docs/`
- ตั้งชื่อสื่อความหมาย ไม่ซ้ำซ้อน
- อย่าทิ้งข้อมูลสำคัญไว้ใน raw text/log โดยไม่มีโครงสร้าง

---

## 🧭 History Location

ประวัติทั้งหมดของโปรเจ็กต์นี้ (ตั้งแต่ June 15) ถูกเก็บเป็น board cards + project memory — เพื่อให้ AGENTS.md เบาและอ่านง่าย

**ถ้าต้องการค้นประวัติย้อนหลัง > 3 วัน หรือหาข้อมูลของ component ใด**: 
- ดูที่ **board** (`board_list`) — หัวข้อการทำงานเรียงตามเวลา
- หรือ **search project memory** (`ctx_search(sources=["message","memory","git_commit"])`)
- ประวัติล่าสุด 3 วัน (July 13-15) อยู่ด้านล่างนี้เท่านั้น

---

## Session History (July 13–15, 2026)

### July 15 — Full Pipeline Connection (Bond → GeoPixel → Hamburger → GPX5)
- **Created `geofield_full.c`**: standalone CLI for Bond→GeoPixel→Hamburger→GPX5 encode/decode
- Pipeline chain: `64B chunk → PoglsPiece → bond_piece_fingerprint → HbTileIn → hb_encode_run → hamburger_encode → .gpx5`
- Binary Shell codec: `fold_fibo_intersect` NOT used (simple non-zero counter instead)
- Roundtrip: 7/7 PASS but ratio ~1.2× overhead (expansion, not compression)
- **Gap Analysis**: compared implementation vs doc spec → 3 critical gaps found:
  1. **GeoField (Tier 1) MISSING** — no GpSphere scatter, no FrustumBlock, no diamond slots
  2. **geo_frame_seek MISSING** — 384× reduction (768B→2B enc) is the PRIMARY compression
  3. **Diamond Shell WRONG** — using non-zero counter instead of `fold_fibo_intersect` geometric invariants
- **Lesson saved to memory**: 6 memories (IDs 468-473) documenting pipeline order, Diamond Shell algorithm, geo_frame_seek priority, hamburger compile paths, GPX5 container semantics, and the "sequential chunk = wrong architecture" constraint

### July 14 — GFCS Codec Fix + GPXL v3-v4 Rewrite (ไม่สมบูรณ์, gap ถูกค้นพบ July 15)
- Fixed partial-last-block buffer overflow in `geofield_full_decompress()` (stack corruption → infinite loop)
- Binary Shell codec corrected: `best_nz = BS_CHUNK_SZ + 1` fix for uninitialized best_buf
- GPXL v3: frame-based format (v1/v2 CoordRecord+LetterCube+GFCS codebook removed)
- GPXL v4: Geometric mode FrustumBlock scatter (54 diamond slots), Sequential mode Fibonacci stride-37
- Benchmarks: structured data 0.02x-0.18x, incompressible ~1.05x overhead (RAW fallback capped)
- Pure C CLI (`geofield_cli.c`): 10/10 PASS 48B blocks with Binary Shell codec

### July 13 — Pipeline Glue + Hamburger Integration Exploration
- Explored `pipeline_glue.h` (856 lines header-only driver) — chain: chunk→seed→bond→shell→geopixel→hamburger→gpx5
- Identified compile dependencies: `pipeline_glue.h` include chain too broken (geo_shell.h, shell_container.h, skeleton_index.h, etc.)
- Mapped hamburger_encode.h compilation: needs `-lzstd` from MinGW, includes `geopixel/include/hamburger/`, `geopixel/include/hbv/`, `geopixel/include/geopixel/`
- Hamburger `hb_encode_run` dispatch: 1440-tick warm-up cycle → per-tile dispatch → `hb_codec_apply` → `hb_invert_record_enc` → invert stream freeze → LUT built
- GPX5 format documented: SEED + INVERT CHAIN container, not compression container
