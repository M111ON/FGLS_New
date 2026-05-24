# Session Handoff — POGLS Bond Layer v1.1

## State: 2026-05-18

## Deliverables (all ✅)

| # | Component | Files | Tests |
|---|-----------|-------|-------|
| 1 | Bond header + Config | `pogls_bond.h` `pogls_config.h` | 63/63 |
| 2 | C ABI exports | `pogls_bond_export.c` `pogls_bond_export.h` | — |
| 3 | Shared library (Mingw) | `pogls_bond.dll` | — |
| 4 | Python ctypes bridge | `python_src/pogls_bridge.py` | 50/50 (77K verify/s) |
| 5 | Bond→TGW router | `tgw_bond_dispatch.h` | 336/336 |
| 6 | Pipeline include copy | `core/geo_headers/pogls_bond.h` | — |

## Test Suites

### C: bond v1.1
```bash
cd collection
gcc -O2 -I. -D__USE_MINGW_ANSI_STDIO -o test_bond_v2 test_pogls_bond_v2.c && ./test_bond_v2
```
12 sections, 63 tests — fibo_addr, piece factory, intrinsic bond, wallet bridge, plug chain, reroute, false-positive rate (N=100K, 0 hits), nonce isolation, backward compat, reroute chain, plug TTL, config paths.

### C: TGW dispatch
```bash
gcc -O2 -I. -D__USE_MINGW_ANSI_STDIO -o test_tgw test_tgw_dispatch.c && ./test_tgw
```
9 sections, 336 tests — shape→polarity mapping, TRing 720-slot alloc (odd-spoke constraint), I/O/T/S/0 dispatch, O-latch tick, fanout, stats, slot query.

### Python: ctypes bridge
```bash
set POGLS_SO_PATH=pogls_bond.dll
python poc_bond_standalone.py
```
8 sections, 50 tests — same as bond v1.1 but via ctypes.

## Architecture

```
topology_fp (hex 16 chars)
  → pogls_seed_from_fp(fn + fibo) → uint64 origin_seed
  → pogls_make_piece(origin_seed, axis)
    → geo_key  = fibo_addr(origin_seed)
    → bond_L   = fibo_addr(geo_key ^ 0xAAAA..)
    → bond_R   = fibo_addr(geo_key ^ 0x5555..)
    → shape    = AXIS_SHAPE[axis] (I/O/T/S/Z/L/J)
  → bond_key = bond_L XOR bond_R

tgw_dispatch(piece_a, piece_b):
  1. bond_verify → PoglsBond {valid, bond_key}
  2. shape_polarity → ROUTE or GROUND
  3. tring_pos = bond_key % 720 (GROUND forced to odd)
  4. shape dispatch: I=OK, O=HELD, T=FANOUT, J=OK,
                     S=GROUND, Z=GROUND, L=GROUND
```

## Key Design Decisions

- **Stateless** — no persistence needed. wallet = JSON file, not DB.
- **Nonce = 0 by default** — backward compatible with v1.0. Set via `pogls_config_set_nonce()` for replay protection.
- **32-bit verify mask** — 1/4B false positive. Configurable to 48-bit.
- **ctypes supersedes subprocess** — old `geo_field_bridge.py` (subprocess) and `pogls_bond_py.py` (pure Python v1.0) deprecated.
- **Windows only currently** — DLL built with Mingw. `.so`/`.dylib` build commands documented in AGENTS.md.

## Architecture Gaps (next team)

| Layer | Status | Notes |
|-------|--------|-------|
| Bond → TGW dispatch | ✅ `tgw_bond_dispatch.h` | 336 tests |
| TGW → FGLS/TPOGLS | ❌ | No bridge yet. Handoff mentioned as "next task" for full-stack benchmark |
| GeoPixel integration | ❌ | `geo_pixel.h` still separate from bond layer |

## What's NOT Done (by design)

- Persistence layer (SQLite/LMDB) — stateless by design. wallet = file, not DB.
- Encryption/auth — bond layer is geometry gate, not security protocol.

## Fixes Applied

| Finding | Fix |
|---------|-----|
| `test_cross_consistency.py` imported deprecated pure-Python v1.0 | Rewritten to use ctypes bridge — all 8 tests pass |
| `test_integration_points.py` hardcoded `I:\ZGLS\...` | Changed to `POGLS_PYTHON` env var with fallback |
| Windows-only DLL | Added .so/.dylib build commands to AGENTS.md |
| ZGLS pre-existing `WalletBuilder.seal()` bug | Documented — bond wallet is JSON, not affected |

## Demo Files

| File | What it does |
|------|-------------|
| `poc_bond_standalone.py` | Portable C↔Python bridge demo (8 sections) |
| `poc_geo_sandbox.py` | Pure-sandbox: ingest → ghost → reconstruct |
| `poc_bond_sandbox.py` | Full-stack: topology → bond → ghost → Ω |
| `poc_geofield_to_bond.py` | C geofield → bond → Smart Folder |
| `poc_cross_device.py` | TCP tunnel prototype — `--listen` / `--connect` |

## Package

`zip/bond_layer_v1.1.zip` — 17 files, 62 KB. Ready for handoff.

## Inbox Manager

- Known bug: `apply_all` fails when match is None (new files). Workaround: `put_file` registers, manual Copy-Item to deploy.
- All source files are already on disk in `collection/`.
