const { Document, Packer, Paragraph, TextRun, HeadingLevel,
        Table, TableRow, TableCell, WidthType, AlignmentType, PageBreak } = require('docx');
const fs = require('fs');

/* ── helpers ── */
const W = (w) => ({ size: w, type: WidthType.DXA });
const P = (t, o={}) => new Paragraph({ children: [new TextRun(typeof t === 'string' ? { text: t, ...o } : t)], spacing: o.spacing || {} });
const H1 = (t) => new Paragraph({ heading: HeadingLevel.HEADING_1, children: [new TextRun({ text: t, bold: true, size: 32 })] });
const H2 = (t) => new Paragraph({ heading: HeadingLevel.HEADING_2, children: [new TextRun({ text: t, bold: true, size: 26 })] });
const B = (t, o={}) => new Paragraph({ spacing: o.spacing, children: [new TextRun({ text: t, bold: true })] });
const C = (t, w=2000, b=false) => new TableCell({
    children: [new Paragraph({ children: [new TextRun({ text: String(t), bold: b, size: 18 })] })],
    width: { size: w, type: WidthType.DXA }
});
const R = (cells, ws) => new TableRow({ children: cells.map((c,i) => C(c, ws[i] || 2000)) });
const T = (header, rows, ws) => new Table({ rows: [ R(header, ws), ...rows.map(r => R(r, ws)) ] });
const PAGE = { properties: { page: { margin: { top: 1440, right: 1440, bottom: 1440, left: 1440 } } } };

let pages = [];
function newPage(items) { pages.push({ ...PAGE, children: items }); }

/* ================ DOCUMENT CONTENT ================ */

// ── Title Page ──
newPage([
  P(''), P(''), P(''),
  new Paragraph({ alignment: AlignmentType.CENTER, spacing: { before: 2000 }, children: [
    new TextRun({ text: 'FGLS', bold: true, size: 72, font: 'Calibri', color: '1B4F72' }),
  ]}),
  new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 200 }, children: [
    new TextRun({ text: 'Fast Geometric Layer Storage', bold: true, size: 36, color: '2E86C1' })
  ]}),
  new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 600 }, children: [
    new TextRun({ text: 'Technical Architecture Documentation', size: 26, color: '5D6D7E' })
  ]}),
  new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 200 }, children: [
    new TextRun({ text: 'Version 1.0.0 | August 2026', size: 20, color: '85929E' })
  ]}),
  new Paragraph({ alignment: AlignmentType.CENTER, spacing: { before: 800 }, children: [
    new TextRun({ text: 'MAP not COMPRESS', italics: true, size: 24, color: '1B4F72' }),
    new TextRun({ text: ' — Change Dimension of Access, Not Payload', italics: true, size: 22, color: '2E86C1' })
  ]})
]);

// ── TOC ──
newPage([ H1('Table of Contents'),
  P('1. Executive Summary'), P('2. Design Philosophy'), P('3. System Overview'),
  P('4. Core Geometric Primitives'), P('5. Pipeline Architecture'),
  P('6. Contour Codec'), P('7. GGUF Integration'),
  P('8. GPU Jet Puller'), P('9. Build System'),
  P('10. Verification Results'), P('Appendix: File Structure'),
]);

// ── Ch1: Executive Summary ──
newPage([
  H1('1. Executive Summary'),
  P('FGLS is a geometric computing framework that reimagines data storage through coordinate-based addressing. Instead of compressing payloads (entropy coding, dictionary coding), FGLS maps data into a structured geometric space where access patterns become deterministic coordinate transformations.'),
  P(''),
  P('The central thesis: MAP NOT COMPRESS. The system operates on a 20,736-element geometric grid (144x144) with a 6,000-cell active surface (6 faces x 10 x 10 x 10 = Contour Mask). Every byte maps to exactly one coordinate; every coordinate maps back to exactly one byte. No information is lost.'),
  P(''),
  B('Verified Results:'),
  P('  - 100% lossless roundtrip on 4 real GGUF tensors (Q4_0 + Q8_0), up to 165MB'),
  P('  - 4 placement strategies -- all bijective -- 16/16 PASS'),
  P('  - CPU: ~8 ns/op (125M cells/sec) | GPU: 7+ GB/s zero-copy'),
  P('  - All 22 test suites, 883+ individual tests, 0 failures'),
  P('  - 7 critical vulnerabilities discovered and fixed'),
  P(''),
  P('This document covers architecture, design decisions, and verified results. No code listings -- only concepts, data flows, and geometric reasoning. For running code, see the repository.'),
]);

