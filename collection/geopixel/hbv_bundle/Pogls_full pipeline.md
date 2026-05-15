# Tab 1

Now I have all the pieces. Here is the comprehensive summary.  
\---

## POGLS Data Pipeline — Complete Analysis

### 1\. The Real Data Source: It's NOT (Only) Images

The entire pipeline in \`I:\\FGLS\_new\\collection\\\` is a **geometric content fingerprinting and routing fabric**, not a conventional image compressor. The input is **any arbitrary byte stream** — files, mmap'd data, network buffers, sensor streams. The pipeline treats all data identically:  
\`\`\`  
ANY binary stream → 64B chunks → geometric fingerprints (ThetaCoord on dodecahedron torus)  
    → DiamondBlock cells → route/fold → flow detection → DNA records  
    → (optionally) H64 Hilbert grid → (optionally) hamburger tile codec  
\`\`\`  
BMP files are used only as **test fixtures** (\`high\_detail.bmp\`, \`test02\_768.bmp\`). The scanner explicitly detects pre-compressed formats (ZIP, GZIP, ZSTD, PNG, JPEG, MP4, WEBP — lines 45-52 of \`pogls\_scanner.h\`) and marks them \`SCAN\_FLAG\_PASSTHRU\` — they bypass all compression and pass through verbatim.  
\---

### 2\. \`pogls\_scanner.h\` — The Entry Point

**Path**: \`I:\\FGLS\_new\\collection\\geopixel\\hbv\_bundle\\core\\pogls\_scanner.h\`  
**Role**: Takes any byte buffer, splits into 64B DiamondBlock-aligned chunks, and emits a \`ScanEntry\` per chunk via callback.  
**\`scan\_buf\` function** (lines 164-237):  
1\. Splits \`buf\` into 64B chunks (last chunk zero-padded if \<64B).  
2\. On first chunk, checks magic bytes for pre-compressed formats; if detected, marks ALL chunks \`SCAN\_FLAG\_PASSTHRU\`.  
3\. Per chunk:  
   \- **Seed** (lines 116-129): XOR-fold 8 words (\`w\[0\]^...^w\[7\]\`) → avalanche via \`theta\_mix64\` (murmur finalizer-style). This is the **content fingerprint** — 64 bytes reduced to 1 uint64.  
   \- **ThetaCoord** (via \`theta\_map(seed)\`): Deterministic geometric coordinate (face/edge/z on dodecahedron torus).  
   \- **Checksum**: XOR-fold of raw 64B.  
4\. Optionally computes angular address via \`pogls\_node\_to\_address\` (gear=0, world=0 fixed).  
5\. Calls callback \`cb(entry, user)\` per chunk.  
**Key insight**: The \`seed\` is a content-dependent fingerprint, and \`theta\_map(seed)\` maps each chunk to a **dodecahedron face (0..11), edge (0..4), and torus layer z (0..255)**. Similar content maps to nearby coordinates.  
\---

### 3\. \`pogls\_fold.h\` — The 64B DiamondBlock Cell

**Path**: \`I:\\FGLS\_new\\collection\\geopixel\\hbv\_bundle\\core\\pogls\_fold.h\`  
**Role**: Defines the **DiamondBlock** — a 64B (1 CPU cache line) geometric cell with built-in integrity verification.  
**Layout of DiamondBlock** (lines 139-148):  
| Offset | Size | Field | Role |  
|--------|------|-------|------|  
| 0-7 | 8B | \`CoreSlot\` | Primary data: face\_id(5b), engine\_id(7b), vector\_pos(24b), fibo\_gear(4b), quad\_flags(8b), reserved(16b) |  
| 8-15 | 8B | \`Invert\` | \`\~core.raw\` — pair invariant: \`core ^ invert \== 0xFF...\` (Layer 1 audit) |  
| 16-47 | 32B | \`Quad Mirror\` | 4 rotated copies of Core (rot 0,1,2,3 bytes) for geometric intersect |  
| 48-63 | 16B | \`Honeycomb\` | Tails state: merkle\_root, algo\_id, migration, dna\_count, reserved |  
**\`fold\_block\_init\`** (lines 183-197):  
\`\`\`c  
DiamondBlock fold\_block\_init(face\_id, engine\_id, vector\_pos, fibo\_gear, quad\_flags)  
\`\`\`  
Builds a complete DiamondBlock: constructs CoreSlot, computes Invert \= \~core.raw, builds Quad Mirror.  
**\`fold\_build\_quad\_mirror\`** (lines 161-177):  
Packs 4 byte-rotated copies of CoreSlot into 32B quad\_mirror:  
\- Copy 0: original (rot 0\)  
\- Copy 1: rotate left 1B (rot 8\)  
\- Copy 2: rotate left 2B (rot 16\)  
\- Copy 3: rotate left 3B (rot 24\)  
**\`fold\_fibo\_intersect\`** (lines 253-258):  
\`\`\`c  
uint64\_t fold\_fibo\_intersect(const DiamondBlock \*b)  
\`\`\`  
AND of all 4 rotated copies: \`q\[0\] & q\[1\] & q\[2\] & q\[3\]\`. This returns the **bits that survive rotation** — the geometric invariants of the cell. If this is 0, the cell has no geometric structure (entropy exhausted → flow ends).  
**3-Layer Verify** (lines 342-360):  
\- **Layer 1** (\~0.3ns): XOR audit — \`core ^ invert \== 0xFF...\` (hard gate)  
\- **Layer 2** (\~5-10ns): Fibonacci Intersect entropy check — if \`popcount(intersect)\` \< threshold, needs Merkle (Layer 3\)  
\- **Layer 3**: Full Merkle verify (expensive, rarely triggered)  
\---

### 4\. \`geo\_diamond\_field.h\` — The Diamond Cell Field

**Path**: \`I:\\FGLS\_new\\collection\\geopixel\\hbv\_bundle\\core\\geo\_diamond\_field.h\`  
**Role**: Treats DiamondBlocks as cells in a routing field. Implements cell flow: gate → route → accumulate → write DNA on termination.  
**\`diamond\_gate\`** (lines 61-79):  
\- **L0**: XOR audit (hard gate) — drop cells that fail the pair invariant  
\- **L1**: Drift check (sampled 1/8 of cells) — if drift \> 72, triggers verify  
Returns: 0=drop, 1=pass, 2=pass+drift\_detected  
**\`diamond\_route\`** (lines 86-91):  
\`\`\`c  
int diamond\_route(const DiamondBlock \*b)  // returns 1=MAIN, 2=TEMP  
\`\`\`  
Uses \`popcount(fibo\_intersect) & 1\` parity to assign lane:  
\- Even popcount \= MAIN (icosa symmetric)  
\- Odd popcount \= TEMPORAL (dodeca asymmetric)  
**Flow routing** (lines 243-320):  
\- \`DiamondFlowCtx\` accumulates: \`route\_addr\` (XOR of all intersects), \`hop\_count\`, \`drift\_acc\`  
\- \`diamond\_route\_update(r, intersect)\`: \`rotl(r, K) ^ (intersect \* P)\` — invertible path encoding  
\- \`diamond\_flow\_end()\` detects termination: intersect==0 (dead), drift\>72 (drift), or ring full  
\- On termination, \`diamond\_dna\_write()\` saves \`route\_addr \+ hop\_count \+ offset\` into the cell's HoneycombSlot  
**The crucial insight**: The route address accumulates an **invertible fingerprint of the entire flow path**. Since \`diamond\_replay\_step()\` can recover individual intersects from consecutive route addresses, every cell along the path can be reconstructed from just the DNA record and the first cell.  
**AVX2 batch processing** (lines 475-624): 4-flow parallel (\`DiamondFlowCtx4\`) using \`\_mm256\` intrinsics for route update, drift check, and flow end detection. Called \`diamond\_batch\_temporal\_x4()\`.  
\---

### 5\. \`geo\_route.h\` — ThetaCoord → TorusNode Bridge

**Path**: \`I:\\FGLS\_new\\collection\\geopixel\\hbv\_bundle\\core\\geo\_route.h\`  
**Role**: Converts \`ThetaCoord\` (face/edge/z) → \`TorusNode\` for walking the dodecahedron torus.  
**Layers** (from comment):  
\`\`\`  
theta\_map   → ThetaCoord (face, edge, z)  
geo\_route   → TorusNode  (face, edge, z, state=0)  
diamond\_flow→ route\_addr accumulation → DNA write  
\`\`\`  
The torus (\`geo\_dodeca\_torus.h\`) has a verified \`g\_map\[12\]\[5\]\` adjacency table (12 dodecahedron faces, 5 edges each) with a \`torus\_step()\` function that walks the surface, tracking XOR parity via \`state\`.  
\---

### 6\. \`geo\_dodeca\_torus.h\` — The Dodecahedron Torus Walker

**Path**: \`I:\\FGLS\_new\\collection\\geopixel\\hbv\_bundle\\core\\geo\_dodeca\_torus.h\`  
Defines \`TorusNode { face, edge, z, state }\` and \`g\_map\[12\]\[5\]\` — the mathematically verified adjacency map for a dodecahedron with wrap-around (torus topology). \`torus\_step()\` walks one edge, and \`torus\_run\_field()\` combines boundary pass \+ XRay recording \+ step \+ fibo clock tick for streaming.  
\---

### 7\. \`theta\_map.h\` — The Core Mapping Function

**Path**: \`I:\\FGLS\_new\\collection\\geopixel\\hbv\_bundle\\core\\theta\_map.h\`  
**Role**: Maps any \`uint64\_t\` → \`ThetaCoord {face(0..11), edge(0..4), z(0..255)}\`.  
**Implementation** (lines 48-63):  
\`\`\`c  
uint64\_t h \= theta\_mix64(raw);  // murmur finalizer avalanche  
face \= (h\_hi \* 12\) \>\> 32;       // Lemire fast reduce (no division)  
edge \= (h\_lo \* 5\) \>\> 32;        // Lemire fast reduce  
z    \= (h \>\> 16\) & 0xFF;        // mid-bits  
\`\`\`  
This is a **pure integer hash** that deterministically scatters any 64-bit value onto the dodecahedron torus. The \`theta\_mix64\` ensures avalanche — every bit of input affects the output.  
\---

### 8\. \`angular\_mapper\_v36.h\` / \`pogls\_fibo\_addr.h\` — Fibonacci Address Engine

**Path**: \`I:\\FGLS\_new\\collection\\core\\pogls\_engine\\TPOGLS\_S36\_Release\\core\\pogls\_fibo\_addr.h\`  
**Role**: Replaces floating-point angular addressing with pure integer Fibonacci sampling.  
**Core law**: \`A \= floor(θ × 2²⁰)\` becomes \`A \= (n × PHI\_UP) % PHI\_SCALE\` — NO float, NO math.h, NO 2π.  
**Constants** (frozen):  
\- \`PHI\_SCALE \= 2²⁰ \= 1,048,576\`  
\- \`PHI\_UP \= 1,696,631\` (floor(φ × 2²⁰), World A)  
\- \`PHI\_DOWN \= 648,055\` (floor(φ⁻¹ × 2²⁰), World B)  
**Gear modes** (4-bit FIBO\_GEAR in CoreSlot):  
\- **G1 (0-3) direct**: \`(n × base) % PHI\_SCALE\`  
\- **G2 (4-8) batch**: \`(n × base × factor) % PHI\_SCALE\`  
\- **G3 (9-15) blast**: \`(n × (base \<\< shift)) % PHI\_SCALE\`  
**Inverse**: \`fibo\_addr\_to\_node\_a(addr) \= (addr × PHI\_UP\_INV) % PHI\_SCALE\` — exact integer inverse for position recovery.  
\---

### 9\. \`pogls\_hilbert64\_encoder.h\` — The 64-Slot Hilbert Grid

**Path**: \`I:\\FGLS\_new\\collection\\geopixel\\pogls\_hilbert64\_encoder.h\`  
**Role**: Maps ScanEntry stream into a 64 × 16B Hilbert-grid packet organized by RGB color channels.  
**Layout**:  
\- 12 dodecahedron faces × 4 paths (3 positive \+ 1 invert) \= 48 slots  
\- 16 ghost/residual slots (slots 48-63)  
\- RGB channel \= face % 3: faces 0,3,6,9→R; 1,4,7,10→G; 2,5,8,11→B  
Each face gets 4 slots: 3 positive (path\_id 0,1,2) \+ 1 invert (path\_id 3, XOR-derived from the 3 positive seeds).  
**\`h64\_feed()\`**: Inserts one ScanEntry into the slot grid at a given path\_id (0-2).  
**\`h64\_derive\_invert()\`**: Computes invert \= XOR of 3 positive seeds for a face.  
**\`h64\_finalize()\`**: Derives all 12 invert paths.  
\---

### 10\. \`pogls\_to\_geopixel.h\` — H64 → Geopixel Bridge

**Path**: \`I:\\FGLS\_new\\collection\\geopixel\\pogls\_to\_geopixel.h\`  
**Role**: Converts \`HilbertPacket64\` → \`H64GeopixelBridge\` (seed-only tiles, 4B each). 16× theoretical compression ratio (4096B raw → 256B per packet).  
**\`h64\_to\_geopixel()\`** (lines 86-129): Extracts 32-bit seeds from each cell, epoch-mixes them, produces tile descriptors ready for hamburger encoding.  
**\`h64\_pipeline()\`** (lines 133-147): End-to-end: ScanEntry\[\] → HilbertPacket64 → GeopixelBridge in one call.  
\---

### 11\. \`hamburger\_classify.h\` / \`hamburger\_encode.h\` — Tile Codec

**Path**: \`I:\\FGLS\_new\\collection\\geopixel\\hamburger\_classify.h\`  
**Role**: Classifies tiles as FLAT / GRADIENT / EDGE / NOISE using integer variance (×100 to avoid float). Assigns codecs per carrier type.  
**Path**: \`I:\\FGLS\_new\\collection\\geopixel\\hamburger\_encode.h\`  
**Role**: Full tile-based encode/decode with:  
\- Codec dispatch: SEED (4B for flats), DELTA (XOR vs prediction), HILBERT (walk/no-walk bitmask), RICE3 (Rice k=3), FREQ (block-average \+ LEB128 residuals), ZSTD19  
\- Invert recorder (H2): records the "analog negative" — encoded bytes that differ from prediction  
\- 1440-tick warm-up with LUT freeze for O(1) decode  
\- Multi-cycle support for large images  
\- \`hamburger\_encode\_image()\`: One-call: classify → auto\_pipes → tile → encode → write \`.gpx5\`  
\---

### 12\. \`frustum\_coord.h\` / \`frustum\_coset.h\` — The Storage Layer

**Path**: \`I:\\FGLS\_new\\collection\\geopixel\\frustum\_coord.h\`  
**Role**: Universal block coordinate interface for 6-face frustum (cube/cylinder/goldberg). Every block is 4896B \= 17 × 2 × 144 with O(1) neighbor lookup and shell addressing.  
**Path**: \`I:\\FGLS\_new\\collection\\geopixel\\frustum\_coset.h\`  
**Role**: Byte-exact layout of a 4896B FrustumBlock:  
\- 54 DiamondBlocks (3456B data zone), partitioned into 3 cosets of 18 diamonds each  
\- Meta zone: letter\_map\[144\], slope\_map\[144\], drain\_state\[12\], merkle\_roots\[12×32\], reserved  
\- O(1) direct seek for every diamond and field  
\---

### 13\. The Complete End-to-End Data Flow

\`\`\`  
\[ANY binary data: file, mmap, network, sensor\]  
            │  
            ▼  split into 64B chunks (zero-padded tail)  
┌─────────────────────────────────────────┐  
│         scan\_buf() \[pogls\_scanner.h\]     │  
│  \- XOR-fold 64B → seed (8B fingerprint)  │  
│  \- theta\_map(seed) → ThetaCoord          │  
│    (face 0..11, edge 0..4, z 0..255)     │  
│  \- detect pre-compressed → PASSTHRU flag │  
│  \- emit ScanEntry via callback           │  
└──────────────────────┬──────────────────┘  
                       │  ScanEntry per chunk  
                       ▼  
┌─────────────────────────────────────────┐  
│   diamond\_flow / DiamondBlock pipeline   │  
│  \[pogls\_fold.h \+ geo\_diamond\_field.h\]    │  
│                                          │  
│  For each ScanEntry:                     │  
│  1\. fold\_block\_init → DiamondBlock       │  
│  2\. XOR chunk content into core.raw      │  
│  3\. diamond\_gate → drop/pass/drift       │  
│  4\. diamond\_route → MAIN/TEMP lane       │  
│  5\. diamond\_route\_update → accumulate    │  
│  6\. flow\_end check → DNA write           │  
│     (route\_addr \+ hop\_count \+ offset)     │  
└──────────────────────┬──────────────────┘  
                       │  Route-fingerprinted stream  
                       ▼  
┌─────────────────────────────────────────┐  
│  (OPTIONAL) H64Encoder → HilbertPacket64 │  
│  \[pogls\_hilbert64\_encoder.h\]             │  
│  \- 12 faces × 4 paths \= 48 positive      │  
│  \- 16 ghost slots                        │  
│  \- RGB balanced                           │  
└──────────────────────┬──────────────────┘  
                       │  HilbertCell\[64\]  
                       ▼  
┌─────────────────────────────────────────┐  
│  (OPTIONAL) GeopixelBridge → hamburger   │  
│  \[pogls\_to\_geopixel.h \+ hamburger\_encode\] │  
│  \- Seed-only tiles (4B each)              │  
│  \- Classify → auto-pipes → codec apply   │  
│  \- Write .gpx5 file                       │  
└──────────────────────┬──────────────────┘  
                       │  
                       ▼  
                .gpx5 file (or .h64p)  
\`\`\`

### 14\. Decode Path

\`\`\`  
.gpx5 file → gpx5\_open → LUT lookup (O(1) tile access)  
    → seed\_local → hilbert\_entry → dispatch → codec\_invert  
    → reconstructed YCgCo pixels → (optional) BMP write  
\`\`\`  
Or via \`PoglsStream\` (chunk-level streaming):  
\`\`\`  
ReconContext → PoglsStream → pogls\_stream\_next() → 64B chunks  
    → byte-perfect reconstruction of original data  
\`\`\`

### 15\. The Core Philosophical Insight

What the user was missing: **This is not a compressor of pixel values. It is a geometric router for arbitrary byte streams.**  
The "compression" comes from the fact that:  
1\. Each 64B chunk gets a **deterministic geometric fingerprint** (seed → ThetaCoord) based on its content  
2\. Chunks with similar content route through similar **geometric paths** on the dodecahedron torus  
3\. Only **flow boundaries** (where content changes enough to exhaust the geometric invariant bits or exceed drift threshold) trigger DNA writes  
4\. Between boundaries, chunks can be **re-derived** from their position in the flow and the accumulated route address  
The hamburger tile codec is **one consumer** layered on top — it takes the H64 geometric addresses and classifies/encodes them using traditional tile codecs. But the fundamental pipeline is **data → geometry → route → fingerprint → (optionally) encode**. The DNA records in the honeycomb slots are the compressed representation: a single route address \+ hop count can reconstruct an entire flow path through the geometric space.

# Tab 2

Now I have a comprehensive understanding. Here is the detailed analysis:  
\---

## Comprehensive Analysis: Temporal Residual Zone & Atomic Reshape

### Source Files Examined

| File | Path |  
|------|------|  
| geo\_temporal\_lut.h | \`I:\\FGLS\_new\\collection\\src\\geo\_temporal\_lut.h\` |  
| geo\_temporal\_ring.h | \`I:\\FGLS\_new\\collection\\core\\core\\geo\_temporal\_ring.h\` |  
| geo\_tring\_addr.h | \`I:\\FGLS\_new\\collection\\geopixel\\hbv\_bundle\\geo\_tring\_addr.h\` |  
| geo\_tring\_fec.h | \`I:\\FGLS\_new\\collection\\core\\core\\geo\_tring\_fec.h\` |  
| pogls\_atomic\_reshape.h | \`I:\\FGLS\_new\\collection\\active\_updates\\pogls\_atomic\_reshape.h\` |  
| pogls\_38\_temporal.h | \`I:\\FGLS\_new\\collection\\core\\pogls\_engine\\gpu\\pogls\_38\_temporal.h\` |  
| pogls\_38\_repair.h | \`I:\\FGLS\_new\\collection\\core\\pogls\_engine\\gpu\\pogls\_38\_repair.h\` |  
| geo\_diamond\_field.h | \`I:\\FGLS\_new\\collection\\core\\core\\geo\_diamond\_field.h\` |  
| pogls\_delta.h | \`I:\\FGLS\_new\\collection\\core\\core\\pogls\_delta.h\` |  
| pogls\_delta\_ext.h | \`I:\\FGLS\_new\\collection\\core\\core\\pogls\_delta\_ext.h\` |  
| pogls\_delta\_world\_b.h | \`I:\\FGLS\_new\\collection\\core\\core\\pogls\_delta\_world\_b.h\` |  
| pogls\_fold.h | \`I:\\FGLS\_new\\collection\\core\\core\\pogls\_fold.h\` |  
| fabric\_wire.h | \`I:\\FGLS\_new\\collection\\active\_updates\\fabric\_wire.h\` |  
| fabric\_wire\_drain.h | \`I:\\FGLS\_new\\collection\\active\_updates\\fabric\_wire\_drain.h\` |  
| heptagon\_fence.h | \`I:\\FGLS\_new\\collection\\active\_updates\\heptagon\_fence.h\` |  
| pogls\_rotation.h | \`I:\\FGLS\_new\\collection\\active\_updates\\pogls\_rotation.h\` |  
| pogls\_1440.h | \`I:\\FGLS\_new\\collection\\active\_updates\\pogls\_1440.h\` |  
| pogls\_hilbert64\_encoder.h | \`I:\\FGLS\_new\\collection\\geopixel\\pogls\_hilbert64\_encoder.h\` |  
| frustum\_layout\_v2.h | \`I:\\FGLS\_new\\collection\\active\_updates\\frustum\_layout\_v2.h\` |  
| geo\_whe.h | \`I:\\FGLS\_new\\collection\\core\\pogls\_engine\\twin\_core\\geo\_whe.h\` |  
| geo\_config.h | \`I:\\FGLS\_new\\collection\\core\\pogls\_engine\\twin\_core\\geo\_config.h\` |  
| gpx5\_container.h | \`I:\\FGLS\_new\\collection\\geopixel\\gpx5\_container.h\` |  
| test\_atomic\_reshape.c | \`I:\\FGLS\_new\\collection\\active\_updates\\test\_atomic\_reshape.c\` |  
\---

### 1\. What is a "Temporal Residual Zone"?

The phrase **"temporal residual zone"** is not a single defined term in the codebase, but emerges from three intersecting concepts:

#### (a) The Addressing Residual Zone (\`geo\_tring\_addr.h\`)

The address space is split into two halves:  
\`\`\`c  
\#define TRING\_LIVE\_N      3456u   // live zone:   0..3455  (144 x 24\)  
\#define TRING\_RESIDUAL\_N  3456u   // residual zone: 3456..6911  
\#define TRING\_TOTAL       6912u   // live \+ residual  
\`\`\`  
The \`TringAddr\` struct encodes (tile\_x, tile\_y, channel) into a geometric address with a \`zone\` classifier:  
\`\`\`c  
typedef struct {  
    uint16\_t addr;        // geometric address 0..6911  
    uint8\_t  pattern\_id;  // compound index 0..143  
    uint8\_t  zone;        // 0=anchor 1=mid 2=residual  
} TringAddr;  
\`\`\`  
When \`ch \== 2\` (the B carrier in the RGB model), the address routes to the residual zone by adding \`TRING\_LIVE\_N\` as an offset. The zone is determined by \`pattern\_id\`:  
\`\`\`c  
uint8\_t zone \= (pid \< 48u) ? 0u : (pid \< 96u) ? 1u : 2u;  
\`\`\`

#### (b) The Ghost/Residual Zone in Hilbert Encoding (\`pogls\_hilbert64\_encoder.h\`)

In the 64-slot Hilbert packet, slots 48-63 form the **ghost/residual zone**:  
\`\`\`c  
\#define H64\_GHOST\_SLOTS    16u   /\* slots 48..63 \= residual/ghost zone \*/  
\#define H64\_FLAG\_GHOST    0x04u  /\* residual zone cell \*/  
\`\`\`  
The commentary says: \*"This is the residual zone storage that 'has no body but has shape.'"\*  
The function \`h64\_commit\_ghost()\` writes noise deltas into the ghost zone:  
\`\`\`c  
static inline uint8\_t h64\_commit\_ghost(H64Encoder \*enc, uint8\_t face, uint64\_t delta\_seed)  
\`\`\`  
These ghost cells have \`path\_id \= 4\` and carry \`H64\_FLAG\_GHOST\`. They store the XOR residual that pairs with the wireless Hilbert transform. The ghost phase auto-advances (0..3) per commit.

#### (c) Temporal Ring — The Temporal Dimension (\`geo\_temporal\_ring.h\`)

The temporal ring is a 720-point walk over the dodecahedron's 12 pentagon faces, each composed of 5 tetrahedra:  
\`\`\`c  
\#define TEMPORAL\_WALK\_LEN  720u  
\#define TRING\_COMP(enc)    ((enc) / 60u)    // face/compound index 0..11  
\#define TRING\_CPAIR(enc)   (((enc) \+ 360u) % 720u)  // chiral partner  
\`\`\`  
The \`TRingCtx\` tracks position (head) and self-healing gaps:  
\`\`\`c  
typedef struct {  
    TRingSlot slots\[TEMPORAL\_WALK\_LEN\];  // 720 slots  
    uint16\_t  head;                      // current walk position  
    uint16\_t  missing;                   // gap count  
    uint32\_t  chunk\_count;               // total assigned  
} TRingCtx;  
\`\`\`  
Key temporal ring functions:  
\- \`tring\_tick(r)\` — advance head by 1 (position \= time)  
\- \`tring\_snap(r, enc)\` — verify arriving enc is on the path, mark present, detect gaps  
\- \`tring\_is\_valid\_next(r, enc)\` — pure predicate: is enc the expected next?  
\- \`tring\_verify\_next(r, enc)\` — check then snap (safe ordered ingress)  
\- \`tring\_pair\_pos(enc)\` — chiral route: key on pole A → instant route to pole B

#### (d) Diamond Field Temporal Routing (\`geo\_diamond\_field.h\`)

The \`diamond\_route()\` function classifies cells as MAIN (icosa, symmetric) or TEMPORAL (dodeca, asymmetric) based on the parity of their Fibonacci intersection popcount:  
\`\`\`c  
static inline int diamond\_route(const DiamondBlock \*b) {  
    uint64\_t intersect \= fold\_fibo\_intersect(b);  
    return (\_\_builtin\_popcountll(intersect) & 1u) ? 1 : 2;  
    /\* 1=MAIN(icosa), 2=TEMP(dodeca) \*/  
}  
\`\`\`  
The **temporal carry** mechanism in \`diamond\_batch\_temporal()\` allows incomplete flows to be serialized into a BridgeEntry ring (temporal ring buffer) and resumed in the next batch:  
\`\`\`c  
static inline uint32\_t diamond\_batch\_temporal(  
    DiamondBlock \*cells, uint32\_t n, uint64\_t baseline,  
    DiamondFlowCtx \*ctx,  
    BridgeEntry \*temp\_ring, uint32\_t \*temp\_head, uint32\_t \*temp\_tail)  
\`\`\`  
The \`DiamondFlowCtx\` accumulates flow state across batches:  
\`\`\`c  
typedef struct {  
    uint64\_t route\_addr;   // accumulated XOR of intersects  
    uint16\_t hop\_count;    // cells passed (not dropped)  
    uint16\_t \_pad;  
    uint32\_t drift\_acc;    // drift accumulator  
} DiamondFlowCtx;  
\`\`\`

#### (e) The \`gpx5\_container.h\` Residual Zone

In the GPX5 container format, the B plane explicitly occupies the residual zone:  
\`\`\`c  
\#define GPX5\_PLANE\_B  2u  /\* carrier B, ch=2, residual zone 3456..6911 \*/  
\`\`\`  
\---

### 2\. What is "Atomic Reshape"?

Defined in \`I:\\FGLS\_new\\collection\\active\_updates\\pogls\_atomic\_reshape.h\` (Task \#6).  
**Atomic Reshape** is the mechanism that transitions the system from one Rubik-face rotation state to the next. It is the only way to clear accumulated fence-locked shadow slots and advance the global rotation state.

#### The 4-Step Sequence

\`\`\`  
Step 1: collect(shadow)    — check threshold: locked\_count \>= 54  
Step 2: stage(new\_layout)  — compute next\_rot and lane\_group (pure, no mutation)  
Step 3: flip(rotation)     — THE ATOMIC MOMENT: 1-byte write, rotation advances  
Step 4: drain(pentagon)    — flush all 12 pentagon drains \+ clear fences  
\`\`\`

#### Sacred Invariant: "17 Does Not Exist"

\`\`\`c  
// 8  \= before\_state  (rotation\_state before flip)  
// 9  \= after\_state   (rotation\_state after flip)  
// 17 \= during        (does not exist — atomic moment)  
\`\`\`  
From the code comments:  
\> \*"8+9=17 — the boundary has no storage, no observable half-state. Like CAS (Compare-And-Swap) on CPU: external observers see either before OR after, never during."\*

#### Key Data Structures

\`\`\`c  
typedef struct {  
    uint8\_t  next\_rot;          /\* rotation after advance (0..5)        \*/  
    uint8\_t  next\_lane\_group;   /\* pogls\_lane\_group(next\_rot)          \*/  
    uint8\_t  shadow\_pending;    /\* snapshot of pending count            \*/  
    uint8\_t  drain\_count;       /\* always FGLS\_DRAIN\_COUNT \= 12        \*/  
} ReshapeStage;  
typedef enum {  
    RESHAPE\_OK           \=  0,  /\* full sequence completed             \*/  
    RESHAPE\_NOT\_READY    \=  1,  /\* threshold not met (\< 54 locked)     \*/  
    RESHAPE\_PARTIAL\_DRAIN= \-1,  /\* some drains failed to flush         \*/  
    RESHAPE\_STAGE\_FAIL   \= \-2,  /\* new layout invalid                  \*/  
} ReshapeResult;  
\`\`\`

#### Key Functions

| Function | Role |  
|----------|------|  
| \`reshape\_collect(block)\` | Check heptagon\_locked\_count \>= 54 via \`pogls\_reshape\_ready()\` |  
| \`reshape\_stage(block, \&stage)\` | Compute next rotation (pure, no state change) |  
| \`reshape\_flip(block, \&stage)\` | **Atomic moment**: advance global rotation state 1-byte write |  
| \`reshape\_drain(block)\` | Flush 12 pentagon drains \+ clear fence bits |  
| \`pogls\_atomic\_reshape(block, \&stage)\` | Full 4-step entry point |  
| \`reshape\_cycle\_complete()\` | True if rotation\_state \== 0 (full cycle done) |

#### Threshold: POGLS\_SACRED\_NEXUS \= 54

From \`fabric\_wire.h\`:  
\`\`\`c  
\#define POGLS\_SACRED\_NEXUS   54u   /\* 2 x 3^3 — Rubik stickers \*/  
\`\`\`  
54 \= minimum complete state for a valid Rubik face rotation. 6 reshapes → full Rubik cycle (0→1→2→3→4→5→0).

#### Preceding Accumulation

The \`fabric\_wire\_drain.h\` tracks pending shadow slots:  
\`\`\`c  
static inline uint8\_t fabric\_shadow\_flush\_pending(const FrustumBlock \*block)  
// counts shadow\_state entries with (OCCUPIED | FENCE) \== (OCCUPIED | FENCE)  
\`\`\`  
The \`fabric\_atomic\_reshape\_check()\` function combines this into a single call:  
\`\`\`c  
static inline bool fabric\_atomic\_reshape\_check(const FrustumBlock \*block) {  
    return pogls\_reshape\_ready(fabric\_shadow\_flush\_pending(block));  
}  
\`\`\`

#### The 1440 Cycle and Rotation Advancement

From \`pogls\_rotation.h\`, the rotation advances at a specific phase boundary:  
\`\`\`c  
// In pogls\_dispatch\_1440\_rot(idx):  
if (idx \== POGLS\_BASE\_CYCLE) {   // idx \== 720 \= phase flip 0→1  
    fabric\_rotation\_advance\_global();  
}  
\`\`\`  
The fence key ties rotation and phase to specific pentagon drains:  
\`\`\`c  
static inline uint8\_t pogls\_fence\_key(uint8\_t rot, uint8\_t phase) {  
    return (uint8\_t)((rot % POGLS\_LANE\_GROUPS) \* 2u \+ (phase & 1u));  
}  
// 12 distinct values \= 12 pentagon drains (POGLS\_TRING\_FACES)  
\`\`\`  
\---

### 3\. How Do They Relate to the Repair/Restoration Mechanism?

Repair is handled by a separate subsystem: \`pogls\_38\_repair.h\`.

#### The Scale Ladder Repair Pipeline

When an address falls outside the unit circle (geometric integrity check), the repair pipeline activates:  
\`\`\`c  
// Scale Ladder: zoom out to repair at macro level  
static const uint16\_t l38\_scale\_ladder\[L38\_SCALE\_STEPS\] \= { 162, 54, 18, 6, 2 };  
\`\`\`  
**Pipeline flow:**  
1\. **Try Rubik permutation** at current scale via \`l38\_try\_repair()\` — permutes low 20 bits of angular address and checks unit circle  
2\. **If fail → narrow scope** (scale down ÷3) — \`162 → 54 → 18 → 6 → 2\`  
3\. **If all scales fail → World Flip** — \`l38\_world\_flip\_addr()\` transforms address via PHI multiplication (A→B or B→A)  
4\. **If flip succeeds** → fold back (address restored)  
5\. **If flip also fails → RECYCLE** flag — not death, the system learns  
**Key functions:**  
\`\`\`c  
static inline int l38\_repair\_pipeline(L38EjectFrame \*ef, uint32\_t \*addr\_inout)  
static inline int l38\_repair\_cell(L38RepairCtx \*ctx, L17Lattice \*lat,  
                                  uint32\_t cell\_id, uint64\_t angular\_addr,  
                                  uint8\_t world, L38TailsCheckpoint \*tails\_out)  
\`\`\`

#### The Movement Log

Every repair step is logged in a ring buffer:  
\`\`\`c  
typedef struct \_\_attribute\_\_((packed)) {  
    uint32\_t  frame\_id;  
    uint8\_t   move;        // l38\_move\_t (DETACH, REPAIR\_TRY, REPAIR\_OK, etc.)  
    uint8\_t   world;       // 0=A, 1=B  
    uint8\_t   scale\_step;  // ladder step (0=162 .. 4=2)  
    uint8\_t   rubik\_step;  // move index 0..19  
    uint32\_t  cell\_id;     // L17 cell\_id  
} L38MoveEntry;  
\`\`\`

#### Temporal Ring Self-Healing

The temporal ring also provides self-healing via gap detection:  
\`\`\`c  
static inline uint16\_t tring\_first\_gap(const TRingCtx \*r)  // scan for missing slots  
static inline int tring\_snap(TRingCtx \*r, uint32\_t enc)    // snap to correct position  
\`\`\`

#### DiamondField Drift Detection

The diamond field monitors cell integrity:  
\`\`\`c  
// drift\_score \> 72 \= flow boundary (FLOW\_END\_DRIFT)  
static inline uint32\_t diamond\_drift\_score(const DiamondBlock \*b, uint64\_t baseline)  
\`\`\`  
\---

### 4\. How Do They Fit into the Overall Pipeline?

#### End-to-End Data Flow

\`\`\`  
\[Raw Data\]  
   │  
   ▼  
pogls\_scanner → ScanEntry stream  
   │  
   ▼  
pogls\_1440.h — 1440-cycle dispatch (idx 0..1439)  
   │  idx720 \= idx % 720  
   │  phase  \= idx / 720  (0 or 1\)  
   │  bit6   \= (trit ^ slope ^ phase ^ rot\_xor) & 1  \[Switch Gate\]  
   │  
   ▼  
fabric\_wire.h — classify layer \+ Switch Gate  
   │  trit \= idx720 % 27  
   │  slope \= idx720 % 256  
   │  
   ├── bit6 \= 0 → STORE (World A, 2^n)  
   │   └── fabric\_wire\_commit() → heptagon\_fence PATH A  
   │       └── shadow\_state\[\].OCCUPIED set  
   │  
   └── bit6 \= 1 → DRAIN (World B, 3^n)  
       └── fabric\_wire\_commit() → heptagon\_fence PATH B  
           ├── drain\_state\[\].ACTIVE | FLUSH | MERKLE set  
           └── shadow\_state\[\].FENCE locked (irreversible)  
                │  
                ▼  (accumulate fence-locked slots)  
            \[heptagon\_locked\_count\] → reshape\_ready(54)?  
                │  
                ├── NO → continue accumulating  
                │  
                └── YES → Atomic Reshape Sequence  
                    │  
                    ├── 1\. collect(block) → reshape\_ready check  
                    ├── 2\. stage(block, \&stage) → compute next\_rot  
                    ├── 3\. flip(block, \&stage) → ATOMIC: rotation\_state++  
                    │     └── 1-byte write (the "17" moment)  
                    └── 4\. drain(block) → flush 12 pentagon drains  
                          └── heptagon\_fence\_reshape\_clear()  
                              └── FENCE bits cleared, OCCUPIED preserved  
\`\`\`

#### Temporal Pipeline (Parallel Flow)

\`\`\`  
GeoData (DiamondBlock\[\] arrays)  
   │  
   ▼  
fold\_fibo\_intersect() → compute intersect cell  
   │  
   ▼  
diamond\_batch\_temporal()  
   │  ├── diamond\_ctx\_pop\_temp() — resume from temporal carry  
   │  ├── diamond\_batch\_run() — hot path encode  
   │  │   ├── diamond\_route() → classify MAIN/TEMP  
   │  │   ├── diamond\_route\_update() → XOR into route\_addr  
   │  │   ├── diamond\_drift\_score() → monitor drift  
   │  │   └── diamond\_flow\_end() → detect boundary  
   │  │       ├── FLOW\_END\_DEAD  (intersect \== 0\)  
   │  │       ├── FLOW\_END\_DRIFT (drift \> 72\)  
   │  │       └── diamond\_dna\_write() → HoneycombSlot  
   │  └── diamond\_ctx\_push\_temp() → carry to next batch  
   │  
   ▼  
geo\_temporal\_ring.h — ordering verification  
   │  tring\_tick() / tring\_verify\_next() / tring\_snap()  
   │  
   ▼  
geo\_whe.h — Wireless Hilbert Entanglement  
   │  whe\_step() → violation detection \+ tail tracking  
   │  whe\_final\_fp() → 64-bit flow fingerprint  
   │  
   ▼  
pogls\_38\_repair.h — if address out of unit circle  
   │  l38\_repair\_pipeline() → scale ladder \+ rubik \+ world flip \+ recycle  
   │  
   ▼  
pogls\_delta.h — commit to .pogls/ delta lanes  
   │  delta\_append() → delta\_commit() → merkle write  
   │  
   ▼  
\[Storage: FrustumBlock 4896B containers\]  
\`\`\`  
\---

### 5\. Critical Architecture: The Three Fence Paths

From \`heptagon\_fence.h\`, the system enforces three access paths:  
| Path | Direction | Condition | Irreversible? |  
|------|-----------|-----------|---------------|  
| PATH A | CORE/GEAR → SHADOW write | Layer OK, slot not locked | FENCE bit set on write |  
| PATH B | SHADOW → FLUSH (drain) | is\_drain=true, SECURITY layer, key match | Once FENCE set, only Atomic Reshape clears it |  
| PATH C | SHADOW → CORE direct read | **Architecturally absent** — always denied | N/A |  
\---

### 6\. Key Sacred Numbers Summary

| Number | Meaning | Where Used |  
|--------|---------|------------|  
| 144 | TE\_CYCLE, FiboClock period | \`geo\_config.h\`, \`frustum\_layout\_v2.h\` |  
| 720 | Temporal ring walk length, BASE\_CYCLE | \`geo\_temporal\_lut.h\`, \`pogls\_1440.h\` |  
| 1440 | EXT\_CYCLE \= 2 x 720 | \`pogls\_1440.h\`, \`pogls\_rotation.h\` |  
| 54 | SACRED\_NEXUS, Rubik stickers, DiamondBlock count | \`fabric\_wire.h\`, \`frustum\_layout\_v2.h\` |  
| 12 | Pentagon drains, dodeca faces | \`frustum\_layout\_v2.h\` |  
| 28 | Shadow slot count | \`frustum\_layout\_v2.h\` |  
| 3456 | FULL\_N \= 144 x 24 | \`geo\_config.h\`, \`geo\_tring\_addr.h\` |  
| 4896 | FILE\_SIZE \= 17 x 2 x 144 | \`frustum\_layout\_v2.h\` |  
\---

### 7\. Conclusion

**Temporal Residual Zone** is the ghost/negative/residual space in the Hilbert-addressed geometric memory. It exists in multiple layers:  
\- At the addressing level: channel 2 data routes to addresses 3456..6911  
\- At the encoding level: the 16 ghost slots (48-63) in the Hilbert 64-slot packet  
\- At the temporal level: the chiral partner (enc+360) in the 720-point temporal walk  
\- At the FEC level: parity data covers gaps in the temporal stream  
**Atomic Reshape** is the lockstep state transition that advances the global rotation state boundary. It is triggered when 54 shadow slots accumulate FENCE locks (the Rubik sticker threshold), and executes a 4-step collect→stage→flip→drain sequence where the flip is a 1-byte write whose during-state is unobservable (the "17 does not exist" invariant).  
Together, they form the **repair/restoration mechanism**: the temporal residual zone provides the address space for ghost/negative data that enables reconstruction (via FEC or invert derivation), while atomic reshape provides the mechanism to clear accumulated one-way fence states and advance the pipeline to its next geometric orientation. The scale-ladder repair pipeline (\`pogls\_38\_repair.h\`) handles individual address-level integrity failures, complementing the system-level reshape cycle.