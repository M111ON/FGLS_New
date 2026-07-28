---
illustration_id: 01
type: framework
style: blueprint
---

FGLS Pipeline Architecture - Technical Schematic

STRUCTURE: hierarchical pipeline flow, left-to-right progression

ZONES:
- Zone 1 (Input): 64B raw data chunks entering the system, represented as glowing data blocks
- Zone 2 (PoglsPiece): Initial geometric decomposition, base-12 lattice structure visible
- Zone 3 (Bond): Fingerprint extraction, deterministic coordinate mapping, hash-free routing
- Zone 4 (Hamburger): Multi-stage encoding - HbTileIn → hb_encode_run → hb_codec_apply
- Zone 5 (GPX5): Final container format with SEED + INVERT CHAIN structure

CORE ARCHITECTURE (displayed as overlay schematic):
- Base-12 lattice: 12^4 = 20,736 total slots, 12^3 = 1,728 pipes, 12^2 = 144 clusters, 12^1 = 12 ticks
- Dual Timer System: frame_seek (1,440 ticks, stride-37 geometry) + beam_timer (20,736 slots, spine traversal)
- Coordinate IS Data principle: no hash tables, no collision, O(1) deterministic mapping

VISUAL ELEMENTS:
- Blueprint grid background with subtle coordinate axes
- Pipeline stages as connected schematic blocks with labeled ports
- Data flow arrows showing transformation at each stage
- Mathematical notation overlays: 12^k decomposition, Fibonacci strides
- Timer visualization: circular dial showing 1,440 tick cycle, linear spine showing 20,736 progression

LABELS:
- "64B Chunk Input" at entry point
- "PoglsPiece" at decomposition stage
- "Bond Fingerprint" at coordinate mapping
- "HbTileIn → hb_encode_run" at encoding core
- "GPX5 Container" at output
- "frame_seek: 1440 | stride-37" on timer overlay
- "beam_timer: 20736 | spine" on timer overlay
- "Base-12 Lattice: 12^4 = 20,736" as architecture foundation

COLORS:
- Background: Deep Navy (#0A1628)
- Primary Grid: Cyan (#00D4FF) - subtle blueprint grid lines
- Pipeline Blocks: White (#FFFFFF) with Cyan (#00D4FF) borders
- Data Flow: Electric Blue (#0066FF) arrows
- Timer Elements: Amber (#FFB800) for frame_seek, Magenta (#FF00FF) for beam_timer
- Mathematical Text: White (#FFFFFF) with slight glow
- Accent Highlights: Teal (#00FFD1) for key values

STYLE:
- Technical schematic aesthetic, precise linework
- Blueprint blue (#0A1628 background, #00D4FF lines)
- Monospace typography for labels and numbers
- Grid-based composition with clear zones
- Engineering diagram precision, architectural drafting feel
- Subtle scan-line texture for retro-technical atmosphere
- Clean geometric shapes, no organic curves

ASPECT: 16:9