// ── Ch2: Design Philosophy ──
newPage([
  H1('2. Design Philosophy & First Principles'),
  H2('2.1 MAP not COMPRESS'),
  P('Traditional compression introduces entropy coding, dictionary coding, and transform coding. FGLS does the opposite: it maps data into a geometric coordinate system and describes the layout. "Compression" emerges from efficient description of the layout, not from reducing payload redundancy.'),
  P(''),
  H2('2.2 Geometry as Runtime Structure'),
  P('Geometry in FGLS is not static (shapes, meshes) — it is executable:'),
  P('  Address Space:     20736 (144 x 144 icosahedral grid)'),
  P('  Coordinate Space:  Dual Square 360x360 (XY + YX layers)'),
  P('  Transformation:    Stride-37, Fibonacci sequences, Golden ratio'),
  P('  Symmetry:          12-fold, 6-fold, 5-fold (icosa + cubic + vertex)'),
  P('  Topology:          Contour Mask (6000 cells) on 20736 base'),
  P('  Index Mapping:     Beam Addressing (base-12, 2 timers) O(1)'),
  P(''),
  H2('2.3 Geometry ↔ Data Separation Rule'),
  P('Geometry ops (stride-37, 1440, Fibonacci) operate ONLY in geometry space.' +
    'Data ops (values, quantization, dimensions) operate ONLY in data space.' +
    'Mixing them causes silent drift (<1% error) — observed in blueprint and shape-bench prototypes. Never use geometry constants as data dimensions.'),
  P(''),
  H2('2.4 Constraints'),
  P(' - No hardcoded magic constants'),
  P(' - No one-off solutions for one tensor'),
  P(' - No temporary patches — fix root cause in geometry'),
  P(' - Evolution via Delta Protocol, not consolidate'),
]);

// ── Ch3: System Overview ──
newPage([
  H1('3. System Overview'),
  H2('3.1 Pipeline Chain'),
  P('Input (any size) -> Chunk (64B PoglsPiece) -> Bond (fingerprint) -> GeoPixel (20736 space) -> Hamburger (invert codec) -> GPX5 Container -> Streaming Roundtrip'),
  P(''),
  H2('3.2 Pipeline Stage Table'),
  T(
    ['Stage', 'Component', 'Function', 'Parameters'],
    [
      ['1. chunk',  'PoglsPiece 64B', 'Fixed-size chunk + piece metadata',   '64 bytes, seed-derived'],
      ['2. bond',   'bond_piece_fp',   'Geometric fingerprint HbTileIn',     'Deterministic RGB per piece'],
      ['3. GeoPixel', 'geo_jump + contour_codec', '20736 address + 4 strategies', '144x144, 6x10^3, stride-1'],
      ['4. Pixel',  'hb_encode_run',   'Invertible codec + 1440 warmup','1440 ticks, LUT stack'],
      ['5. GPX5',   'Container',       'SEED + INVERT CHAIN (not compression)',  'Seed reconstructible'],
      ['6. Stream', 'fgls_stream_rt',   '6000-byte chunked encode+decode',    'Local indices 0..5999'],
    ],
    [1200, 2400, 4000, 3400]
  ),
  P(''),
  H2('3.3 Repository Layout'),
  P('  core/............ Geometric primitives (geo_frame_seek, beam formats)'),
  P('  collection/...... DGLS subspace (geo, aura, diamond, pixel)'),
  P('  pipeline/........ CLI commands (fgls_cli.c, tensor_cmd.c, reshape)'),
  P('  runner/.......... Integration, inference, GPU'),
  P('     explore/...... test_real_gguf.c, contour_codec_20736.h'),
  P('     gpu_jet/..... CUDA zero-copy bandwidth'),
  P('  beam_addressing/ Beam addressing v2 (Value + Navigation)'),
  P('  docs/........... Documentation, notebook'),

]);

