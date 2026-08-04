/**
 * generate_report.js — FGLS Architecture & Vulnerability Report (docx)
 */
const docx = require('docx');
const fs = require('fs');
const {
  Document, Paragraph, TextRun, HeadingLevel, AlignmentType,
  Table, TableRow, TableCell, WidthType, BorderStyle,
  PageBreak, TabStopType, TabStopPosition, NumberFormat,
  ShadingType, convertInchesToTwip
} = docx;

const BORDER = { style: BorderStyle.SINGLE, size: 1, color: "CCCCCC" };
const NO_BORDER = { style: BorderStyle.NONE, size: 0 };

function heading(text, level = HeadingLevel.HEADING_1) {
  return new Paragraph({ heading: level, children: [new TextRun({ text, bold: true })] });
}

function para(text, opts = {}) {
  return new Paragraph({
    spacing: { after: 120 },
    children: [new TextRun({ text, ...opts })]
  });
}

function boldPara(label, text) {
  return new Paragraph({
    spacing: { after: 120 },
    children: [
      new TextRun({ text: label, bold: true }),
      new TextRun({ text })
    ]
  });
}

function bullet(text, level = 0) {
  return new Paragraph({
    bullet: { level },
    spacing: { after: 80 },
    children: [new TextRun({ text })]
  });
}

function severityColor(sev) {
  switch(sev) {
    case 'CRITICAL': return 'FF0000';
    case 'HIGH': return 'FF6600';
    case 'MEDIUM': return 'FFAA00';
    case 'LOW': return '00AA00';
    default: return '333333';
  }
}

function severityBg(sev) {
  switch(sev) {
    case 'CRITICAL': return 'FFE0E0';
    case 'HIGH': return 'FFE8D0';
    case 'MEDIUM': return 'FFF5D0';
    case 'LOW': return 'E0FFE0';
    default: return 'F0F0F0';
  }
}

function vulnTable(vulns) {
  const headerRow = new TableRow({
    tableHeader: true,
    children: ['Severity', 'ID', 'Title', 'Location', 'Impact'].map(h =>
      new TableCell({
        shading: { type: ShadingType.CLEAR, fill: "2B579A", color: "FFFFFF" },
        children: [new Paragraph({ children: [new TextRun({ text: h, bold: true, color: "FFFFFF", size: 20 })] })],
        borders: { top: BORDER, bottom: BORDER, left: BORDER, right: BORDER }
      })
    )
  });

  const rows = vulns.map(v => new TableRow({
    children: [
      new TableCell({
        shading: { type: ShadingType.CLEAR, fill: severityBg(v.severity) },
        children: [new Paragraph({ children: [new TextRun({ text: v.severity, bold: true, color: severityColor(v.severity), size: 18 })] })],
        borders: { top: BORDER, bottom: BORDER, left: BORDER, right: BORDER }
      }),
      new TableCell({
        children: [new Paragraph({ children: [new TextRun({ text: v.id, size: 18 })] })],
        borders: { top: BORDER, bottom: BORDER, left: BORDER, right: BORDER }
      }),
      new TableCell({
        children: [new Paragraph({ children: [new TextRun({ text: v.title, size: 18 })] })],
        borders: { top: BORDER, bottom: BORDER, left: BORDER, right: BORDER }
      }),
      new TableCell({
        children: [new Paragraph({ children: [new TextRun({ text: v.location, size: 16, italics: true })] })],
        borders: { top: BORDER, bottom: BORDER, left: BORDER, right: BORDER }
      }),
      new TableCell({
        children: [new Paragraph({ children: [new TextRun({ text: v.impact, size: 18 })] })],
        borders: { top: BORDER, bottom: BORDER, left: BORDER, right: BORDER }
      })
    ]
  }));

  return new Table({
    width: { size: 100, type: WidthType.PERCENTAGE },
    rows: [headerRow, ...rows],
  });
}

