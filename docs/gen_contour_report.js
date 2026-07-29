// gen_report.js
// Generate Contour Mask DOCX report
const docx = require("docx");
const fs = require("fs");

const {
  Document, Paragraph, TextRun, HeadingLevel, Table, TableRow, TableCell,
  WidthType, AlignmentType, BorderStyle, ShadingType, PageBreak
} = docx;

// Colors
const DARK = "1a1a2e";
const ACCENT = "16213e";
const LIGHT = "e8e8e8";
const WHITE = "ffffff";

function heading(text, level) {
  return new Paragraph({ heading: level, children: [new TextRun({ text, bold: true })] });
}

function para(text, opts = {}) {
  return new Paragraph({
    spacing: { after: 120 },
    children: [new TextRun({ text, size: 22, ...opts })]
  });
}

function bold(text) { return para(text, { bold: true }); }

function bullet(text) {
  return new Paragraph({
    bullet: { level: 0 },
    children: [new TextRun({ text, size: 22 })]
  });
}

function code(text) {
  return new Paragraph({
    spacing: { after: 60 },
    children: [new TextRun({ text, font: "Courier New", size: 20 })]
  });
}

function makeCell(text, opts = {}) {
  return new TableCell({
    width: { size: opts.width || 2000, type: WidthType.DXA },
    shading: opts.header ? { fill: ACCENT, type: ShadingType.CLEAR } : undefined,
    children: [new Paragraph({
      children: [new TextRun({
        text,
        size: 20,
        bold: opts.header || false,
        color: opts.header ? WHITE : undefined
      })]
    })]
  });
}

function makeRow(cells, header) {
  return new TableRow({
    children: cells.map((c, i) => makeCell(c, { header, width: i === 0 ? 3000 : 1500 }))
  });
}