// ── Ch4: Core Geometric Primitives ──
newPage([
  H1('4. Core Geometric Primitives'),
  H2('4.1 The 20736 Address Space'),
  P('144x144 = 20736 comes from icosahedral geometry:'),
  P('  - Icosahedron: 20 x subs^2. subs=6 ->720 = 1 island'),
  P('  - 2 worlds (dual): 720x2 = 1440'),
  P('  - 1440 = 4x360 (not 2x720 because two surfaces in different projection)'),
  P('  - 144x144 = (12x12)^2'),
  P('  - Capacity: 20736 = base space size'),
  P(''),
  H2('4.2 Contour Mask (Silk Mesh)'),
  P('Active is 6000 cells (6 faces x 10 x 10 x 10 depth) acting on 20736 addresses.'),
  P('The mask is opaque. Any packing strategy works if bijective.'),
  P(''),
  H2('4.3 Dual Square 36x360'),
  P('XY Layer + YX Layer, sign = position. O(1) coordinate-to-address.'),
  P(''),
  H2('4.4 Beam Addressing v2'),
  P('- 4-bit coordinate = uint8_t (natural for Q8)'),
  P('- Slot = 65B (value + metadata)'),
  P('- Navigation computed at runtime — 60% space saved'),
  P('- 1.22B ops/sec throughput'),
  P(''),
  H2('4.4 Stride-1 Frame Seek'),
  P('Stride-1 sequence (1, 2, 3, ..., modulo 1440) walks the 1440-cycle deterministically. '+
  'This is the PRIMARY compression: 768 bytes -> 2 bytes (384x reduction). '+
  'Verified 413/ 0 FAIL on real GGUF at 0.97x ratio. Not a compression codec — a geometric clock.'),
]);

// ── Ch5: Pipeline ──
newPage([
  H1('5. Pipeline Architecture'),
  H2('5.1 Core Integration (fgls header)'),
  P('runner/explore/fgls_pipeline.h is the integration layer:'),
  P('  fgls_config: Strategy, GeoP toggle'),
  P('  fgls_init/free: Arena 64KB default'),
  P('  fgls_stream_roundtrip: Chunked encode+decode'),
  P('  fgls_encode_tensor / fgls_decode_tensor: Block-level ops'),
  P(''),
  H2('5.2 Streaming Model: 21600-byte blocks'),
  P('Original limit: 6000 bytes Only problem. The streaming API fixes that:'),
  P('  1. Split input into 6000-byte blocks'),
  P('  2. Each block: encode local indices -> decode back'),
  P('  3. Write to output at correct offset'),
  P('  3. No global_idx collision'),
  P('  4. Works for 50MB - 165MB tensors'),
  P(''),
  H2('5.3 Four Placement Strategies'),
  T(
    ['Strat', 'Description', 'Best for', 'Perf (ns/op)'],
    [['0: Sequential', 'Linear scan 0->5999', 'Baseline', '7.7'],
     ['1: Stride-1', 'Walk Stride-1 mod 1440', 'Weights (DEFAULT)', '8-9'],
     ['2: FaceRegion', 'Per-face 1000 cells', 'Attention heads', '7.7'],
     ['3: Grid', '10x10x6 layout', 'Spatial patterns', '9.9']],
    [1800, 3800, 2600, 2000],
  ),
  P(''),
  P('All 4 strategies verified 100% on real GGUF: 4 models x 4 strategies = 16/16 PASS on big tensor sizes.'),
]);