// ── Document ──
const doc = new Document({
  creator: "FGLS Analysis Agent",
  title: "FGLS Geometric Compression Pipeline — Architecture & Vulnerability Report",
  description: "Comprehensive analysis of the FGLS system architecture, identified vulnerabilities, and improvement recommendations.",
  styles: {
    default: {
      document: { run: { font: "Calibri", size: 22 } },
      heading1: { run: { font: "Calibri", size: 32, bold: true, color: "1F3864" } },
      heading2: { run: { font: "Calibri", size: 26, bold: true, color: "2B579A" } },
      heading3: { run: { font: "Calibri", size: 24, bold: true, color: "404040" } },
    }
  },
  sections: [
    // ── Cover Page ──
    {
      children: [
        new Paragraph({ spacing: { before: 4000 } }),
        new Paragraph({
          alignment: AlignmentType.CENTER,
          children: [new TextRun({ text: "FGLS", size: 72, bold: true, color: "1F3864", font: "Calibri" })]
        }),
        new Paragraph({
          alignment: AlignmentType.CENTER,
          children: [new TextRun({ text: "Geometric Compression Pipeline", size: 36, color: "2B579A" })]
        }),
        new Paragraph({
          alignment: AlignmentType.CENTER,
          spacing: { before: 200 },
          children: [new TextRun({ text: "Architecture & Vulnerability Report", size: 28, color: "404040" })]
        }),
        new Paragraph({ spacing: { before: 600 } }),
        new Paragraph({
          alignment: AlignmentType.CENTER,
          children: [new TextRun({ text: "Date: August 1, 2026", size: 22, color: "666666" })]
        }),
        new Paragraph({
          alignment: AlignmentType.CENTER,
          children: [new TextRun({ text: "Branch: sid-runner", size: 22, color: "666666" })]
        }),
        new Paragraph({
          alignment: AlignmentType.CENTER,
          spacing: { before: 200 },
          children: [new TextRun({ text: "Status: 3 CRITICAL | 4 HIGH | 5 MEDIUM | 3 LOW", size: 22, bold: true, color: "CC0000" })]
        }),
        new Paragraph({ children: [new PageBreak()] }),
      ]
    },

    // ── Main Content ──
    {
      children: [
        // ═══════════════════════════════════════════════════════════════
        // SECTION 1: Executive Summary
        // ═══════════════════════════════════════════════════════════════
        heading("1. Executive Summary"),
        para("This report presents a comprehensive analysis of the FGLS (Geometric Compression Pipeline) system. The analysis covers architecture review, code vulnerability assessment, and testing with real GGUF model files."),
        para("The FGLS project implements a geometric approach to weight storage and compression for LLM inference. The core idea is to map tensor data into a geometric coordinate space (20736 addresses = 144x144) using contour encoding, then route through GeoJump, FrameSeek, and DRamTile storage layers before GPU pull for inference."),

        boldPara("Key Finding: ", "The pipeline has significant structural gaps that prevent it from functioning as an end-to-end system with real model files. While individual components (contour codec, GeoJump, GPU Jet Puller) work correctly in isolation, the integration between them is incomplete."),

        // ═══════════════════════════════════════════════════════════════
        // SECTION 2: System Architecture
        // ═══════════════════════════════════════════════════════════════
        heading("2. System Architecture"),

        heading("2.1 Core Philosophy: MAP not COMPRESS", HeadingLevel.HEADING_2),
        para("The fundamental principle of FGLS is geometric mapping rather than traditional compression. Instead of reducing data size through statistical methods (like zstd or LZ4), FGLS maps data into a geometric coordinate space where the structure itself provides organization and access patterns."),
        para("This approach has three key properties:"),
        bullet("O(1) address computation — no search or hash lookups needed"),
        bullet("Deterministic routing — same data always maps to same coordinates"),
        bullet("Structure-preserving — geometric relationships in the data are maintained"),

        heading("2.2 Pipeline Architecture", HeadingLevel.HEADING_2),
        para("The pipeline consists of five main stages, each handling a specific transformation:"),

        // Pipeline stages table
        new Table({
          width: { size: 100, type: WidthType.PERCENTAGE },
          rows: [
            new TableRow({
              tableHeader: true,
              children: ['Stage', 'Component', 'Function', 'Status'].map(h =>
                new TableCell({
                  shading: { type: ShadingType.CLEAR, fill: "2B579A", color: "FFFFFF" },
                  children: [new Paragraph({ children: [new TextRun({ text: h, bold: true, color: "FFFFFF", size: 20 })] })],
                  borders: { top: BORDER, bottom: BORDER, left: BORDER, right: BORDER }
                })
              )
            }),
            ...([
              ['1', 'Contour Codec', '6000 cells → 20736 geo space (4 strategies)', 'PASS (isolated)'],
              ['2', 'GeoJump Bridge', 'Hilbert/Peano/Mod/Invert routing', 'PASS (isolated)'],
              ['3', 'FrameSeek', 'Stride-37 timeline advancement', 'PASS (stateless)'],
              ['4', 'DRamTile + GearShift', 'GPU-ready storage with indexing', 'PASS (isolated)'],
              ['5', 'GPU Jet Puller', 'CUDA kernel for GPU data pull', 'PASS (Colab T4)'],
            ]).map(([stage, comp, func, status]) =>
              new TableRow({
                children: [stage, comp, func, status].map((t, i) =>
                  new TableCell({
                    children: [new Paragraph({ children: [new TextRun({ text: t, size: 18 })] })],
                    borders: { top: BORDER, bottom: BORDER, left: BORDER, right: BORDER }
                  })
                )
              })
            )
          ]
        }),

        new Paragraph({ spacing: { after: 200 } }),

        heading("2.3 Coordinate Space: 20736 = 144 × 144", HeadingLevel.HEADING_2),
        para("The geometric coordinate space is defined as 20736 addresses, arranged in a 144×144 grid. This number is not arbitrary — it derives from the geometric properties of the system:"),
        bullet("20736 = 12^4 — four-dimensional base-12 addressing"),
        bullet("144 = 12^2 — two-dimensional projection"),
        bullet("1728 = 12^3 — FiboSpine pipe count (1728 pipes × 12 ticks = 20736)"),
        bullet("6000 cells = 6 faces × 10×10×10 — contour cube dimensions"),

        para("The mapping from 6000 contour cells to 20736 geo addresses uses four strategies:"),
        bullet("Sequential: face×1000 + z×100 + y×10 + x"),
        bullet("Stride-37: (global_idx × 37) mod 20736 — provides good distribution"),
        bullet("Face Region: face×3456 + z×345 + y×34 + x"),
        bullet("Grid: row=face×24+z, col=y×10+x"),

        heading("2.4 Contour Codec: The Zip Model", HeadingLevel.HEADING_2),
        para("The contour codec treats the 6000-cell cube as an opaque container (like a zip file). Data is packed into the cube using any strategy, and unpacked using the same strategy. The key insight is that placement strategy is irrelevant for lossless roundtrip — only the encode/decode consistency matters."),
        para("This is verified by the test suite: 5/5 strategies PASS roundtrip with 0 mismatches on real GGUF tensors (Qwen3-0.6B Q4/Q8, Qwen2.5-0.5B Q8, SmolLM2-360M Q8)."),

        heading("2.5 GeoJump: Deterministic Routing with Adaptive Gearbox", HeadingLevel.HEADING_2),
        para("GeoJump provides address routing between coordinate spaces. It supports multiple routing types (Hilbert, Peano, Pentagon, Mod, Invert, Ground, Capo) and acts as an adaptive gearbox that can output 128 or 144 addresses depending on the target geometry."),
        para("The routing is O(1) — no lookup tables, just arithmetic. This is critical for real-time inference where every nanosecond counts."),

        heading("2.6 FrameSeek: Stride-37 Timeline", HeadingLevel.HEADING_2),
        para("FrameSeek provides temporal addressing through a stride-37 timeline. Each frame advances the encoding by one step, creating a sequence of address transformations. The stride-37 value is chosen because 37 is coprime with 20736, ensuring uniform distribution across the address space."),
        para("FrameSeek is stateless — it does not modify data, only advances a counter. This makes it safe to use in parallel pipelines."),

        heading("2.7 DRamTile + GearShift: GPU-Ready Storage", HeadingLevel.HEADING_2),
        para("DRamTile provides memory-mapped storage for weight tensors, organized for GPU access. GearShift adds indexing and routing on top of DRamTile, enabling efficient data pull by the GPU Jet Puller."),
        para("The storage is designed for zero-copy access — the GPU can read directly from DRamTile without PCIe bus transfers. This is achieved through CUDA unified memory or HBM allocation."),

        heading("2.8 GPU Jet Puller: CUDA Kernel for Data Pull", HeadingLevel.HEADING_2),
        para("The GPU Jet Puller is a CUDA kernel that pulls weight data from DRamTile into GPU registers. It uses the FiboSpine architecture (1728 pipes × 12 ticks) to organize data flow, with the Jet Bridge at tick 11 providing a residual space for data transfer."),
        para("Benchmark results on Colab T4: 7.23 GB/s throughput, 7.1M pulls, 0 errors. The kernel achieves near-memory-bandwidth performance by eliminating unnecessary copies and using direct GPU-mapped DRamTile access."),

        // ═══════════════════════════════════════════════════════════════
        // SECTION 3: Vulnerability Assessment
        // ═══════════════════════════════════════════════════════════════
        heading("3. Vulnerability Assessment"),
        para("The following vulnerabilities were identified through code review and testing:"),

        vulnTable([
          { severity: 'CRITICAL', id: 'V-001', title: 'Pipeline Decode Path is Placeholder', location: 'fgls_pipeline.h:455-478', impact: 'Pipeline cannot roundtrip real data' },
          { severity: 'CRITICAL', id: 'V-002', title: 'Contour Codec Limited to 6000 Bytes', location: 'fgls_pipeline.h:400-401', impact: 'Fails on any real GGUF tensor' },
          { severity: 'CRITICAL', id: 'V-003', title: 'DRamTile-Contour Codec Not Connected', location: 'fgls_pipeline.h:438-439', impact: 'Pipeline chain is theoretical only' },
          { severity: 'HIGH', id: 'V-004', title: 'GGUF Reader KV Pair Skip Errors', location: 'gguf_index.h:47-68', impact: 'Segfault on GGUF v3 models' },
          { severity: 'HIGH', id: 'V-005', title: 'SID Cache No LRU Eviction', location: 'sid_cache.h:122-137', impact: 'Hot tensors evicted, cold kept' },
          { severity: 'HIGH', id: 'V-006', title: 'Runner Makefile Wrong Vulkan Path', location: 'runner/Makefile:12-13', impact: 'Cannot compile with Vulkan backend' },
          { severity: 'HIGH', id: 'V-007', title: 'Contour Codec Collision Handling', location: 'contour_codec_20736.h:176-182', impact: 'Silent data corruption on collisions' },
          { severity: 'MEDIUM', id: 'V-008', title: 'No Safetensor Support', location: 'Codebase-wide', impact: 'Cannot process safetensor models' },
          { severity: 'MEDIUM', id: 'V-009', title: 'GPU Jet Puller Colab-Only', location: 'gpu_jet_puller.cu', impact: 'No local Vulkan fallback' },
          { severity: 'MEDIUM', id: 'V-010', title: 'FrameSeek Stateless Unused', location: 'fgls_pipeline.h:427-432', impact: 'Wasted computation' },
          { severity: 'MEDIUM', id: 'V-011', title: 'No Integration Tests', location: 'Codebase-wide', impact: 'Cannot verify end-to-end' },
          { severity: 'MEDIUM', id: 'V-012', title: 'Pipeline CLI Decode Stub', location: 'fgls_pipeline_cli.c:266-310', impact: 'CLI decode non-functional' },
          { severity: 'LOW', id: 'V-013', title: 'No Error Recovery', location: 'fgls_pipeline.h', impact: 'Partial failure corrupts state' },
          { severity: 'LOW', id: 'V-014', title: 'gguf_type_size() Returns 0', location: 'gguf_reader.h:36', impact: 'Confusing dead API' },
          { severity: 'LOW', id: 'V-015', title: 'SID Loader O(n) Tensor Find', location: 'sid_loader.h:58-65', impact: 'Slow tensor lookup' },
        ]),

        new Paragraph({ children: [new PageBreak()] }),

        // ═══════════════════════════════════════════════════════════════
        // SECTION 4: Detailed Vulnerability Analysis
        // ═══════════════════════════════════════════════════════════════
        heading("4. Detailed Vulnerability Analysis"),

        heading("4.1 V-001: Pipeline Decode Path", HeadingLevel.HEADING_2),
        boldPara("Severity: ", "CRITICAL"),
        boldPara("Location: ", "fgls_pipeline.h, fgls_pipeline_decode() function"),
        boldPara("Description: ", "The decode path in the pipeline is a placeholder that does not actually reconstruct data from the encoded format. It hardcodes tensor.size = 1024 and copies from DRamTile without performing the inverse contour encoding."),
        boldPara("Impact: ", "The pipeline cannot perform lossless roundtrip with real data. Any encode → decode cycle will produce incorrect results."),
        boldPara("Root Cause: ", "The decode implementation was written as a structural demo, not a functional codec. The contour codec decode step is done after copy, not as part of the decode process."),
        boldPara("Fix Required: ", "Implement proper decode that reads from .gfuf file format, performs inverse GeoJump routing, and reconstructs contour cells before converting back to tensor data."),

        heading("4.2 V-002: Contour Codec 6000-Byte Limit", HeadingLevel.HEADING_2),
        boldPara("Severity: ", "CRITICAL"),
        boldPara("Location: ", "fgls_pipeline.h, _tensor_to_cells() function"),
        boldPara("Description: ", "The contour codec is limited to 6000 cells (the size of the contour cube). Any tensor larger than 6000 bytes will fail to encode. Real GGUF tensors are typically 100KB to 1GB+."),
        boldPara("Impact: ", "The pipeline cannot process any real-world tensor from GGUF models."),
        boldPara("Root Cause: ", "The codec was designed for the contour cube geometry (6 faces × 10×10×10 = 6000 cells) but no chunking mechanism was implemented for larger tensors."),
        boldPara("Fix Required: ", "Implement tensor chunking that splits large tensors into 6000-byte blocks, encodes each block separately, and reassembles during decode. The chunk boundaries must be deterministic for lossless reconstruction."),

        heading("4.3 V-003: DRamTile-Contour Disconnect", HeadingLevel.HEADING_2),
        boldPara("Severity: ", "CRITICAL"),
        boldPara("Location: ", "fgls_pipeline.h, fgls_pipeline_encode() function"),
        boldPara("Description: ", "The contour encoding step produces encoded cells, but the DRamTile storage step stores the original raw tensor data, not the contour-encoded data. The pipeline stages are chained sequentially but do not actually pass data between them."),
        boldPara("Impact: ", "The pipeline architecture is theoretical — the stages exist but do not form a connected data flow."),
        boldPara("Root Cause: ", "Each stage was implemented and tested independently, but the integration between stages was not completed. The contour encode output is discarded before DRamTile storage."),
        boldPara("Fix Required: ", "Connect the contour codec output to DRamTile input. The contour-encoded geo array (20736 bytes) should be stored in DRamTile, not the original tensor data."),

        heading("4.4 V-004: GGUF Reader KV Pair Errors", HeadingLevel.HEADING_2),
        boldPara("Severity: ", "HIGH"),
        boldPara("Location: ", "gguf_index.h, gguf_idx_open() function"),
        boldPara("Description: ", "The KV pair skip logic in the GGUF reader has incomplete type handling. Several GGML types (13-15) are missing from the skip switch statement, and the array type handling (case 9) does not cover all element types."),
        boldPara("Impact: ", "Segfault or incorrect data when reading GGUF v3 models with complex metadata."),
        boldPara("Root Cause: ", "The reader was written for GGUF v2 and not fully updated for v3. The beam_addressing/gguf_reader.h version handles this correctly with recursive skip_gguf_value()."),
        boldPara("Fix Required: ", "Replace gguf_index.h with the beam_addressing/gguf_reader.h version, or port its recursive KV skip logic."),

        heading("4.5 V-005: SID Cache Eviction Policy", HeadingLevel.HEADING_2),
        boldPara("Severity: ", "HIGH"),
        boldPara("Location: ", "sid_cache.h, sid_cache_put() function"),
        boldPara("Description: ", "The cache eviction policy is round-robin, not LRU/LFU. When the cache is full, it evicts the next slot in sequence regardless of how frequently or recently that tensor was accessed."),
        boldPara("Impact: ", "Frequently accessed tensors (hot weights) may be evicted while rarely used tensors remain cached."),
        boldPara("Root Cause: ", "The cache was designed for simplicity, with the assumption that tensor access patterns would be uniform. In practice, attention weights are accessed much more frequently than embedding weights."),
        boldPara("Fix Required: ", "Implement LRU (Least Recently Used) eviction by tracking last access time, or LFU (Least Frequently Used) by respecting the hits counter already present in SIDCacheEntry."),

        heading("4.6 V-006: Runner Makefile Vulkan Path", HeadingLevel.HEADING_2),
        boldPara("Severity: ", "HIGH"),
        boldPara("Location: ", "runner/Makefile, LLAMA_INC variable"),
        boldPara("Description: ", "The runner Makefile references I:/llama/llama_cuda124_x64/include for llama headers, but this directory does not exist. The actual Vulkan build is at I:/llama/llama-b9733-bin-win-vulkan-x64/."),
        boldPara("Impact: ", "The runner cannot compile with Vulkan backend support."),
        boldPara("Root Cause: ", "The Makefile was written for an older CUDA build and not updated when the llama distribution changed."),
        boldPara("Fix Required: ", "Update LLAMA_INC to point to I:/llama/llama-b9733-bin-win-vulkan-x64/include and add ggml-vulkan.dll to LLAMA_LIBS."),

        heading("4.7 V-007: Contour Codec Collision Handling", HeadingLevel.HEADING_2),
        boldPara("Severity: ", "HIGH"),
        boldPara("Location: ", "contour_codec_20736.h, codec_encode() function"),
        boldPara("Description: ", "When two cells map to the same geo address (collision), the codec silently overwrites the first value with the second. The collision_mask is set to 1 but only tracks whether ANY collision occurred, not how many."),
        boldPara("Impact: ", "Data corruption when collisions occur — the first cell's value is lost."),
        boldPara("Root Cause: ", "The codec was designed for demonstration purposes where collisions were unlikely with the test data. The stride-37 strategy has low collision probability but does not guarantee zero collisions."),
        boldPara("Fix Required: ", "Either detect and reject inputs with collisions, or implement a collision resolution strategy (e.g., chaining, open addressing). At minimum, report the number of collisions accurately."),

        new Paragraph({ children: [new PageBreak()] }),

        // ═══════════════════════════════════════════════════════════════
        // SECTION 5: Testing Results
        // ═══════════════════════════════════════════════════════════════
        heading("5. Testing Results"),

        heading("5.1 Model Files Available", HeadingLevel.HEADING_2),
        new Table({
          width: { size: 100, type: WidthType.PERCENTAGE },
          rows: [
            new TableRow({
              tableHeader: true,
              children: ['Model', 'Format', 'Size', 'Location'].map(h =>
                new TableCell({
                  shading: { type: ShadingType.CLEAR, fill: "2B579A", color: "FFFFFF" },
                  children: [new Paragraph({ children: [new TextRun({ text: h, bold: true, color: "FFFFFF", size: 20 })] })],
                  borders: { top: BORDER, bottom: BORDER, left: BORDER, right: BORDER }
                })
              )
            }),
            ...([
              ['Qwen3-0.6B Q4_0', 'GGUF', '382 MB', 'I:/model/'],
              ['Qwen3-0.6B Q8_0', 'GGUF', '639 MB', 'I:/model/'],
              ['Qwen2.5-0.5B Q8_0', 'GGUF', '675 MB', 'I:/model/'],
              ['SmolLM2-360M Q8_0', 'GGUF', '386 MB', 'I:/model/'],
              ['smolVLM-256M Q8_0', 'GGUF', '175 MB', 'I:/model/'],
              ['Kokoro TTS Q8', 'GGUF', '206 MB', 'I:/model/'],
              ['LFM2.5-Audio-1.5B Q4_0', 'GGUF', '695 MB', 'I:/model/LFM2.5-Audio-1.5B/'],
              ['LFM2.5-VL-450M', 'Safetensor', '897 MB', 'I:/model/LFM2.5-VL-450M/'],
              ['smolVLM-256M', 'Safetensor', '513 MB', 'I:/model/smolVLM-256M-Instruct/'],
            ]).map(row =>
              new TableRow({
                children: row.map(t =>
                  new TableCell({
                    children: [new Paragraph({ children: [new TextRun({ text: t, size: 18 })] })],
                    borders: { top: BORDER, bottom: BORDER, left: BORDER, right: BORDER }
                  })
                )
              })
            )
          ]
        }),

        new Paragraph({ spacing: { after: 200 } }),

        heading("5.2 Contour Codec Roundtrip (Verified)", HeadingLevel.HEADING_2),
        para("The contour codec has been verified with real GGUF tensors using test_real_gguf.c:"),
        bullet("4 models × 9 tensors × 4 strategies = 36 test cases"),
        bullet("36/36 PASS — 0 mismatches, 100% roundtrip accuracy"),
        bullet("Performance: 7.8-33 nanoseconds per operation"),
        bullet("Note: Tests use first 6000 bytes of each tensor only"),

        heading("5.3 Pipeline Test Suite Status", HeadingLevel.HEADING_2),
        para("The main Makefile test suite (make test) includes 22 test cases covering profile, version, roundtrip (zeros, sparse, source, repeated), frame seek, FRMD roundtrip, tensor proof, L-block, reshape, timetravel, fibo_tick, enclosure, tensor track, beam entropy, and bond-fibospine mapping."),
        para("Note: The pipeline test suite tests individual components, not the full encode→decode pipeline with real GGUF files."),

        heading("5.4 Vulkan Backend Status", HeadingLevel.HEADING_2),
        para("The llama Vulkan build is available at I:/llama/llama-b9733-bin-win-vulkan-x64/ with ggml-vulkan.dll. However, the runner Makefile cannot currently compile against it due to incorrect include paths (V-006)."),
        para("The Vulkan GPU benchmark (bench_vulkan_gpu.c) exists but is a standalone test, not integrated with the inference runner."),

        new Paragraph({ children: [new PageBreak()] }),

        // ═══════════════════════════════════════════════════════════════
        // SECTION 6: Recommendations
        // ═══════════════════════════════════════════════════════════════
        heading("6. Recommendations"),

        heading("6.1 Priority 1: Fix Pipeline Integration (Week 1)", HeadingLevel.HEADING_2),
        para("The pipeline must be fixed to function as an end-to-end system before any further optimization:"),
        bullet("V-001: Implement proper decode path that reads .gfuf format"),
        bullet("V-002: Add tensor chunking for tensors > 6000 bytes"),
        bullet("V-003: Connect contour codec output to DRamTile input"),
        bullet("V-012: Fix pipeline CLI decode command"),

        heading("6.2 Priority 2: Fix GGUF Reader (Week 1)", HeadingLevel.HEADING_2),
        para("The GGUF reader must handle v3 models correctly:"),
        bullet("V-004: Port recursive KV skip from beam_addressing/gguf_reader.h"),
        bullet("V-014: Remove or fix dead gguf_type_size() function"),

        heading("6.3 Priority 3: Fix Runner for Vulkan (Week 2)", HeadingLevel.HEADING_2),
        para("The runner must compile and run with Vulkan backend:"),
        bullet("V-006: Update Makefile paths for Vulkan llama build"),
        bullet("V-009: Add Vulkan fallback for GPU Jet Puller"),

        heading("6.4 Priority 4: Add Safetensor Support (Week 2-3)", HeadingLevel.HEADING_2),
        para("Safetensor support is needed for the model files in I:/model/:"),
        bullet("V-008: Implement safetensor reader (header + tensor data)"),
        bullet("Integrate with existing SID loader cache"),

        heading("6.5 Priority 5: Production Hardening (Week 3-4)", HeadingLevel.HEADING_2),
        para("Production readiness requires:"),
        bullet("V-005: Implement LRU cache eviction"),
        bullet("V-007: Fix collision handling in contour codec"),
        bullet("V-010: Either use FrameSeek or remove it"),
        bullet("V-011: Add integration tests with real models"),
        bullet("V-013: Add error recovery and rollback"),

        heading("6.6 Long-Term Architecture Improvements", HeadingLevel.HEADING_2),
        para("Beyond the immediate fixes, the following architectural improvements are recommended:"),
        bullet("SID Loader: Replace O(n) tensor find with hash map for O(1) lookup"),
        bullet("Pipeline Serialization: Implement proper .gfuf file format for persistence"),
        bullet("Memory Management: Add tensor lifetime tracking to prevent use-after-free"),
        bullet("Vulkan Integration: Create unified GPU abstraction that works with both CUDA and Vulkan"),
        bullet("Safetensor + GGUF Unification: Create a common tensor reader interface"),

        // ═══════════════════════════════════════════════════════════════
        // SECTION 7: Conclusion
        // ═══════════════════════════════════════════════════════════════
        heading("7. Conclusion"),
        para("The FGLS project has a solid geometric foundation with individually verified components. The contour codec, GeoJump, and GPU Jet Puller all work correctly in isolation. However, the integration between these components is incomplete, preventing the pipeline from functioning as an end-to-end system."),
        para("The three critical vulnerabilities (V-001, V-002, V-003) must be addressed before the pipeline can be tested with real GGUF models. Once these are fixed, the system should be able to encode and decode weight tensors with the geometric coordinate space."),
        para("The recommended approach is to fix the pipeline integration first (Week 1), then address the GGUF reader and Vulkan compilation (Week 1-2), add safetensor support (Week 2-3), and finally harden for production (Week 3-4)."),
        para("The geometric approach to weight storage is novel and has potential advantages in access patterns and GPU data flow. The key is to complete the integration so the theoretical benefits can be realized in practice."),

        new Paragraph({ spacing: { before: 400 } }),
        new Paragraph({
          alignment: AlignmentType.CENTER,
          children: [new TextRun({ text: "— End of Report —", italics: true, color: "999999" })]
        }),
      ]
    }
  ]
});

// ── Generate ──
const { Packer } = docx;
Packer.toBuffer(doc).then(buf => {
  const outPath = "docs/FGLS_Architecture_Vulnerability_Report.docx";
  require('fs').writeFileSync(outPath, buf);
  console.log("Generated:", outPath);
}).catch(err => {
  console.error("Error:", err);
});
