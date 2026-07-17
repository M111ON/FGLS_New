# Handoff: pogls_bond_edge.h Exploration

**Date:** 2026-07-16
**Session:** Short exploration — bond edge vs L-block relationship

## Goal of Next Session

Decide where `pogls_bond_edge.h` belongs in the architecture and whether to integrate it with L-block connectors.

## State of Play

- `pogls_bond_edge.h` exists at project root (`I:\FGLS_new\pogls_bond_edge.h`, 173 lines)
- **Not yet placed** in `collection/` (general-purpose modules per project rule memory #118)
- L-block container spec at `docs/hilibert-lblock-spec.md` — 307 tests passing
- Bond edge analysis complete — file is **complementary** to L-block, not competing:

| L-block | Bond Edge |
|---|---|
| Spatial locality (3-cell co-access) | Directed variable-arity graph |
| Fixed connectors (raw 32B data) | Tamper-evident edges (origin→target + weight) |
| Static dependency graph | Data-dependent relationships |

## Key Findings

1. **Bond edge reuses `pogls_bond.h` machinery** — same `fibo_addr` + salt + verify mask → tamper-evidence without separate integrity system
2. **PoglsPlug stays fixed 4-face (N/S/E/W)** — bond edges are independent, variable-arity supplement
3. **Potential integration point:** L-block connectors (32B raw) could be replaced/enhanced with bond edges for automatic tamper-evidence on block-to-block relationships
4. **`weight` field is generic `uint32_t`** — caller-defined (strength, priority, byte offset, refcount)

## Open Decisions

1. Move `pogls_bond_edge.h` to `collection/`? (per memory #118: general-purpose components belong there)
2. Should bond edges replace or supplement L-block connectors?
3. Should bond edges get their own test suite?
4. Integration with pipeline: `Input → Chunk → L-block → [Bond Edge?] → Bond → Shell → Pixel → Hamburger → GPX5`

## Skills to Use

- `pogls-pipeline` — for architecture context on bond/geo integration
- `msys2-build-pipeline` — if compiling tests on Windows/MinGW

## Artifacts

- `pogls_bond_edge.h` — the file under discussion (project root)
- `collection/pogls_bond.h` — parent bond layer (reference)
- `docs/hilibert-lblock-spec.md` — L-block spec (453 lines)
- `docs/hilibert-lblock-value.md` — L-block value proposition
- `runner/pogls_hilbert_container/` — L-block implementation (307 tests)