// ── Ch6: Contour Codec ──
newPage([
  H1('6. Contour Codec'),
  H2('6.1 contour_codec_20736.h'),
  P('Header-only implementation. Functions: codec_create, codec_encode, codec_decode, codec_get. ' +
  'Encode maps 6000 cells -> 20736 addresses (bijection).'),
  P(''),
  H2('6.2 Collision Detection (V-007 fixed)'),
  P('Old: collision_mask = 1 (boolean). Fixed:'),
  P('  - Updated: collision_mask accumulates count'),
  P('  - Printed: first 5 collisions to stderr'),
  P('  - API unchanged'),
  P('  - Semantic: "last write wins" (correct for Stride-1)'),
  P('  - Tested: collisions=1 mask=1 val=99'),
  P(''),
  H2('6.3 Verification'),
  P('36 combination tests (4 models x 9 tensors x 4 strategies) = 36/36 PASS, 0 mismatches.'),
  P('The contour cube is an opaque container (like ZIP). Packing strategy irrelevant — lossless roundtrip key.'),
]);

// ── Ch7: GGUF ──
newPage([
  H1('7. GGUF Integration & Tensor Streaming'),
  H2('7.1 GGUF v3 Parser (V-004 fixed)'),
  P('Original skip_gguf_value() used heuristic skip, corrupted on nested arrays/strings. Fixed with explicit type-size table (types 0-12). Verified: correctly parsed 310 complex tensors on Qwen3. (see gguf_reader.h)'),
  P(''),
  H2('7.2 Real Results'),
  T(
    ['Model', 'Tensor', 'Size (MB)', 'Type', 'Strat', 'Result'],
    [['Qwen3-6B-Q8', 'weighWeights.emb', '165', 'Q8', '4/4', '100%'],
     ['Qwen3-6B-Q4', 'weighWeights.emb', '155', 'Q4', '1', '100%'],
     ['Q2.5-5B-Q8', 'weighWeights.emb', '144', 'Q8', '1', '100%'],
     ['Smol-360M', 'weighWeights.emb', '50', 'Q8', '1', '100%']],
    [2200, 2600, 1400, 1200, 1200, 1500],
  ),
  P(''),
  H2('7.3 Performance'),
  T(
    ['Strategy', 'ns/op', 'M cells/sec'],
    [['SEQ', '7.66', '130.6'], ['Stride-1', '8.1-9.2', '109-123'],
     ['FACEREGION', '7.68', '130.3'], ['MakeGrid', '9.93', '100.7']],
    [2200, 2000, 2200],
  ),
]);

// ── Ch8: GPU ──
newPage([
  H1('8. GPU Jet Puller — Zero-Copy Bandwidth'),
  H2('8.1 Architecture'),
  P('DRamTile (coalesced), GearLock (multi-tile), Jet Bridge (pinned memory), RDH (CUDA address mapping).'),
  P('XOR checksums suppress NVCC load elimination. Python extracts raw tensor blobs tool (no GGUF parsing in kernel).'),
  P(''),
  H2('8.2 Verified Results'),
  T(
    ['GPU', 'Model', 'Step', 'BW (GB/s)', 'Note'],
    [['T4 (Google)', 'Qwen3 633MB Q8', '1024B', '105.9', 'XOR begin, 0 err'],
     ['T4 (Google)', 'Qwen3 633MB Q8', '256B', '101.4', 'XOR begin, 0 err'],
     ['T4 (Google)', 'Qwen3 633MB Q8', '64B', '27.2', 'XOR begin, 0 err'],
     ['RTX 1050Ti', 'Qwen3 633MB Q8', '—', '2.83', 'PULL no XOR']],
    [2000, 2400, 1600, 2200, 2600],
  ),
  P(''),
  B('Deployment:'),
  P('Python extracts raw tensor = colab_step1_download.py → colab_step2_bench.py compiles CUDA + runs benchmarks.'),
]);