const children = [
  // Title page
  new Paragraph({ spacing: { before: 4000 } }),
  new Paragraph({
    alignment: AlignmentType.CENTER,
    children: [new TextRun({ text: "Contour Mask", size: 56, bold: true, color: ACCENT })]
  }),
  new Paragraph({
    alignment: AlignmentType.CENTER,
    children: [new TextRun({ text: "3-Directional Profile Measurement System", size: 28, color: "666666" })]
  }),
  new Paragraph({ spacing: { before: 400 } }),
  new Paragraph({
    alignment: AlignmentType.CENTER,
    children: [new TextRun({ text: "FGLS Geometric Computing — Technical Report", size: 24, color: "999999" })]
  }),
  new Paragraph({
    alignment: AlignmentType.CENTER,
    children: [new TextRun({ text: "July 30, 2026", size: 22, color: "999999" })]
  }),
  new Paragraph({ children: [new PageBreak()] }),

  // 1. Executive Summary
  heading("1. Executive Summary", HeadingLevel.HEADING_1),
  para("Contour Mask is a geometric observation tool that measures weight tensor structure through independent directional profiles. Inspired by the contour gauge (profile gauge) — a tool with thin pins that conform to an object's shape — Contour Mask creates multiple \"views\" of the same data by filtering through non-overlapping mask patterns."),
  para("Key results:"),
  bullet("Lossless: 600/600 exact reconstruction"),
  bullet("Cross-mask correlation: 0.0002 average — masks see genuinely different data"),
  bullet("Storage: 1.0x (partition, not compression)"),
  bullet("Directions: A, C, E (opposite pairs B, D, F cancel)"),

  // 2. Concept
  heading("2. Concept: Contour Gauge for Data", HeadingLevel.HEADING_1),
  para("A contour gauge (profile gauge) is a measuring tool with many thin pins arranged in a line. When pressed against an object, each pin moves independently to match the surface, creating a copy of the profile."),
  para("Contour Mask applies this concept to data:"),
  bullet("Mask = grid of holes (stencil pattern)"),
  bullet("Data placed on mask → only values through holes are visible"),
  bullet("Pins show what's visible → contour = shape of data through mask"),
  bullet("Multiple non-overlapping masks partition the grid, covering all cells exactly once"),

  // 3. Direction Rules
  heading("3. Direction Configuration (CRITICAL)", HeadingLevel.HEADING_1),
  para("The 6 faces of a cube are named A through F. The critical rule:"),
  bold("Opposite directions CANCEL when averaged or inverted."),
  para("A-B, C-D, E-F are opposite pairs. Using both members of a pair causes information loss — they pull against each other."),
  para("Solution: only ONE direction per pair is active:"),
  bullet("Active: A, C, E"),
  bullet("NOT used: B, D, F (would cancel A, C, E)"),
  bullet("Adjacent pairs complement: A+C, C+E, E+A"),
  bullet("NEVER pair: A+B, C+D, E+F → these CANCEL"),

  new Table({
    width: { size: 9000, type: WidthType.DXA },
    rows: [
      makeRow(["Relationship", "Pairs", "Effect"], true),
      makeRow(["Opposite (CANCEL)", "A↔B, C↔D, E↔F", "Pull against each other"]),
      makeRow(["Adjacent (complement)", "A+C, C+E, E+A", "Different views, same data"]),
      makeRow(["Active directions", "A, C, E", "One per axis"]),
    ]
  }),

  // 4. Architecture
  heading("4. Architecture", HeadingLevel.HEADING_1),
  para("The mask pattern uses modular arithmetic to partition the grid:"),
  code("mask_id = (x + y + z) % N_MASKS"),
  para("For N_MASKS=3 on a 10×10×6 grid (600 cells):"),
  bullet("Mask 0: 200 pins (cells where (x+y+z) % 3 == 0)"),
  bullet("Mask 1: 200 pins (cells where (x+y+z) % 3 == 1)"),
  bullet("Mask 2: 200 pins (cells where (x+y+z) % 3 == 2)"),
  bullet("Total: 600 pins = 600 cells (1.0x storage, lossless)"),

  // 5. Results
  heading("5. Results", HeadingLevel.HEADING_1),

  heading("5.1 Single Tensor (blk.27.ffn_up.weight)", HeadingLevel.HEADING_2),
  new Table({
    width: { size: 9000, type: WidthType.DXA },
    rows: [
      makeRow(["Metric", "Value"], true),
      makeRow(["Lossless", "600/600 (100%)"]),
      makeRow(["Storage", "1.0x"]),
      makeRow(["Mask 0 entropy", "6.631 bits"]),
      makeRow(["Mask 1 entropy", "6.677 bits"]),
      makeRow(["Mask 2 entropy", "6.682 bits"]),
      makeRow(["Cross-mask correlation", "0.04 - 0.16 (LOW)"]),
    ]
  }),

  heading("5.2 Full Model Scan (Qwen3-0.6B-Q8_0)", HeadingLevel.HEADING_2),
  para("197 Q8_0 tensors scanned:"),
  new Table({
    width: { size: 9000, type: WidthType.DXA },
    rows: [
      makeRow(["Metric", "Value"], true),
      makeRow(["Tensors scanned", "197"]),
      makeRow(["Avg cross-mask 0-1", "0.0002"]),
      makeRow(["Avg cross-mask 0-2", "0.0006"]),
      makeRow(["Avg cross-mask 1-2", "-0.0012"]),
      makeRow(["Avg entropy", "6.686 - 6.697 bits"]),
      makeRow(["Interpretation", "LOW correlation = masks see different data"]),
    ]
  }),

  heading("5.3 Most Viewable Tensors", HeadingLevel.HEADING_2),
  para("Tensors with lowest average cross-mask correlation (most structurally diverse views):"),
  new Table({
    width: { size: 9000, type: WidthType.DXA },
    rows: [
      makeRow(["Rank", "Tensor", "Avg Correlation"], true),
      makeRow(["1", "blk.14.ffn_up.weight", "-0.1577"]),
      makeRow(["2", "blk.18.attn_q.weight", "-0.0976"]),
      makeRow(["3", "blk.1.ffn_up.weight", "-0.0975"]),
      makeRow(["4", "blk.8.ffn_down.weight", "-0.0891"]),
      makeRow(["5", "blk.2.attn_v.weight", "-0.0889"]),
    ]
  }),

  // 6. Key Findings
  heading("6. Key Findings", HeadingLevel.HEADING_1),
  bullet("Opposite directions CANCEL — must declare active directions upfront"),
  bullet("Adjacent pairs complement — genuinely different views of same data"),
  bullet("Real model weights have LOW cross-mask correlation (0.0002 avg)"),
  bullet("Synthetic data has HIGH correlation (0.79-0.86) — spatial pattern dominates"),
  bullet("Mask filters real data — different masks see different structure"),
  bullet("Storage is 1.0x — partition, not compression"),
  bullet("Lossless reconstruction from mask profiles"),

  // 7. Files
  heading("7. Implementation Files", HeadingLevel.HEADING_1),
  new Table({
    width: { size: 9000, type: WidthType.DXA },
    rows: [
      makeRow(["File", "Purpose", "Status"], true),
      makeRow(["contour_mask_v2.c", "Encoder (mask filters data)", "2/2 PASS"]),
      makeRow(["contour_analyze.c", "Adjacent pair analysis", "Verified"]),
      makeRow(["contour_model_scan.c", "Full model scan (197 tensors)", "Verified"]),
      makeRow(["contour_mask.c", "V1 basic (superseded)", "Superseded"]),
    ]
  }),

  // 8. Next Steps
  heading("8. Next Steps", HeadingLevel.HEADING_1),
  bullet("GPU acceleration — mask filtering is parallel per cell"),
  bullet("Connect to inference pipeline — contour mask → weight selection"),
  bullet("Multi-model comparison — Qwen vs LLaMA vs Mistral"),
  bullet("Larger grids — test 100×100×100 on T4 GPU"),
  bullet("Integration with GPU Jet Puller for bandwidth measurement"),
];

const doc = new Document({
  sections: [{
    properties: {
      page: {
        margin: { top: 1440, right: 1440, bottom: 1440, left: 1440 }
      }
    },
    children
  }]
});

docx.Packer.toBuffer(doc).then(buf => {
  fs.writeFileSync("I:/FGLS_new/docs/contour-mask-report.docx", buf);
  console.log("Written: docs/contour-mask-report.docx (" + buf.length + " bytes)");
});
