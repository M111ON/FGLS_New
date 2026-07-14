# Quality Polish Report — FGLS_new

Generated: 2026-07-14

## Executive Summary

| Metric | Value |
|--------|-------|
| C source files (runner) | 155 |
| Python files | 508 |
| C++ source files | 27 |
| Header files (runner) | 50 |
| Test executables (`main()` in runner/) | 130+ |
| Build artifacts (.exe) in runner/ | 19 tracked |
| Duplicate headers (3+ copies) | 100+ |
| Session/handoff .md docs | 100+ |
| Root dir entries | 150 |
| Core directory copies | 5 |

---

## Priority 1 — Critical Structural Issues

### P1-1. Runner Monolith (180KB)
`runner/llama_pogls_runner_sid_v2.c` is 180KB — a single file that handles SID, DRamTile, GPU, VRamTile, KV remap, chat loop, CLI parsing, and everything else. Need to split into modules:
- `runner_cli.c` / `runner_chat.c` / `runner_init.c` / etc.

### P1-2. Duplicate Core Directories
There are **5 copies** of the same core source headers:
- `core/` (716 files)
- `collection/core/` (627 files)
- `collection/geopixel/hbv_bundle/core/core/`
- `core/pogls_engine/TPOGLS_s11/TPOGLS_s11/core/`
- `core/pogls_engine/TPOGLS_S36_Release/core/`

Plus `deploy/colab/FGLS_new/collection/core/` (deploy copy)

100+ headers appear 3+ times (e.g. `geo_config.h` 17 copies, `geo_net.h` 20 copies, `pogls_fold.h` 18 copies). This is a maintenance liability — which copy is canonical?

**Action**: Deduplicate to one canonical `collection/core/`, remove `core/`, remove deploy duplicates from version control.

### P1-3. No Unified Test Runner
130+ test `.c` files with individual `main()` functions. No automated test discovery, no CI integration, no coverage tracking. Tests in:
- `runner/*.c` (90+ test_*.c files)
- `runner/tests/` (19 files)
- `runner/pogls_tools/` (2 files)
- Module dirs: `pogls_*/test_*.c` (7 files)

**Action**: Create a test harness that discovers and runs all tests, produces pass/fail summary, and can be wired to CI.

### P1-4. Root Directory Pollution
Root has 150 entries — test scripts, session logs, zips, backups, config files, docs. Should be:
- `docs/` — documentation
- `runner/` — inference engine
- `collection/` — core library
- `tools/` — CLI tools
- `scripts/` — test/build scripts

Current root has `test_*.py`, `test_*.txt`, `.zip`, `.geopixel`, `.tar.gz`, session `.md` files, build logs.

---

## Priority 2 — Code Quality

### P2-1. 508 Python Files — Unmanaged Explosion
Many are experiments that should be consolidated or removed:
- `rest_server_s*.py` (12+ versions of the same server)
- `geo_net_py.py`, `geo_net_ctypes_s*.py` (5+ networking experiment copies)
- `hamburger_codec_poc_v*.py` (5 POC versions)
- `exp_geo*.py`, `hb3.py`, `test_*.py` (30+ loose Python scripts in root)

### P2-2. Oversized Headers in runner/
- `dramtile_store.h` 64KB — implementation-heavy header
- `kv_remap.h` 31.5KB
- `pogls_loader.h` 24.6KB
- `kv_page_store.h` 22.4KB

These should be `.c` + `.h` pairs, not monolithic header-only implementations.

### P2-3. Duplicate Geopixel Pipeline Files
- `tools/geopixel_pipeline.py` — 1545 lines, massive working diff (+1287/-258)
- `tools/geopixel_pipeline.py.geopixel` — stale copy loose in root
- `tools/geopixel_gui.py` — separate GUI version
- `geopixel/` — separate geopixel directory at root

### P2-4. Stale /crash Artifacts
- `runner/nul` — file named "nul" (Windows reserved name)
- `*.geopixel` files scattered (auto-saves from editor?)
- `null` file at root
- `magic-context.jsonc.MOVED_READPLEASE`

### P2-5. Inconsistent build targets
- `Makefile` in runner/ — clean but limited
- `Makefile` in collection/ — separate
- `build.bat`, `build_test.bat`, `build_gguf.bat` — ad-hoc scripts
- No CMake or single-entry build

---

## Priority 3 — Project Hygiene

### P3-1. Session Log Overload
100+ `HANDOFF_*.md`, `session-ses_*.md`, `SESSION*.md` files accumulate. These are useful for context but pollute search. Options:
- Move to `docs/session-logs/` (already exists at `docs/session-logs/`)
- Or `archive/sessions/`

### P3-2. .gitignore Coverage
- `*.o` gitignored but `.o` files still in some dirs (tracked or untracked)
- `*.exe` gitignored but `tools/*.exe` shows as untracked (symlink copies to runner/)
- `runner/nul` should be gitignored explicitly
- `*.geopixel` should be gitignored

### P3-3. Untracked File Cleanup
30+ untracked files including:
- Build artifacts: `addr_resolve`, `dramtile_dump`, `pogls_*` (no extension — MinGW builds without .exe?)
- Test binaries: `test_*.exe` in tools/
- Archives: `FGLS_build_v2.0.zip`
- Temp: `test_small.txt`, `test_batch/`, `test_compression.py`

### P3-4. package-lock.json in Root
`package-lock.json` present but no `package.json` — likely belongs to `clangd-mcp-server/` or `.opencode/` but checked into wrong level.

---

## Priority 4 — Architectural Improvements

### P4-1. Module Boundary Clarity
`runner/` mixes:
- POGLS standalone modules (`pogls_gguf/`, `pogls_geo/`, etc.)
- LLM inference runner
- Test files
- DLLs and import libs

Standalone modules should be truly standalone (memory `P4-1` already says `Zero-dep std C with extern "C" guards`).

### P4-2. CI / Automation
No `.github/workflows/` CI pipeline. The `.github/` directory exists but appears empty of workflows. For a project targeting production use, CI is critical.

### P4-3. Versioned API Surface
No versioned API for the POGLS loader/library. `pogls_loader.h` is the bridge to external inference engines — needs stability guarantees and versioning.

---

## Prioritized Task List

### Must Fix (Quality Blockers)

1. **Deduplicate core directories**: Collapse core/ → collection/core/, remove deploy copies from VC
2. **Split runner monolith**: 180KB → 3-5 files (cli, chat, init, sid, kv)
3. **Clean root directory**: Move test scripts to `scripts/`, logs to `docs/session-logs/`, archives to `archive/`
4. **Create unified test runner**: One command to run all tests with pass/fail summary
5. **Fix .gitignore**: Add `*.geopixel`, `runner/nul`, `null`, `package-lock.json`

### Should Fix (Quality Improvements)

6. **Convert header-only implementations to .c/.h pairs**: dramtile_store.h, kv_remap.h, etc.
7. **Consolidate Python experiments**: Remove or archive old rest_server_s*.py, hamburger POCs, geo_net variants
8. **Move session logs**: All 100+ HANDOFF/SESSION docs to `docs/session-logs/`
9. **Clean untracked files**: Remove or gitignore build artifacts, .geopixel copies, test debris
10. **Add CI workflow**: GitHub Actions for build + test

### Nice to Have (Polish)

11. **CMake build** instead of raw Makefile
12. **API versioning** for pogls_loader
13. **Code coverage** tracking
14. **Documentation consolidation**: Merge DEVELPOMENT_SUMMARY.md, HISTORY.md, WORKLOG.md into structured docs
15. **Standardize on one Python version** for scripts (3.10 appears across .pyc caches)
