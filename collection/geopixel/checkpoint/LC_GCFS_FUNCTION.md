# LetterCube GCFS — What It Actually Does

## TL;DR
**Spatial Memory + Deterministic Recovery**
- Creates a 27,648-slot navigable address space (like a Rubik's cube routing layer)
- Routes addresses through 3 decision points: pair→branch→ground
- Persists to disk as 4,896B chunks (GCFS) with automatic recovery from seed alone
- Supports "ghost delete" (amnesia routing — data untouched but unreachable)

---

## The 3 Core Subsystems

### 1. **LC Wire (lc_wire.h)** — Address Navigation
Turns any uint32 address into a routing decision:

```
Address 1000 → Rubik Router → 3 outcomes:
├─ ROUTE     : pair_addr (normal path exists)
├─ BRANCH    : sibling_addr (alternative path)
└─ GROUND    : residue_zone (fallback zone)
```

**Math:**
```
route = ((addr >> 1) ^ addr) % 3
  0 = pair route     (follow pair_addr link)
  1 = branch route   (go to sibling: addr ^ 1)
  2 = ground route   (deterministic map to residue 20,736..27,647)
```

**State per address:**
```c
LCWNode {
  address,      // which slot (0..27,647)
  pair_addr,    // paired address (linked)
  LCHdr,        // 32-bit: RGB9 color + sign + magnitude + angle + level
  occupied,     // 1 = filled, 0 = empty
}
```

**Operations:**
- `lcw_insert()` — place address pair (A↔B) with header
- `lcw_ghost()` — zero magnitude (amnesia) → path unreachable
- `lcw_flip()` — toggle polarity (red↔cyan complement)
- `lcw_route()` — derive destination from address + palette filter

### 2. **LC Filesystem (lc_fs.h)** — File→Chunk Mapping
Maps disk chunks to LC node pairs:

```
File "mydata" (10 KB)
  ├─ Chunk 0 (4,896B) → LC pair {addr_a, addr_b}
  ├─ Chunk 1 (4,896B) → LC pair {addr_c, addr_d}
  └─ Chunk 2 (96B)    → LC pair {addr_e, addr_f}
```

**Per chunk:**
```c
LCFSChunk {
  addr_a,         // reader node
  addr_b,         // writer node (complement of A)
  chunk_offset,   // byte position in file
  chunk_size,     // actual bytes (≤4,896)
  seed,           // deterministic reconstruction key
  ghosted,        // 1 = deleted
}
```

**File handle:**
```c
LCFSFile {
  name,           // "mydata.dat"
  chunk_count,    // how many 4,896B blocks
  level,          // LC_LEVEL_0..3 (hierarchy)
  chunks[],       // array of chunk descriptors
  trav,           // fold-aware traversal state
}
```

**Operations:**
- `lcfs_open()` — register file, allocate LC address pairs
- `lcfs_read()` — route through LC pipeline → fetch chunk
- `lcfs_delete()` — ghost all node pairs → amnesia
- `lcfs_rewind()` — traverse fold stack, reconstruct from seed

### 3. **GCFS Persistence (lc_gcfs_wire.h)** — Disk Format
Physical 4,896B chunk on disk:

```
┌─ Header (36B) ────────────────────┐
│ magic:0x46474C53                  │
│ version:0x01                      │
│ coset_count, active_count         │
│ crc32, reserved_mask, dispatch_id │
├─ Metadata (252B) ─────────────────┤
│ 9 CosetMetaRec × 28B              │
│ {seed_hi/lo, master, coset_id}    │
├─ Payload (4,608B) ────────────────┤
│ actual data (user bytes)          │
└───────────────────────────────────┘
```

**Key insight:** 4,608B = exactly 27,648 / 6 spokes
- Each spoke gets 4,608 slots
- 6 spokes total (from TRing pentagon % 6)
- Natural alignment with CPU dispatch layer

---

## How They Work Together

### Write Path
```
Data Bytes → tgw_stream_dispatch() [L2]
    ↓
goldberg_pipeline_write() [L5, frozen]
    ↓
pl_write(addr, value) [L2]
    ↓
[NEW] lcfs_write(chunk_idx, payload) → LCFile.write()
    ↓
    ├─ route addr through lcw_route()
    ├─ allocate GCFS chunk or reuse
    └─ write 4,896B block to disk
    
Disk File (append-only chunks)
```

### Read Path
```
User calls: LCFile.read(chunk_idx)
    ↓
lcfs_read(chunk_idx)
    ├─ look up LCFSChunk[idx]
    ├─ get LC pair {addr_a, addr_b}
    ├─ route addr_a through lcw_route()
    ├─ check if path blocked (ghost or collision)
    └─ if unblocked: fetch GCFS chunk from disk
    
Returns: 4,608B payload or None (blocked)
```

### Recovery Path (Rewind)
```
Lost chunk data → call LCFile.rewind(chunk_idx)
    ↓
lcfs_rewind(chunk_idx)
    ├─ get seed from LCFSChunk.seed
    ├─ traverse fold stack (unwind hierarchy)
    ├─ reconstruct GCFS chunk header via seed
    └─ return original 4,608B payload
    
Works even after delete (seed never erased)
```

---

## Ghost Delete (Amnesia)

When you delete a file:

1. **Before Delete:**
   ```
   LCWNode[1000] { occupied=1, pair=1001, hdr={RGB9, mag=64} }
   LCWNode[1001] { occupied=1, pair=1000, hdr={complement} }
   
   routing: addr=1000 → rubik_route → dest=1001 ✓ reachable
   ```

2. **After Delete (lcfs_delete):**
   ```
   lcw_ghost(1000):  hdr.magnitude = 0  (zero mag = ghost)
   lcw_ghost(1001):  hdr.magnitude = 0  (pair also erased)
   
   routing: addr=1000 → lch_is_ghost() → GROUND (unreachable)
   ```

3. **Result:**
   - Node pair marked deleted in LC routing table
   - Actual GCFS chunk data stays on disk (append-only)
   - Path becomes unreachable → invisible to reads
   - Seed stored separately → data recoverable if seed known

---

## Why This Design

| Feature | Benefit |
|---------|---------|
| **3-route Rubik** | Deterministic routing (no hash table, no collision handling) |
| **Pair semantics** | Reader↔Writer separation; complement polarity |
| **LC header (RGB9)** | Palette filtering during traversal (no external sort needed) |
| **GCFS 4,896B chunk** | Perfect fit: 6 spokes × 768B = 4,608B payload |
| **Seed-only recovery** | Regenerate chunk from deterministic seed (no logging needed) |
| **Ghost amnesia** | Delete via magnitude=0, not file truncation |
| **Fold-aware traversal** | Hierarchical LC_LEVEL support without recursion |

---

## Integration Point (Layer 4)

```
TGW_DISPATCH (Layer 2)
    ↓ (ROUTE/GROUND split)
GOLDBERG (Layer 5)
    ↓ (blueprint write)
PL_WRITE (in-memory)
    ↓ [NEW HOOK]
LCFile.write() ← Layer 4 Persistence
    ├─ route addr through lcw_route()
    ├─ store in GCFS chunk
    └─ append to disk
```

**No changes to L2/L5 dispatch logic**
Just one output hook: `pl_write() → lcfs_write()`

---

## Status

✅ **LC-GCFS is complete:**
- Core logic frozen (lc_wire.h)
- Filesystem ops ready (lc_fs.h)
- GCFS persistence defined (lc_gcfs_wire.h)
- Python client + .so compiled

❌ **NOT yet connected:**
- tgw_stream_dispatch doesn't call lcfs_write()
- no chunk allocation strategy
- no multi-file coordination

**Next:** Add output hook after CPU pipeline locks (after geopixel patch + SD1-SD5 pass)