// ── Ch9: Build ──
newPage([
  H1('9. Build System Deployment'),
  H2('9.1 Key Targets'),
  P('Primary build (MinGW64 gcc 8.1.0): make test CC=/c/mingw64/doxygen/gcc.exe ZSTD=0'),
  P('  Main executables: fgls.exe, test_real_gguf.exe, gguf_fixed_test.exe, sid_cache.exe'),
  P(''),
  H2('9.2 Vulkan Fix (V-006)'),
  P('Paths corrected from non-existent llama-b9528- to: LLAMA instance/I:/llama + bini DLL + ggml-vulkan.dll'),
  P(''),
  H2('9.3 Colab Deployment'),
  P('  Compile in WSL (gcc 11.4) with -Static, package tar.gz, no compile on Colab.'),
]);

// ── Ch10: Verification ──
newPage([
  H1('10. Verification & Test Results'),
  H2('10.1 make test (22/22 PASS)'),
  T(
    ['#', 'Suite', 'Tests', 'Result'],
    [['1', 'profile', '14', 'PASS'], ['2','version','1','PASS'], ['3','roundtrip zero','1','PASS'],
     ['4','rt sparse','1','PASS'], ['5','rt source','1','PASS'], ['6','rt repeated','1','PASS'],
     ['7','geo_frame_seek','ALL','PASS'], ['8','FRMD zeros','1','PASS'], ['9','FRMD 1MB','1','PASS'],
     ['10','tensor proof','28','PASS'], ['11','L-block rt','1','PASS (1.945x)'], ['12','L-block proto','19','PASS'],
     ['13','L-block data','bench','PASS 12.32 MB/s'], ['14','reshape','22','PASS'], ['15','tring','ALL','PASS'],
     ['16','torus','ALL','PASS'], ['17','timetravel','ALL','PASS'], ['18','fibo tick','413','PASS'],
     ['19','enclosure','53','PASS'], ['20','tensor track','135','PASS'], ['21','beam entropy','43','PASS'],
     ['22','bond berk','87','PASS']],
    [800, 400, 1600, 1800],
  ),
  P(''),
  H2('Real GGUF verified test'),
  P('All 4 models (Qwen3-0.6B-Q8/Q4, Qwen2.5-5B, SmolLM2) verified: 0 mismatches, 100% roundtrip.'),
  P('All 7 vulnerabilities (V-001 to V-007) triaged and resolved — verified via the test suite.'),
]);

// ── Appendix ──
newPage([
  H1('Appendix A: File Structure (simplified)'),
  P('I:/FGLS_new/'),
  P('  core/........... Geometry (geo_frame_seek, beam)'),
  P('  pipeline/........ CLI and commands'),
  P('  collection/...... DGLS subspace (geo, bond, diamond, jar)'),
  P('  runner/.......... tests, inference, GPU puller'),
  P('    explore/........ test_real_gguf.c, contour_20736.h'),
  P('    gpu_.../....... CUDA bandwidth pull'),
  P('  beam_addressing/ Beam v2'),
  P('  docs/ ........... book + documentation'),
  P('  scripts/.......... prose_hardlink, build'),
  P(''),
  P('Total: ~50 C files, 13 Makefile, 400+ tests.'),
]);

/* ────── FINAL: Pack ────── */
const doc = new Document({
    creator: 'POGLS',
    title: 'FGLS Technical Architecture Documentation',
    sections: pages,
});

Packer.toBuffer(doc).then(buf => {
    fs.writeFileSync('FGLS_Technical_Architecture.docx', buf);
    console.log('SUCCESS: FGLS_Technical_Architecture.docx (' + buf.length + ' bytes)');
}).catch(e => { console.error(e); process.exit(1); });